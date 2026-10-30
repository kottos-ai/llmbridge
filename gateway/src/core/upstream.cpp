// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The upstream leg on both backends: opening, pooling and retrying a connection, the
// response and what a venue said in it, and where a request goes after a failure.

#include "engine.hpp"

#include "core/io.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"
#include "net/socket_util.hpp"
#include "request.hpp"
#include "response.hpp"
#include "scan.hpp"
#include "stream.hpp"

#include <cstring>
#include <string_view>

namespace llmbridge::detail
{

    // What the provider said about its own limits, kept from the response head.
    // Shared by both backends and by the streaming and non-streaming paths, so the
    // four cannot disagree about what a refusal meant.
    void Engine::note_quota(ClientConn* client, const net::http::ResponseHead& h,
                            std::string_view head) noexcept
    {
        client->req.f.quota_exhausted = static_cast<uint8_t>(h.quota_exhausted);
        client->req.f.retry_after_s = h.retry_after_s;
        // Bedrock states the tier it served only here, and an earlier attempt's must not
        // stand for this one; a body's own is read after, when this is empty.
        client->req.f.served_tier.set(head.substr(h.bedrock_tier_at, h.bedrock_tier_len));
        client->req.f.served_latency.set(head.substr(h.bedrock_latency_at, h.bedrock_latency_len));
    }

    // Bedrock is first: a Claude-via-Bedrock response carries x-amzn and
    // Anthropic's own id together, and the AWS one is what CloudTrail indexes.
    void Engine::note_venue_req_id(ClientConn* client, std::string_view head) noexcept
    {
        client->req.f.venue_req_id.set({});
        static constexpr std::string_view kNames[] = {"x-amzn-requestid", "request-id",
                                                      "x-request-id"};
        for (const std::string_view name : kNames)
        {
            const std::string_view v = net::http::find_header(head, name);
            if (v.empty()) continue;
            client->req.f.venue_req_id.set(v);
            return;
        }
    }

    void Engine::note_upstream_rtt(ClientConn* client, const UpstreamConn* u) noexcept
    {
        // At the head, when the venue has acknowledged the whole request, and on the
        // socket under TLS. One getsockopt, after t4 is stamped so it is not inside it.
        const net::TcpRtt rtt = net::tcp_rtt(u->fd);
        client->req.f.upstream_srtt_us = rtt.srtt_us;
        client->req.f.upstream_min_rtt_us = rtt.min_us;
    }

    void Engine::note_upstream_error(ClientConn* client, const net::http::ResponseHead& h,
                                      std::string_view body) noexcept
    {
        client->req.f.upstream_error.set({});
        // 0 is a head we never framed, and a 3xx is not the venue naming a failure.
        if (h.status < 400) return;
        client->req.f.upstream_error.set(scan_error_type(body));
    }

    void Engine::note_reply(const ClientConn* client, int status, bool streamed) const noexcept
    {
        if (!_policy) return;
        const ReqState& f = client->req.f;
        _policy->on_reply(ReplyFacts{.upstream_index = client->upstream_slot, .status = status,
                                     .attempt = f.failover_attempts, .tag = f.policy_tag,
                                     .seq = f.req_seq, .streamed = streamed});
    }

    bool Engine::pool_upstream(UpstreamConn* u, bool keep_alive, bool body_ended) noexcept
    {
        using R = UpstreamPool::Refusal;
        switch (_pool->release(*u, keep_alive, body_ended, upstream_request_sent(u), now_ns()))
        {
            case R::None:
                return true;
            case R::Unsent:
                // The provider answered before our request finished going out, so it
                // saw a truncated request: GATEWAY-INTERNALS.md 8.
                LB_WARN("closing instead of pooling, request was still going out ", *u);
                ++_stats.upstream_unsent;
                return false;
            case R::Full:
                LB_WARN("CAP pool full, closing instead of pooling ", *u,
                        " limit=", static_cast<uint64_t>(_pool->cap()), " (reuse stops here)");
                return false;
            default:
                return false;
        }
    }

    UpstreamConn* Engine::new_upstream(int slot) noexcept
    {
        // The venue's address now, never a cached one: the ip may have rotated. One
        // that does not parse fails the acquire, on both backends, and a name with no
        // address yet is asked for off the loop.
        const Upstream& up = _upstreams[static_cast<size_t>(slot)];
        sockaddr_in addr{};
        if (!net::resolve_ipv4(up.ip.c_str(), up.port, addr))
        {
            ++_stats.upstream_unresolved;
            request_reresolve(slot);
            return nullptr;
        }
        auto* u = new UpstreamConn();
        u->venue = slot;
        u->ts_opened = now_ns();
        u->up_addr = addr;
        u->rbuf.reserve(kInitialBuf);
        return u;
    }

    UpstreamConn* Engine::acquire_upstream(int slot, AcquireFail* why, int* local_err) noexcept
    {
        AcquireFail ignored = AcquireFail::None;
        AcquireFail& fail = why ? *why : ignored;
        fail = AcquireFail::None;
        // Only this venue's pool: a connection to one provider cannot serve a request
        // bound for another, and handing one over would send the request, and its
        // credential, to the wrong company.
        if (UpstreamConn* u = _pool->acquire(slot))
        {
            u->from_pool = true; // reused -> a pre-response failure is retry-eligible
            u->ts_pool_taken = now_ns();
            u->retried = false;  // fresh request: one retry available again
            ++_stats.upstream_reused;
            return u;
        }
        UpstreamConn* u = new_upstream(slot);
        if (!u)
        {
            fail = AcquireFail::Unresolved;
            return nullptr;
        }
        adopt_warm(u);
#ifdef LLMBRIDGE_HAVE_TLS
        if (upstream_of(u).tls && !tls_attach_upstream(u))
        {
            delete u;
            fail = AcquireFail::Venue;
            return nullptr;
        }
#endif
        if (const int err = _ops->connect(*u))
        {
            delete u;
            if (connect_error_is_local(err))
            {
                fail = AcquireFail::Local;
                if (local_err) *local_err = err;
                ++_stats.upstream_local_failures;
                return nullptr;
            }
            // Known at once, as epoll learns an unreachable network: the venue moves on as
            // it would for the same answer arriving later.
            fail = AcquireFail::Venue;
            note_connect_failure(slot, std::strerror(err));
            return nullptr;
        }
        _upconns->add(u);
        ++_stats.upstream_conns_opened;
        // Pairs with "upstream reuse": without it an upstream first appears in
        // the log at its second request, never at its birth.
        LB_DEBUG("upstream open ", *u, " to ", upstream_of(u).ip, " pool=", pooled_upstream_count());
        return u;
    }

    void Engine::on_connect_error(UpstreamConn* u, int err) noexcept
    {
        // This host's (connect_error_is_local) is answered as an acquire answers it: a 503,
        // nothing failed over, nothing told to the policy. Anything else is the venue's.
        const bool local = connect_error_is_local(err);
        if (!local) note_connect_failure(u->venue, std::strerror(err));
        ClientConn* client = unpair(u);
        _ops->close(*u);
        if (local) ++_stats.upstream_local_failures;
        if (!client) ++_stats.errors;
        else if (local) error_respond(client, 503, local_connect_why(err));
        else fail_request(client, 502, "upstream connect refused");
    }

    bool Engine::retry_upstream(UpstreamConn* u) noexcept
    {
        // A pooled reuse, not yet retried, that failed before any response byte: the
        // provider almost certainly dropped it idle without running the request.
        if (!u->from_pool || u->retried) return false;
        if (now_ns() - u->ts_pool_taken > kStaleRetryWindowNs) return false; // may have run
        ClientConn* client = u->client;
        if (!client || !client->req.can_redispatch()) return false; // a response byte arrived
        // The same venue: a retry that lands elsewhere is a silent reroute, and the
        // request was translated for this dialect and carries its credential.
        UpstreamConn* uf = new_upstream(u->venue);
        if (!uf) return false;
        // A streamed body was staged in pieces, so it is rebuilt from the client's buffer,
        // with the key a policy that decided on its head named and any model or members spliced in,
        // as dispatch sent it.
        if (client->req.f.body_parts)
        {
            const std::string_view head(client->rbuf.data(), client->msg.header_len);
            NamedKey key;
            if ((!client->req.f.key_header.empty() &&
                 named_key(head, client->req.f.head, client->req.f.key_header.view(), upstream_of(u).dialect, key,
                           upstream_of(u).auth_bearer)) ||
                forward_parts(client, upstream_of(u), key, uf->out.stage()))
            {
                delete uf;
                return false;
            }
        }
        uf->retried = true; // this request's one allowed retry is now spent
#ifdef LLMBRIDGE_HAVE_TLS
        if (upstream_of(uf).tls && !tls_attach_upstream(uf))
        {
            delete uf;
            return false;
        }
#endif
        if (const int err = _ops->connect(*uf))
        {
            delete uf;
            if (!connect_error_is_local(err))
            {
                note_connect_failure(u->venue, std::strerror(err));
                return false;
            }
            // This host's: answered 503 here, never reported as the stale venue's EOF.
            ++_stats.upstream_local_failures;
            error_respond(client, 503, local_connect_why(err));
            return true;
        }
        client->req.f.upstream_ip = uf->up_addr.sin_addr.s_addr;
        client->req.f.ts_up_sent = client->req.f.ts_body_sent = 0; // this attempt's own send
        // Plaintext, re-pushed through the new session; a send still reading it gets a copy.
        if (!client->req.f.body_parts) uf->out.stage() = u->out.take();
        _upconns->add(uf);
        ++_stats.upstream_conns_opened;
        ++_stats.upstream_retries;
        // WARN: a recovered failure the client never learns of. A rising rate means the
        // provider drops keep-alives faster than --pool-idle reaps them.
        LB_WARN("upstream stale, resending on a fresh connection ", *u);
        _ops->close(*u); // discard the dead connection; unpairs it
        pair(client, uf);
        mark_first_send(uf); // the resend so far is this attempt's first send
        return true;
    }

    void Engine::release_upstream(UpstreamConn* u, bool keep_alive, bool body_ended) noexcept
    {
        if (!pool_upstream(u, keep_alive, body_ended))
        {
            _ops->close(*u);
            return;
        }
        // A slow client may have paused this upstream's reads, and a pooled connection
        // must be readable or the next response is never seen. The pool's only door.
        if (_caps.stream_backpressure) _ops->set_read(*u, true);
    }

    // One decision and one log text per cause, on both backends.
    void Engine::upstream_lost(UpstreamConn* u, UpFail f) noexcept
    {
        static constexpr const char* kWhy[] = {"upstream EOF", "upstream TLS failed", "upstream write failed"};
        static constexpr const char* kExhausted[] = {"upstream EOF, retry exhausted",
                                                     "upstream TLS failed, retry exhausted",
                                                     "upstream write failed, retry exhausted"};
        const auto i = static_cast<size_t>(f);
        ClientConn* client = u->client;
        if (!client) return _ops->close(*u); // an idle pooled connection the venue dropped
        if (retry_upstream(u)) return;
        if (!upstream_failed(client, 502, kWhy[i])) error_respond(client, 502, kExhausted[i]);
    }

    void Engine::on_upstream_eof(UpstreamConn* u, bool tls_failed) noexcept
    {
        u->peer_eof = true; // never pooled now, whatever finishes the exchange
        ClientConn* client = u->client;
        if (client && client->req.f.streaming)
        {
            // Only a real EOF may end a close-delimited stream: a corrupt one that ends
            // in a well-formed [DONE] would hide the corruption from the client.
            if (tls_failed) return abort_pair(client);
            return stream_on_upstream_eof(u);
        }
        upstream_lost(u, tls_failed ? UpFail::Tls : UpFail::Eof);
    }

    // New bytes in `u->rbuf`. The head is parsed once, by the framer; its Body kind
    // frames the rest, streamed or whole.
    void Engine::on_upstream_bytes(UpstreamConn* u) noexcept
    {
        ClientConn* client = u->client;
        // Stray bytes on an idle pooled upstream are the tail of an exchange that is
        // over; kept, they would be the next client's response.
        if (!client) return _ops->close(*u);
        client->req.f.ts_progress = now_ns();
        // A TLS record may hold no response byte (a session ticket); only a byte ends the body.
        if (client->req.f.body_in_rbuf && !u->rbuf.empty()) end_body_stream(client, u);
        if (client->req.f.streaming) return stream_pump(u);

        const int64_t t4 = now_ns(); // before the parse, so it counts in the response path
        const net::http::FrameStatus hs = u->framer.head(u->rbuf, sent_head(*client));
        if (hs == net::http::FrameStatus::NeedMore) return;
        if (hs == net::http::FrameStatus::Error) return error_respond(client, 502, "upstream response head framing");
        const net::http::ResponseHead& h = u->framer.h();
        // Only a 200 is a real stream; a provider error is relayed with its own status.
        // A translated one needs a venue that streams (a stream:true elsewhere is refused).
        if (h.event_stream && h.status == 200 && h.body != net::http::Body::None &&
            (!client->req.f.translate_body || provider::spec(client->req.f.effective_dialect).streams))
        {
            // t4 for a stream is the head, not the first token: LATENCY.md 3.
            client->req.f.ts_up_recvd = now_ns();
            note_upstream_rtt(client, u);
            note_quota(client, h, std::string_view(u->rbuf.data(), h.header_len));
            note_venue_req_id(client, std::string_view(u->rbuf.data(), h.header_len));
            note_reply(client, h.status, /*streamed=*/true);
            return begin_stream(u, h);
        }

        const net::http::ParsedResponse r = u->framer.message(u->rbuf);
        if (r.failed())
        {
            // The head framed, so its status is the venue's answer even when the body
            // cannot be framed: a close-delimited 503 must still count against it. An
            // interim 1xx is not an answer.
            if (h.status >= 200) note_reply(client, h.status, /*streamed=*/false);
            return error_respond(client, 502, "upstream response framing");
        }
        if (!r.complete()) return;
        client->req.f.ts_up_recvd = t4;
        note_upstream_rtt(client, u);
        note_quota(client, h, std::string_view(u->rbuf.data(), h.header_len));
        note_venue_req_id(client, std::string_view(u->rbuf.data(), h.header_len));
        note_upstream_error(client, h, r.body);
        note_served_tier(client, r.body, /*tail=*/true);
        note_served_model(client, r.body, /*streamed=*/false);
        if (provider::GeminiFacts g; client->req.f.gemini_reply && h.status == 200 && provider::gemini_facts(r.body, g))
            keep_gemini_facts(client, g);
        note_reply(client, h.status, /*streamed=*/false);
        on_response(u, h, r.body, r.total_len);
    }

    void Engine::on_response(UpstreamConn* u, const net::http::ResponseHead& h,
                              std::string_view body, size_t total_len) noexcept
    {
        ClientConn* client = u->client;
        // The venue alone decides whether its connection stays open: a client's own
        // Connection header is never forwarded.
        const bool keep_alive = h.keep_alive;
        const std::string_view up_head(u->rbuf.data(), h.header_len - 4);
        if (client->req.f.translate_body && h.status != 0 && h.status != 200)
        {
            // A provider failure keeps its status and message: a 429 or 529 translated
            // as a completion would fail and reach the client as a generic 502.
            append_relayed_reply(client->out.stage(), h.status, up_head,
                                 provider::upstream_error_to_openai(body, "upstream_error"), false);
            count_error(client);
        }
        else if (client->req.f.translate_body)
        {
            if (!xlate_resp(client->req.f.effective_dialect, body, client->xlate_scratch, client->req.f.tok))
            {
                unpair(u);
                u->rbuf.erase(0, total_len); // framing was valid; the connection is reusable
                release_upstream(u, h.keep_alive, true);
                return error_respond(client, 502, "response translate");
            }
            std::string timing;
            if (_timing_headers)
            {
                const int64_t ts_resp_built = now_ns(); // t5: built here, written next
                const TimingSplit sp = split_of(client->req.f, ts_resp_built);
                append_timing_headers(timing, client->req.f, sp, "x-llmbridge-upstream-us");
                append_usage_headers(timing, client->req.f.tok);
            }
            append_http(client->out.stage(), "HTTP/1.1 200 OK", client->xlate_scratch, timing);
        }
        else
        {
            // A decoded chunked body is re-framed with Content-Length, keeping the
            // upstream's own status; anything else is relayed verbatim.
            if (h.body == net::http::Body::Chunked)
                append_relayed_reply(client->out.stage(), h.status ? h.status : 200, up_head, body, true);
            else client->out.stage().append(u->rbuf.data(), total_len);
            client->req.f.tok = scan_usage(body); // a byte-forward reports its counts too
            if (client->req.f.responses_api) provider::openai::count_search_calls(body, false, client->req.f.tok);
        }
        client->req.f.phase = Phase::Replying;
        unpair(u);
        // Anything past the message is a second response, and keeps it out of the pool.
        u->rbuf.erase(0, total_len);
        release_upstream(u, keep_alive, true);
        respond(client);
    }

    bool Engine::upstream_failed(ClientConn* client, int status, const char* why) noexcept
    {
        const Retry r = failover_target(client, status, why);
        if (!r.retry) return false;

        LB_WARN(ReqId{client->req.f.req_seq}, " venue ", static_cast<int64_t>(client->upstream_slot),
                " failed (", why, "); retrying on ", static_cast<int64_t>(r.upstream_index));
        ReqState& f = client->req.f;
        if (UpstreamConn* u = unpair(client))
        {
            if (f.body_streaming) reset_venue(u); // it holds part of a body it will not finish
            _ops->close(*u);
        }
        ++f.failover_attempts;
        client->upstream_slot = r.upstream_index;
        ++_stats.upstream_failovers;
        // The original request back at the front of rbuf. The forward rebuilds it for the
        // new venue's dialect and erases exactly these bytes again, so a pipelined
        // request queued behind it keeps its place. A body kept in rbuf is still there.
        if (!f.body_in_rbuf) client->rbuf.insert(0, client->req.saved);
        // A body still arriving goes on as it arrives to a venue that takes it as it is, a
        // substitution spliced if its model was tracked from the first byte, members if the
        // client's own were, else it parks to its last byte (GATEWAY-INTERNALS.md 12c-12e).
        else if (f.body_streaming && !(overrides_stream(client, /*at_head=*/false) &&
                                       (f.subst_model.empty() || f.subst_tracked || f.model_in_path) &&
                                       venue_streams(client_dialect_of(client->rbuf), client->upstream_slot)))
        {
            stop_body(client);
            f.body_parked = true;
            f.splice = false; // nothing goes spliced: the body is rewritten whole after the confirm
            f.ts_progress = 0; // no venue to time out: the body's deadline holds it (12b)
            ++_stats.streamed_body_parked;
            return true;
        }
        dispatch(client);
        return true;
    }

    Retry Engine::failover_target(ClientConn* client, int status, const char* why) noexcept
    {
        if (!_policy || !client || client->doomed) return {};
        ReqState& rf = client->req.f;
        // A venue that had the request failed, and the client has been sent nothing: a
        // reply begun is no failure of this kind, and a parked body no venue holds.
        if (rf.body_parked || (rf.phase != Phase::Dispatched && rf.phase != Phase::Responding) ||
            !client->out.idle())
            return {};
        // The policy hears of every such failure, so one that counts them sees them all; it
        // routes only those that are safe to redo. Every precondition here is about safety,
        // not policy: a response byte in means the venue may have acted, so a re-send could
        // duplicate it; re-sending one we no longer hold is impossible; and an unbounded
        // chain turns one dead provider into a latency multiplier. A hedge is its own
        // redundancy: a leg that fails is counted, never sent again.
        const bool retryable = _upstreams.size() >= 2 && !rf.hedged && client->req.can_redispatch() &&
                               (!client->req.saved.empty() || rf.body_in_rbuf) &&
                               rf.failover_attempts < kMaxFailoverAttempts - 1;
        const FailureFacts f{.upstream_index = client->upstream_slot, .status = status, .reason = why,
                             .attempt = rf.failover_attempts, .tag = rf.policy_tag, .seq = rf.req_seq,
                             .retryable = retryable};
        const Retry r = _policy->on_failure(f);
        if (!retryable || !r.retry) return {};
        if (r.upstream_index < 0 || static_cast<size_t>(r.upstream_index) >= _upstreams.size())
        {
            LB_WARN(ReqId{client->req.f.req_seq}, " failover to out-of-range upstream ",
                    static_cast<int64_t>(r.upstream_index), "; giving up");
            return {};
        }
        // Sending it back to the venue that just failed is the one answer that cannot
        // help, and it is how a policy accidentally writes an infinite loop.
        if (r.upstream_index == client->upstream_slot)
        {
            LB_WARN(ReqId{client->req.f.req_seq}, " failover named the venue that just failed; "
                                            "giving up");
            return {};
        }
        // The next venue's key comes from the header the retry names, never the failed
        // one's; none, once any attempt has named one, would send another venue's key.
        if (r.credential_header.empty() ? !client->req.f.key_header.empty() : !key_header_ok(r.credential_header))
        {
            LB_WARN(ReqId{client->req.f.req_seq}, " failover named no usable credential header; giving up");
            return {};
        }
        // The new venue's overrides, never the failed one's: its model name is wrong for
        // another venue. Copied now, since the views die once on_failure has returned.
        if (!rf.model_override.set(r.model) || !rf.tier_override.set(r.service_tier) ||
            !rf.speed_override.set(r.speed) || !rf.geo_override.set(r.inference_geo) ||
            (!r.anthropic_beta.empty() && !beta_token_ok(r.anthropic_beta)) ||
            !substitution_ok(r.substitute_model, r.substitute_for))
        {
            LB_WARN(ReqId{rf.req_seq}, " failover override unusable or longer than the gateway "
                                       "holds; giving up");
            return {};
        }
        rf.beta_override.set(r.anthropic_beta);
        if (!r.credential_header.empty()) rf.key_header.set(r.credential_header);
        rf.subst_model.set(r.substitute_model);
        rf.subst_for.set(r.substitute_for);
        return r;
    }

} // namespace llmbridge::detail
