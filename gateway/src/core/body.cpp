// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// A request body forwarded to its venue as it arrives instead of after its last byte:
// GATEWAY-INTERNALS.md 12.

#include "engine.hpp"

#include <algorithm>

#include "core/io.hpp"
#include "core/limits.hpp"
#include "net/socket_util.hpp"
#include "request.hpp"
#include "response.hpp"

namespace llmbridge::detail
{

    bool Engine::body_streamable(const ClientConn* c, const net::http::Message& m) noexcept
    {
        // A policy reads the body before it routes, so its requests wait for all of it,
        // unless it decides on the head (12c).
        if (!_stream_body || (_policy && !_decides_on_head) || m.body_len < _stream_body_min || m.encoded ||
            c->rbuf.size() >= m.total_len)
            return false;
        if (!net::http::request_line_ok(c->rbuf) || target_refusal(c->rbuf)) return false; // the framing path refuses it
        const Dialect client = client_dialect_of(std::string_view(c->rbuf.data(), m.header_len));
        if (client == Dialect::OpenAI && !_stream_openai_body) return false;
        // A byte-forward only: a translation reads the whole body. A policy picks the venue
        // later, so one that could take it is enough to decide on the head.
        if (_policy ? !any_venue_streams(client) : !venue_streams(client, 0)) return false;
        // Each holds a venue connection to its last byte; past the cap one waits, as before.
        if (_stream_body_max == 0 || _bodies_streaming < _stream_body_max) return true;
        ++_stats.streamed_body_capped;
        return false;
    }

    bool Engine::venue_streams(Dialect client, int slot) const noexcept
    {
        const TranslationPlan plan = resolve_translation(client, venue_at(slot).dialect, false);
        return plan.ok && !plan.translate;
    }

    bool Engine::any_venue_streams(Dialect client) const noexcept
    {
        for (size_t i = 0; i < _upstreams.size(); ++i)
            if (venue_streams(client, static_cast<int>(i))) return true;
        return false;
    }

    // Admitted before its last byte: forwarded as it arrives, or parked unsent in rbuf to
    // its last byte (12c), which holds no venue and no slot. An OpenAI client's JSON is
    // checked as it passes either way; a file upload is not JSON. Any client's JSON that
    // streams under a model substitution is scanned too, its model tracked (12d), and so
    // is one that streams with service_tier, speed or inference_geo spliced in (12e).
    void Engine::start_body(ClientConn* c, bool forward) noexcept
    {
        ReqState& f = c->req.f;
        f.body_in_rbuf = true;
        const bool json = json_media_type(f.head.content_type.in(c->rbuf));
        const bool openai = client_dialect_of(c->rbuf) == Dialect::OpenAI;
        f.subst_tracked = json && forward && !f.subst_model.empty() && !f.model_in_path;
        f.members_tracked = json && forward ? f.member_overrides() : 0;
        f.body_scanned = json && (openai || f.subst_tracked || f.members_tracked);
        if (f.body_scanned)
        {
            if (!c->req.scan) c->req.scan = std::make_unique<provider::BodyScan>();
            c->req.scan->reset(/*all_keys=*/openai, f.subst_tracked, f.members_tracked);
        }
        f.body_in = static_cast<uint32_t>(body_so_far(c).size());
        if (forward)
        {
            f.body_streaming = f.body_streamed = true;
            ++_stats.streamed_bodies;
            ++_bodies_streaming;
            return dispatch(c);
        }
        f.body_parked = true;
        f.phase = Phase::Dispatched; // in flight, so the sweep holds it to the body deadline
        c->ever_framed = true;
        ++_stats.streamed_body_parked;
        LB_DEBUG(ReqId{f.req_seq}, " body parked to its last byte on ", *c);
    }

    void Engine::stop_body(ClientConn* c) noexcept
    {
        if (!c->req.f.body_streaming) return;
        c->req.f.body_streaming = false;
        --_bodies_streaming;
    }

    // The venue holds a body short of its Content-Length, which it never runs.
    void Engine::reset_venue(UpstreamConn* u) noexcept
    {
        u->peer_eof = true;
        if (u->fd < 0) return;
        u->reset_on_close = true;
        net::set_reset_on_close(u->fd);
    }

    void Engine::cut_body(ClientConn* c) noexcept
    {
        stop_body(c);
        if (UpstreamConn* u = unpair(c))
        {
            reset_venue(u);
            _ops->close(*u);
        }
    }

    // --body-timeout from its head, or --body-grace plus a second for every --body-min-rate
    // bytes in, whichever is sooner: a slow but steady upload keeps earning time.
    int64_t Engine::body_deadline(const ReqState& f) const noexcept
    {
        int64_t at = _body_timeout_ns > 0 ? f.ts_req_recvd + _body_timeout_ns : INT64_MAX;
        if (_body_min_rate > 0)
            at = std::min(at, f.ts_req_recvd + _body_grace_ns +
                                  static_cast<int64_t>(f.body_in * 1'000'000'000ULL / _body_min_rate));
        return at;
    }

    // The reply cuts the body (error_respond) and closes the connection, so the rest of
    // the body is never waited for.
    void Engine::body_timeout(ClientConn* c, uint64_t& counter, const char* why) noexcept
    {
        ++counter;
        LB_WARN(ReqId{c->req.f.req_seq}, " TIMEOUT ", why, " ", *c, " body_in=",
                static_cast<uint64_t>(body_so_far(c).size()), " of ", static_cast<uint64_t>(c->msg.body_len),
                " after_ns=", now_ns() - c->req.f.ts_req_recvd);
        error_respond(c, 408, why, kSlowBodyMessage);
    }

    std::string_view Engine::body_so_far(const ClientConn* c) const noexcept
    {
        const size_t end = std::min(c->rbuf.size(), c->msg.total_len);
        if (end <= c->msg.header_len) return {};
        return std::string_view(c->rbuf.data() + c->msg.header_len, end - c->msg.header_len);
    }

    // New body bytes: scanned first, then whatever is known good goes upstream. The body
    // stays in rbuf, so a stale-connection retry can resend it. The time this takes after
    // t3 is ours (body_work_ns); before it, connect or upwrite holds it. The last byte's
    // arrival ends the client's upload.
    void Engine::pump_body(ClientConn* c) noexcept
    {
        const int64_t t = now_ns();
        ReqState& f = c->req.f;
        // Late before these bytes came: they never go, so the venue never has the body whole.
        if (t > body_deadline(f)) return body_timeout(c, _stats.streamed_body_timeouts, "streamed body too slow");
        const bool after_t3 = f.ts_up_sent != 0;
        const std::string_view body = body_so_far(c);
        f.body_in = static_cast<uint32_t>(body.size());
        const bool last = body.size() == c->msg.body_len;
        size_t safe = body.size();
        if (f.body_scanned)
        {
            if (c->req.scan->feed(body, last) == provider::BodyScan::Verdict::Refused)
                return refuse_body(c, c->req.scan->why());
            safe = c->req.scan->safe();
        }
        if (last && !end_upload(c, t)) return;
        // A head decision's last byte waits for its confirm (12c), which end_upload runs.
        if (f.confirm_pending && safe == c->msg.body_len) --safe;
        if (!forward_body(c, body, safe)) return;
        if (after_t3) f.body_work_ns += now_ns() - t;
    }

    bool Engine::forward_body(ClientConn* c, std::string_view body, size_t safe) noexcept
    {
        UpstreamConn* u = c->up;
        if (!u || safe <= c->req.f.body_sent) return true; // parked, or nothing new is safe
        if (c->req.f.splice) append_spliced(c, u->out.stage(), c->req.f.body_sent, safe);
        else u->out.stage().append(body.substr(c->req.f.body_sent, safe - c->req.f.body_sent));
        c->req.f.body_sent = static_cast<uint32_t>(safe);
        // Unconnected, the connect's completion sends it; staged is not progress, sent is.
        return !u->connected || send_upstream(u);
    }

    namespace
    {
        MemberSplice members_of(const ReqState& f) noexcept
        {
            return {f.tier_override.view(), f.speed_override.view(), f.geo_override.view()};
        }
        Splice splice_of(const ReqState& f) noexcept
        {
            // A path names the model (model_in_path): only members go into the body.
            if (f.model_in_path) return {{}, members_of(f)};
            return {{f.subst_for.view(), f.subst_model.view()}, members_of(f)};
        }
    } // namespace

    // The scan holds the model's value back until it closes, and each member it tracks with
    // the separator before it, so a range never splits one.
    void Engine::append_spliced(const ClientConn* c, std::string& out, size_t from, size_t to) const noexcept
    {
        splice_of(c->req.f).append(out, body_so_far(c), from, to, c->msg.body_len, *c->req.scan);
    }

    bool Engine::splice_hit(const ClientConn* c) const noexcept
    {
        return splice_of(c->req.f).substitutes(body_so_far(c), *c->req.scan);
    }

    // The body overrides in force can go into a stream (12e): none but members, each plain,
    // spliced into a JSON body at the head decision; on a later attempt, only members whose
    // client's own the scan has held back from the first byte.
    bool Engine::overrides_stream(const ClientConn* c, bool at_head) const noexcept
    {
        const ReqState& f = c->req.f;
        if (!f.model_override.empty() && !f.model_in_path) return false;
        const uint8_t m = f.member_overrides();
        if (!m) return true;
        if (!members_of(f).plain()) return false;
        return at_head ? json_media_type(f.head.content_type.in(c->rbuf)) : (m & ~f.members_tracked) == 0;
    }

    // The client's own value of a member the scan tracked, raw, without its quotes; empty
    // when it sent none.
    std::string_view Engine::client_member(const ClientConn* c, uint8_t bit) const noexcept
    {
        if (!(c->req.f.members_tracked & bit)) return {};
        const provider::BodyScan& s = *c->req.scan;
        for (size_t i = 0; i < s.member_count(); ++i)
            if (const provider::BodyScan::Member& m = s.members()[i]; m.bit == bit)
                return body_so_far(c).substr(m.value_at + 1, m.end - m.value_at - 2);
        return {};
    }

    const char* Engine::forward_parts(const ClientConn* c, const Upstream& up, const NamedKey& key,
                                      std::string& into) const noexcept
    {
        const ReqState& f = c->req.f;
        const std::string_view head(c->rbuf.data(), c->msg.header_len);
        const std::string_view beta = beta_for(up, f);
        if (!f.splice)
            return build_forward(head, c->msg.header_len, f.head, _strip_headers, up.host_hdr, up.base_path,
                                 body_so_far(c).substr(0, f.body_sent), into, c->msg.body_len, key, beta,
                                 f.path_model(), up.dialect);
        // The length is ours: the client's, and what the replacement and members add (12d, 12e).
        if (const char* bad = build_forward(head, c->msg.header_len, f.head, _strip_headers, up.host_hdr,
                                            up.base_path, {}, into, c->msg.body_len + splice_of(f).grow(), key,
                                            beta, f.path_model(), up.dialect))
            return bad;
        append_spliced(c, into, 0, f.body_sent);
        return nullptr;
    }

    // A body sent whole (held, parked or redone, or failed over once whole): its model is
    // known, so a substitution is an override like Decision::model, applied in one rewrite.
    void Engine::resolve_substitution(ClientConn* c) noexcept
    {
        ReqState& f = c->req.f;
        f.splice = false;
        f.model_substituted = false;
        f.members_spliced = 0; // rewritten whole, if set: no splice
        if (f.subst_model.empty() || !f.model_override.empty() || f.model.view() != f.subst_for.view()) return;
        f.model_override.set(f.subst_model.view());
        f.model_substituted = true;
    }

    bool Engine::end_upload(ClientConn* c, int64_t t) noexcept
    {
        ReqState& f = c->req.f;
        stop_body(c);
        f.ts_body_end = t;
        f.client_upload_ns = span_since(c->ts_first_byte, t);
        // Setup holds the upload, as a held body's does, so subtracting it still works.
        if (!f.client_conn_reused) f.client_conn_setup_ns = span_since(c->ts_accepted, t);
        c->ts_first_byte = c->rbuf.size() > c->msg.total_len ? t : 0; // as open_request
        if (!f.decided_on_head)
        {
            if (_sink) (void)capture_model(c); // the facts only: every byte already passed
            return true;
        }
        // What a held body would have been refused for, before its last byte leaves.
        if (const char* why = capture_model(c))
        {
            refuse_body(c, why);
            return false;
        }
        return confirm_body(c, t);
    }

    // Whole now, and sent as a held body: the upload was the client's, so t0 is its last
    // byte, and nothing done on an earlier attempt is this one's work.
    void Engine::dispatch_whole(ClientConn* c, int64_t t) noexcept
    {
        c->req.f.body_parked = false;
        c->req.f.ts_req_recvd = t;
        c->req.f.body_work_ns = 0;
        dispatch(c);
    }

    // The body failed a check after part of it may have gone: stop, and reset the upstream
    // so the provider holds a body short of its Content-Length and never runs it.
    void Engine::refuse_body(ClientConn* c, const char* why) noexcept
    {
        ++_stats.streamed_body_refusals;
        LB_WARN(ReqId{c->req.f.req_seq}, " streamed body refused (", why, ") after ",
                static_cast<uint64_t>(c->req.f.body_sent), " bytes went upstream; resetting it");
        cut_body(c);
        end_body_stream(c, nullptr);
        const bool keys = std::string_view(why).find("key") != std::string_view::npos;
        error_respond(c, 400, why, keys ? kAmbiguousKeysMessage : kNotJsonMessage);
    }

    // A response byte arrived, or the request is over: the copy in rbuf is no longer a
    // resend's, and body bytes still to come are dropped as they arrive.
    void Engine::end_body_stream(ClientConn* c, UpstreamConn* u) noexcept
    {
        if (c->req.f.body_streaming && u)
        {
            // The response beat the request out: cut short, the connection is never pooled.
            stop_body(c);
            u->peer_eof = true;
            ++_stats.upstream_unsent;
            LB_WARN("response before the whole body went; the upstream will not be pooled ", *u);
        }
        const size_t held = std::min(c->rbuf.size(), c->msg.total_len);
        c->req.discard += c->msg.total_len - held;
        c->rbuf.erase(0, held);
        c->req.f.body_in_rbuf = c->req.f.body_parked = false;
    }

    void Engine::drop_discard(ClientConn* c) noexcept
    {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(c->req.discard, c->rbuf.size()));
        c->rbuf.erase(0, n);
        c->req.discard -= n;
        c->req.f.body_in += static_cast<uint32_t>(n); // its deadline still runs (sweep_client)
        if (c->req.discard) return;
        c->ts_first_byte = c->rbuf.empty() ? 0 : now_ns(); // the next request's arrival
        if (!c->req.close_on_drain) return;
        c->req.close_on_drain = false;
        _ops->close(*c);
    }

} // namespace llmbridge::detail
