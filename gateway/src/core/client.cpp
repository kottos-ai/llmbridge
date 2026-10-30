// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The policy seam on a freshly framed request, and replying to a client.

#include "engine.hpp"

#include "core/io.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"
#include "request.hpp"
#include "response.hpp"

namespace llmbridge::detail
{

    // Head, plus the model and two streaming flags and nothing else from the body. This
    // is the line where "metadata only, no prompt text". All three were read by
    // capture_model, which both backends run first when a policy exists, or, on the head
    // of a body still arriving, by decide_on_head.
    RequestFacts Engine::facts_of(const ClientConn* c, bool whole) const noexcept
    {
        // A streamed body's splice is done once it is whole: every member set went in (12e).
        const uint8_t spliced = whole && c->req.f.splice ? c->req.f.member_overrides() : 0;
        // A path names the model, so its substitution is done at the head (model_in_path).
        const bool substituted = c->req.f.model_in_path ? c->req.f.model_substituted : c->req.f.splice && splice_hit(c);
        return {std::string_view(c->rbuf.data(), c->msg.header_len), c->msg.body_len, c->req.f.model.view(),
                c->req.f.prefix_hash, c->req.f.req_seq, c->req.f.asked_stream, c->req.f.asked_usage, whole,
                whole && substituted, (spliced & provider::BodyScan::kServiceTier) != 0,
                (spliced & provider::BodyScan::kSpeed) != 0, (spliced & provider::BodyScan::kInferenceGeo) != 0};
    }

    Decision Engine::policy_decision(ClientConn* c, bool whole) noexcept
    {
        const Decision d = _policy->decide(facts_of(c, whole));
        if (!d.allow) return checked_refusal(c, d);
        c->req.f.policy_tag = d.tag; // handed back verbatim in FailureFacts
        return d;
    }

    Decision Engine::checked_refusal(const ClientConn* c, Decision d) noexcept
    {
        // A status the responder cannot render would emit a misleading reply, so
        // substitute and say so loudly. The refusal still stands: fail closed.
        if (d.deny_status < 400 || d.deny_status > 599)
        {
            LB_WARN(ReqId{c->req.f.req_seq}, " policy returned out-of-range status ", d.deny_status,
                    " (", d.reason, "); refusing with 403");
            d.deny_status = 403;
        }
        // The one policy string a client sees, spliced into JSON unescaped. Never
        // printed here: a message that failed the check is not safe in a log line.
        if (d.message && !deny_message_ok(d.message))
        {
            LB_WARN(ReqId{c->req.f.req_seq}, " policy message refused (", d.reason,
                    "); sending the generic one");
            d.message = nullptr;
        }
        // Never log the facts: the head carries the client's Authorization.
        ++_stats.policy_denied;
        return d;
    }

    void Engine::on_client_bytes(ClientConn* c) noexcept
    {
        if (c->req.discard) drop_discard(c);
        if (c->doomed) return;
        if (c->req.f.body_streaming || c->req.f.body_parked) pump_body(c);
        else if (c->req.f.phase == Phase::Idle && !c->rbuf.empty()) try_frame(c);
        if (!c->doomed) gate_client_reads(c);
    }

    // Reading continues while a request is in flight, so this is the only bound on what
    // a client can pipeline behind it. No legal request is larger, so a pause costs a
    // legitimate client nothing; it resumes once its requests drain the buffer.
    void Engine::gate_client_reads(ClientConn* c) noexcept
    {
        const bool over = c->req.f.phase != Phase::Idle && c->rbuf.size() > kMaxClientBuffered;
        if (over == c->read_paused) return;
        if (over)
        {
            ++_stats.client_read_pauses;
            LB_WARN("CAP client pipelined ", c->rbuf.size(), " bytes behind its request in flight ",
                    *c, "; reading paused");
        }
        _ops->set_read(*c, !over);
    }

    void Engine::client_gone(ClientConn* c) noexcept
    {
        // Mid-request (a reply or stream still owed included) is an abort; idle is normal.
        if (c->req.f.phase != Phase::Idle) abort_pair(c);
        else _ops->close(*c);
    }

    void Engine::sink_unfinished(ClientConn* c) noexcept
    {
        // Every way a request ends reaches forget(), so a request that never completed is
        // recorded here whatever closed it: a client that left, a stream we truncated, a
        // send that failed. A hedged request is recorded leg by leg (close_shadow).
        ReqState& f = c->req.f;
        if (!_sink || c->shadow || f.sunk || f.hedged || f.phase == Phase::Idle) return;
        // A reply complete upstream keeps its status and a stream we cut stays a truncated
        // 200, as finalize_stream records one. Otherwise the venue never finished, and 499,
        // nginx's "client closed request", says the wait ended with the client.
        const bool replied = f.phase == Phase::Replying;
        const bool cut = f.streaming && c->close_after_resp;
        sink_emit(c, replied ? status_of(c->out.bytes()) : cut ? 200 : 499, f.streaming);
    }

    void Engine::send_continue(ClientConn* c) noexcept
    {
        // Phase stays Idle, so the send completing finishes nothing.
        c->out.stage().append(kContinue);
        if (_ops->send(*c) == Sent::Failed) _ops->close(*c);
    }

    // t0 is stamped before the parse that frames the request, so the parse counts in the
    // request path; a partial request discards it, so network wait never does.
    void Engine::try_frame(ClientConn* c) noexcept
    {
        // A request whose length is known is not re-parsed per read.
        if (c->client_frame_want && c->rbuf.size() < c->client_frame_want) return;
        const int64_t t0 = now_ns();
        net::http::Message m;
        // The one header walk: framing, credentials, Expect and Connection together.
        HeadFacts facts;
        const auto st = net::http::parse_request(c->rbuf, m, HeadReader{c->rbuf, _strip_headers, facts});
        if (st == net::http::FrameStatus::NeedMore)
        {
            if (!m.total_len) return;
            if (!c->client_frame_want)
            {
                c->rbuf.reserve(m.total_len);
                // No body byte yet: the client is waiting on us, not the network.
                if (c->rbuf.size() <= m.header_len && facts.expect == 2) send_continue(c);
                if (c->doomed) return; // the 100 found the client gone
            }
            c->client_frame_want = m.total_len; // held to its last byte, unless it goes now
            // A body that needs no rewrite goes upstream as it arrives.
            if (!body_streamable(c, m)) return;
            if (_policy)
            {
                // A policy that decides on the head may want its leading bytes first (12c).
                if (const size_t want = decide_want(m); c->rbuf.size() < want) c->client_frame_want = want;
                else decide_on_head(c, m, facts, t0);
                return;
            }
            c->client_frame_want = 0;
            open_request(c, m, facts, t0);
            if (_sink) sink_capture(c);
            c->upstream_slot = 0;
            return start_body(c, /*forward=*/true);
        }
        c->client_frame_want = 0;
        open_request(c, m, facts, t0);
        if (st == net::http::FrameStatus::Error) return error_respond(c, 400, "request framing");
        if (!net::http::request_line_ok(c->rbuf)) return error_respond(c, 400, "request line");
        if (const Refusal r = target_refusal(c->rbuf)) return error_respond(c, 400, r.why, r.detail);
        // Nothing here inflates, and every reader downstream assumes JSON.
        if (c->msg.encoded) return error_respond(c, 415, "compressed request body");
        if (_sink) sink_capture(c);
        if (_sink || _policy)
            if (const char* why = capture_model(c)) return error_respond(c, 400, why, kAmbiguousKeysMessage);
        LB_DEBUG(ReqId{c->req.f.req_seq}, " ", request_line(c->rbuf), " on ", *c);
        // The policy seam: framed, but nothing translated, no credential mapped and no
        // upstream acquired, so a refusal reaches no provider. Venue 0 without one.
        c->upstream_slot = 0;
        if (_policy)
        {
            const Decision d = policy_decision(c, /*whole=*/true);
            if (!d.allow) return error_respond(c, d.deny_status, d.reason, d.message);
            if (!apply_decision(c, d)) return;
        }
        dispatch(c);
    }

    // The venue, the overrides and the key a decision names, at decide() or a confirm()
    // that redoes the request.
    bool Engine::apply_decision(ClientConn* c, const Decision& d) noexcept
    {
        // Out of range is the default: a venue that has left the table must not send the
        // request somewhere arbitrary. Unless a key is named for that venue, which venue 0
        // would receive as its own.
        c->upstream_slot = 0;
        if (d.upstream_index >= 0 && static_cast<size_t>(d.upstream_index) < _upstreams.size())
            c->upstream_slot = d.upstream_index;
        else if (d.upstream_index >= 0 && !d.credential_header.empty())
            return error_respond(c, 500, "policy named a key for a venue past the table"), false;
        else if (d.upstream_index >= 0)
            LB_WARN(ReqId{c->req.f.req_seq}, " policy chose upstream ", static_cast<int64_t>(d.upstream_index),
                    " of ", static_cast<int64_t>(_upstreams.size()), "; using 0");
        // Copied: the views die when decide() returns. Refused rather than cut, which
        // would route to whatever model the prefix names.
        ReqState& f = c->req.f;
        if (!f.model_override.set(d.model) || !f.tier_override.set(d.service_tier) ||
            !f.speed_override.set(d.speed) || !f.geo_override.set(d.inference_geo))
            return error_respond(c, 500, "policy override longer than the gateway holds"), false;
        // Lands in a header line: a token or nothing, never a list or a guess.
        if (!d.anthropic_beta.empty() && !beta_token_ok(d.anthropic_beta))
            return error_respond(c, 500, "policy anthropic_beta unusable"), false;
        f.beta_override.set(d.anthropic_beta);
        // Spliced into a stream unescaped, its length declared ahead: refused, never fixed up.
        if (!substitution_ok(d.substitute_model, d.substitute_for))
            return error_respond(c, 500, "policy model substitution unusable"), false;
        c->req.f.subst_model.set(d.substitute_model);
        c->req.f.subst_for.set(d.substitute_for);
        // A name outside the family would be echoed by a byte-forward: refused, never guessed.
        if (!d.credential_header.empty() && !key_header_ok(d.credential_header))
            return error_respond(c, 400, kWhyKeyUnusable, refuse::kKeyHeaderUnusable), false;
        f.key_header.set(d.credential_header);
        f.policy_tag = d.tag;
        return apply_hedge(c, d);
    }

    size_t Engine::decide_want(const net::http::Message& m) const noexcept
    {
        return m.header_len + (_wants_prefix_hash ? std::min(m.body_len, kPrefixHashBytes) : 0);
    }

    // A large body's policy decision on its head (GATEWAY-INTERNALS.md 12c): refused at
    // once, or admitted to stream or to park, its last byte withheld until confirm().
    void Engine::decide_on_head(ClientConn* c, const net::http::Message& m, const HeadFacts& facts,
                                int64_t t0) noexcept
    {
        c->client_frame_want = 0;
        open_request(c, m, facts, t0);
        if (_sink) sink_capture(c);
        ReqState& f = c->req.f;
        f.decided_on_head = f.confirm_pending = f.body_in_rbuf = true;
        ++_stats.policy_head_decisions;
        LB_DEBUG(ReqId{f.req_seq}, " ", request_line(c->rbuf), " on ", *c, " decided on its head");
        // A model only once it has closed in the bytes so far, as capture_model bounds it;
        // the hash over the bytes a held body's covers, which try_frame waited for.
        const std::string_view body = body_so_far(c);
        // A path names the model and whether it streams, so both are known on the head.
        const provider::TopLevelFacts path = f.model_in_path ? body_facts(c->rbuf, {}) : provider::TopLevelFacts{};
        const std::string_view model = f.model_in_path ? path.model : provider::model_of(body);
        if (model.size() <= sizeof f.model.b) f.model.set(model);
        f.asked_stream = path.stream;
        if (_wants_prefix_hash) f.prefix_hash = prefix_hash_of(body.substr(0, std::min(m.body_len, kPrefixHashBytes)));
        const Decision d = policy_decision(c, /*whole=*/false);
        if (!d.allow)
        {
            // Nothing went anywhere: the rest of the body is read and dropped, and the
            // connection carries the client's next request.
            end_body_stream(c, nullptr);
            return error_respond(c, d.deny_status, d.reason, d.message, /*keep=*/true);
        }
        if (!apply_decision(c, d)) return;
        f.held = d.hold || f.hedge_n != 0; // every hedge leg is sent the whole body
        start_body(c, !f.held && overrides_stream(c, /*at_head=*/true) &&
                          venue_streams(client_dialect_of(c->rbuf), c->upstream_slot));
    }

    // The decision in force, as confirm() is handed it: the venue serving now, never -1.
    Decision Engine::decision_in_effect(const ClientConn* c, HedgeLeg* legs) const noexcept
    {
        const ReqState& f = c->req.f;
        for (uint8_t i = 0; i < f.hedge_n; ++i)
        {
            const HedgeLegCopy& k = f.hedge[i];
            legs[i] = HedgeLeg{.upstream_index = k.venue, .model = k.model.view(), .service_tier = k.tier.view(),
                               .speed = k.speed.view(), .inference_geo = k.geo.view(),
                               .anthropic_beta = k.beta.view(), .credential_header = k.key.view()};
        }
        Decision d{};
        d.hedge = f.hedge_n ? legs : nullptr;
        d.hedge_legs = f.hedge_n;
        d.allow = true;
        d.upstream_index = c->upstream_slot;
        d.model = c->req.f.model_override.view();
        d.service_tier = c->req.f.tier_override.view();
        d.speed = c->req.f.speed_override.view();
        d.inference_geo = c->req.f.geo_override.view();
        d.anthropic_beta = c->req.f.beta_override.view();
        d.reason = "";
        d.tag = c->req.f.policy_tag;
        d.credential_header = c->req.f.key_header.view();
        d.hold = c->req.f.held;
        d.substitute_model = c->req.f.subst_model.view();
        d.substitute_for = c->req.f.subst_for.view();
        return d;
    }

    // The whole body's facts against the decision taken on its head, before its last byte
    // leaves: keep, refuse or redo (policy.hpp).
    bool Engine::confirm_body(ClientConn* c, int64_t t) noexcept
    {
        ReqState& f = c->req.f;
        f.confirm_pending = false;
        const RequestFacts whole = facts_of(c, /*whole=*/true);
        HedgeLeg legs[kMaxHedgeLegs];
        const Decision d = _policy->confirm(whole, decision_in_effect(c, legs));
        if (!d.allow)
        {
            f.confirm_outcome = ConfirmOutcome::Refused;
            ++_stats.streamed_body_confirm_refusals;
            const Decision r = checked_refusal(c, d);
            LB_WARN(ReqId{f.req_seq}, " confirm refused (", r.reason, ") after ",
                    static_cast<uint64_t>(f.body_sent), " bytes went upstream");
            cut_body(c);
            end_body_stream(c, nullptr);
            error_respond(c, r.deny_status, r.reason, r.message);
            return false;
        }
        // The venue apply_decision would pick: out of range is the first, unless a key is named.
        const bool in_table = d.upstream_index >= 0 && static_cast<size_t>(d.upstream_index) < _upstreams.size();
        const int slot = in_table ? d.upstream_index : (d.upstream_index >= 0 && !d.credential_header.empty() ? -1 : 0);
        // The model the venue gets, as an override or a substitution: either spelling of the
        // same one is a keep, so a splice already done stands (12d). In force, a parked body's
        // is still to be rewritten; a streamed one's went spliced or as it was.
        const auto served = [&](std::string_view model, std::string_view sub, std::string_view sub_for) {
            return !model.empty() ? model : !sub.empty() && f.model.view() == sub_for ? sub : std::string_view{};
        };
        std::string_view now = f.model_override.view();
        if (f.model_in_path) now = f.path_model();
        else if (f.splice) now = whole.model_substituted ? f.subst_model.view() : std::string_view{};
        else if (f.body_parked) now = served(now, f.subst_model.view(), f.subst_for.view());
        // So for service_tier, speed and inference_geo: on a spliced body the venue gets the
        // override, or with none the client's own member, which the scan read (12e).
        const auto member = [&](std::string_view v, uint8_t bit) {
            return v.empty() && f.splice ? client_member(c, bit) : v;
        };
        using provider::BodyScan;
        // A hedge sends every leg the whole body, so one that went streaming is redone held.
        const bool same =
            (d.hedge_legs == 0 || f.body_parked) && slot == c->upstream_slot &&
            served(d.model, d.substitute_model, d.substitute_for) == now &&
            member(d.service_tier, BodyScan::kServiceTier) == member(f.tier_override.view(), BodyScan::kServiceTier) &&
            member(d.speed, BodyScan::kSpeed) == member(f.speed_override.view(), BodyScan::kSpeed) &&
            member(d.inference_geo, BodyScan::kInferenceGeo) == member(f.geo_override.view(), BodyScan::kInferenceGeo) &&
            d.anthropic_beta == f.beta_override.view() && d.credential_header == f.key_header.view();
        f.policy_tag = d.tag; // a tag alone changing is still a keep
        if (same)
        {
            f.confirm_outcome = ConfirmOutcome::Kept;
            if (!f.splice && !f.body_parked && f.model_in_path && whole.model_substituted)
                ++_stats.streamed_model_substituted;
            if (f.splice)
            {
                f.model_substituted = whole.model_substituted;
                if (whole.model_substituted) ++_stats.streamed_model_substituted;
                else if (f.subst_model.n > f.subst_for.n) ++_stats.streamed_model_padded;
                // The members went in; the client's own tier, if any, went as spaces.
                f.members_spliced = f.member_overrides();
                if (f.members_spliced) ++_stats.streamed_override_spliced;
                if (f.members_spliced & BodyScan::kServiceTier) f.asked_tier.set(client_member(c, BodyScan::kServiceTier));
            }
            if (!f.body_parked) return true;
            if (!apply_hedge(c, d)) return false; // the legs the confirm named, or the ones in force
        }
        else
        {
            // The venue holds part of a body it will never get the rest of: reset.
            f.confirm_outcome = ConfirmOutcome::Redone;
            if (c->up)
            {
                ++_stats.streamed_body_redone;
                if (f.splice && f.member_overrides()) ++_stats.streamed_override_redone;
                LB_WARN(ReqId{f.req_seq}, " confirm redid the request after ",
                        static_cast<uint64_t>(f.body_sent), " bytes went upstream; resetting it");
            }
            cut_body(c);
            if (!apply_decision(c, d)) return false;
        }
        dispatch_whole(c, t);
        return false;
    }

    void Engine::open_request(ClientConn* c, const net::http::Message& m, const HeadFacts& f,
                              int64_t t0) noexcept
    {
        // The one per-request reset, before any reply: even a 400 carries its own
        // sequence number and none of the previous request's state.
        c->req.begin(g_seq.fetch_add(1, std::memory_order_relaxed));
        c->upstream_slot = -1;
        c->req.f.ts_req_recvd = t0;
        c->msg = m;
        c->req.f.head = f;
        c->req.f.model_in_path = client_dialect_of(c->rbuf) == Dialect::Gemini;
        c->req.f.client_upload_ns = span_since(c->ts_first_byte, t0);
        c->req.f.client_conn_reused = c->ever_framed;
        c->req.f.client_conn_setup_ns = c->req.f.client_conn_reused ? 0 : span_since(c->ts_accepted, t0);
        // Whole, its last byte is in, and a pipelining client's next request already here
        // begins now. A streamed body's arrival runs on to its last byte (pump_body).
        if (c->rbuf.size() < m.total_len) return;
        c->req.f.ts_body_end = t0;
        c->ts_first_byte = c->rbuf.size() > m.total_len ? t0 : 0;
    }

    void Engine::take_request(ClientConn* c) noexcept
    {
        if (c->req.f.body_in_rbuf) return; // resent from there
        // A failover attempt runs on a copy put back in rbuf; drop it, or the next
        // failover inserts a second one. With no failover possible, nothing is kept.
        if (!c->req.saved.empty() || !_policy || _upstreams.size() < 2)
        {
            c->rbuf.erase(0, c->msg.total_len);
            return;
        }
        // The original bytes, for a failover: the rebuilt request is in this venue's
        // dialect. What was pipelined behind stays in rbuf.
        const size_t extra = c->rbuf.size() - c->msg.total_len;
        c->req.saved.swap(c->rbuf);
        c->rbuf.assign(c->req.saved, c->msg.total_len, extra);
        c->req.saved.resize(c->msg.total_len);
    }

    void Engine::dispatch(ClientConn* c) noexcept
    {
        // A hedge sends its legs instead, each a request of its own, while the request is
        // still whole at the front of rbuf.
        if (c->req.f.hedge_n && !c->req.f.hedged) return (void)spawn_hedge(c);
        // Resolved per attempt: a failover may land on a venue of another dialect. The
        // client dialect is read off the request line, back in rbuf for a failover.
        const Upstream& up = upstream_of(c);
        if (!c->req.f.body_streaming) resolve_substitution(c);
        {
            const TranslationPlan plan = resolve_dialect(c, up);
            if (!plan.ok) return error_respond(c, 400, plan.why);
            c->req.f.translate_body = plan.translate;
            c->req.f.effective_dialect = plan.venue;
            c->req.f.gemini_reply = provider::spec(up.dialect).wire == provider::Wire::Gemini;
        }
        // A key the policy named is this venue's only credential, or nothing is sent.
        NamedKey key;
        if (!c->req.f.key_header.empty())
            if (const Refusal r = named_key(std::string_view(c->rbuf.data(), c->msg.header_len), c->req.f.head,
                                            c->req.f.key_header.view(), up.dialect, key, up.auth_bearer))
                return error_respond(c, 400, r.why, r.detail);
        // A path model goes into a request line: a policy's that cannot stand there is refused.
        if (c->req.f.model_in_path)
        {
            if (const std::string_view pm = c->req.f.path_model();
                !pm.empty() && (!provider::path_model_ok(pm) ||
                                provider::gemini_path_model(target_of(c->rbuf)).at == std::string_view::npos))
                return error_respond(c, 500, "policy model unusable in a path");
            if (c->req.f.body_streaming)
                c->req.f.model_substituted = c->req.f.model_override.empty() && !c->req.f.path_model().empty();
        }
        // An OAuth token reaches Vertex only under a project; the publisher path the gateway
        // writes is express mode's, which takes an API key alone.
        if (up.dialect == UpstreamDialect::Vertex && up.auth_bearer &&
            (c->req.f.translate_body || !provider::gemini_vertex_scoped(target_of(c->rbuf))))
            return error_respond(c, 400, "vertex oauth without a project", refuse::kVertexProject);
        // Built before an upstream is acquired, so a bad body cannot leak a pooled one.
        note_build(c->msg.total_len);
        const std::string_view body = body_so_far(c);
        if (c->req.f.translate_body)
        {
            const std::string_view head(c->rbuf.data(), c->msg.header_len);
            const char* why = "";
            if (!build_translated_request(up, c->req.f.effective_dialect, body, head, c->req.f.head,
                                          _rebuild, _xlate, why, c->req.f.model_override.view(),
                                          &c->req.f.wants_usage, key, c->req.f.beta_override.view()))
            {
                // A credential is never echoed: it would land in a JSON body unescaped.
                if (why[0] == 't') return error_respond(c, 400, "translate", translate_failure(body));
                if (why[0] == 'n') return error_respond(c, 400, "no credential", refuse::kNoCredential);
                return error_respond(c, 400, "malformed credential",
                                     why[0] == 'b' ? refuse::kBedrockCredential : refuse::kCredential);
            }
        }
        else
        {
            // Byte-forward still rebuilds: Host must name the venue. The model is
            // spliced, so every other byte the client sent survives.
            std::string_view rewritten;
            if (!c->req.f.body_streaming && c->req.f.rewrites_body())
            {
                const ReqState& f = c->req.f;
                std::string_view had;
                // A path names the model, which build_forward rewrites there.
                if (!provider::apply_overrides(body,
                                               {f.model_in_path ? std::string_view{} : f.model_override.view(),
                                                f.tier_override.view(),
                                                f.speed_override.view(), f.geo_override.view()},
                                               &had, _xlate))
                    return error_respond(c, 400, "cannot apply route overrides");
                rewritten = _xlate;
                c->req.f.asked_tier.set(had); // `had` points into rbuf, reused before the sink
            }
            // A streamed body goes as far as it is known good, under the length declared,
            // and a head decision's last byte not before its confirm.
            if (c->req.f.body_streaming)
            {
                if (c->req.f.body_scanned &&
                    c->req.scan->feed(body, body.size() == c->msg.body_len) == provider::BodyScan::Verdict::Refused)
                    return refuse_body(c, c->req.scan->why());
                size_t safe = c->req.f.body_scanned ? c->req.scan->safe() : body.size();
                if (c->req.f.confirm_pending && safe == c->msg.body_len) --safe;
                c->req.f.body_sent = static_cast<uint32_t>(safe);
                // A substitution needs the model tracked from the first byte (12d), members
                // their client's own (12e), which overrides_stream saw to.
                c->req.f.splice = (c->req.f.subst_tracked && !c->req.f.subst_model.empty()) ||
                                  c->req.f.member_overrides() != 0;
                if (const char* bad = forward_parts(c, up, key, _rebuild)) return error_respond(c, 400, bad);
            }
            else if (const char* bad = build_forward(std::string_view(c->rbuf.data(), c->msg.header_len),
                                                     c->msg.header_len, c->req.f.head, _strip_headers,
                                                     up.host_hdr, up.base_path, rewritten.empty() ? body : rewritten,
                                                     _rebuild, rewritten.empty() ? c->msg.body_len : rewritten.size(),
                                                     key, beta_for(up, c->req.f), c->req.f.path_model(),
                                                     up.dialect))
                return error_respond(c, 400, bad);
        }

        c->req.f.phase = Phase::Dispatched; // from here until a response byte, failover may redo it
        c->req.f.body_parts = c->req.f.body_streaming;
        AcquireFail fail = AcquireFail::None;
        int local_err = 0;
        UpstreamConn* u = acquire_upstream(c->upstream_slot, &fail, &local_err);
        if (!u)
        {
            secure_clear(_rebuild); // a credential must not wait in the scratch
            // This host is short of something: another venue would need it too, and this
            // one is not to blame, so nothing fails over and the policy hears nothing.
            if (fail == AcquireFail::Local)
            {
                if (!c->req.saved.empty()) c->rbuf.erase(0, c->msg.total_len);
                return error_respond(c, 503, local_connect_why(local_err));
            }
            // Taken as a request that reached its venue is, so a first attempt fails over
            // like any other: the failover finds its bytes in `saved`.
            take_request(c);
            const char* why = fail == AcquireFail::Unresolved ? "no upstream (venue unresolved)"
                                                               : "no upstream (connect failed)";
            if (!upstream_failed(c, 502, why)) error_respond(c, 502, why);
            return;
        }

        LB_DEBUG(ReqId{c->req.f.req_seq}, " upstream ", *u, " pooled=", u->from_pool,
                 " dest=", upstream_of(u).ip, ":", upstream_of(u).port);
        // The swap hands the request over and takes the upstream's scrubbed, allocated
        // buffer back as the next scratch.
        u->out.stage().swap(_rebuild);
        take_request(c);
        pair(c, u);
        c->ever_framed = true;            // past the setup deadline for good
        c->ts_client_activity = now_ns(); // and the idle clock restarts here
        c->req.f.ts_req_built = now_ns();
        c->req.f.ts_progress = c->req.f.ts_req_built;
        // Per attempt: a failover's venue is not the last one's. Pooled means no handshake.
        c->req.f.ts_wire_ready = u->connected ? c->req.f.ts_req_built : 0;
        c->req.f.ts_up_sent = c->req.f.ts_body_sent = 0;
        c->req.f.upstream_pooled = u->connected;
        c->req.f.upstream_ip = u->up_addr.sin_addr.s_addr;
        mark_first_send(u);

        if (!u->connected) return; // the connect's completion sends it
        send_upstream(u);
    }

    // The staged reply is on the wire: account for it and take the next request.
    void Engine::finish(ClientConn* c) noexcept
    {
        // Only a staged reply finishes. A drained interim 100 Continue, or a write event
        // with nothing owed, is no request: counting one wiped the real one's state.
        if (c->req.f.phase != Phase::Replying) return;
        // An error reply is counted in errors; its stamps are unset, so not in a histogram.
        // A hedge leg is no client's request.
        if (!c->close_after_resp && !c->req.f.reply_kept && !c->shadow)
        {
            const int64_t ts_resp_sent = now_ns(); // t6
            if (ts_resp_sent - _t_start >= _warmup_ns)
            {
                // The split the timing headers use, so the two cannot disagree; t5 does
                // not enter it, so t4 stands in.
                const TimingSplit sp = split_of(c->req.f, c->req.f.ts_up_recvd);
                const int64_t resp_ns = ts_resp_sent - c->req.f.ts_up_recvd;
                if (sp.req_path_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(sp.req_path_ns));
                if (sp.connect_ns >= 0) _stats.connect.record(static_cast<uint64_t>(sp.connect_ns));
                if (resp_ns >= 0) _stats.resp_path.record(static_cast<uint64_t>(resp_ns));
                if (sp.req_path_ns >= 0 && resp_ns >= 0)
                    _stats.overhead.record(static_cast<uint64_t>(sp.req_path_ns + resp_ns));
                ++_stats.requests;
            }
        }
        if (_sink && !c->req.f.sunk) sink_emit(c, status_of(c->out.bytes()), /*streamed=*/false);
        LB_DEBUG(ReqId{c->req.f.req_seq}, " status=", status_of(c->out.bytes()),
                 " bytes=", c->out.bytes().size(), " tokens_in=", c->req.f.tok.in,
                 " tokens_out=", c->req.f.tok.out, " keep_alive=", c->msg.keep_alive, " on ", *c);
        c->out.clear();
        c->req.f.phase = Phase::Idle;
        const bool close_now = c->shadow || c->close_after_resp || !c->msg.keep_alive;
        c->msg = net::http::Message{};
        if (close_now && c->req.discard) c->req.close_on_drain = true; // after the body it still sends
        else if (close_now) return _ops->close(*c);
        on_client_bytes(c); // a pipelined next request is framed from rbuf, with no read
    }

    void Engine::respond(ClientConn* client) noexcept
    {
        // A hedge leg's reply while its client waits wins the client if it is a success, or if
        // no other leg is left to answer; any other is only recorded (GATEWAY-INTERNALS.md 13).
        if (client->shadow && client->hedge_of && client->hedge_of->hedge_open)
        {
            const int status = status_of(client->out.bytes());
            if ((status >= 200 && status < 300) || leg_alone(client)) client = adopt_leg(client);
        }
        switch (_ops->send(*client))
        {
            case Sent::Done: finish(client); return;
            case Sent::Failed: client_gone(client); return;
            case Sent::Queued: return; // the completion finishes it
        }
    }

    ConnBase* Engine::forget(ClientConn* c) noexcept
    {
        sink_unfinished(c); // before unpair: a stream's token counts live on its venue leg
        if (c->req.f.body_streaming && c->up) reset_venue(c->up);
        stop_body(c);
        c->req.f.body_parked = false;
        UpstreamConn* u = unpair(c); // the other leg never keeps a pointer to a freed one
        _clients->remove(c);
        LB_DEBUG("close ", *c, " clients=", _clients->size());
        c->doomed = true;
        if (c->shadow)
        {
            if (ClientConn* p = c->hedge_of)
                for (ClientConn*& l : p->hedge_legs)
                    if (l == c) l = nullptr;
            c->hedge_of = nullptr;
            _dead_shadows.push_back(c);
            return u;
        }
        _doomed->add(c);
        if (c->hedge_open) settle_hedge(c); // a client gone takes its legs with it
        return u;
    }

    ConnBase* Engine::forget(UpstreamConn* u) noexcept
    {
        unpair(u);
        _pool->remove(*u);
        _upconns->remove(u);
        LB_DEBUG("upstream close ", *u, " pool=", pooled_upstream_count()); // after the erase
        u->doomed = true;
        _doomed->add(u);
        return nullptr;
    }

    ConnBase* Engine::forget(ConnBase& c) noexcept
    {
        return visit(c, [this](auto& k) { return forget(&k); });
    }

    void Engine::abort_pair(ClientConn* client) noexcept
    {
        if (!client) return;
        const bool leg = client->shadow; // read first: a close may free the client at once
        _ops->close(*client); // and its upstream
        if (!leg) ++_stats.errors;
    }

    void Engine::fail_request(ClientConn* client, int code, const char* why) noexcept
    {
        if (!upstream_failed(client, code, why)) error_respond(client, code, why);
    }

    void Engine::error_respond(ClientConn* client, int code, const char* why, const char* detail,
                               bool keep) noexcept
    {
        if (!client || client->doomed) return;
        // Every reply of our own is WARN, a client's 4xx too: DEBUG is not in a default build.
        // An authentication refusal names where it came from.
        const std::string peer = (code == 401 || code == 403) ? peer_of(client->fd)
                                                              : std::string{};
        // Past dispatch, a 5xx names the venue: the one that failed, or that was refused.
        if (code >= 500 && client->req.f.phase >= Phase::Dispatched)
            LB_WARN(ReqId{client->req.f.req_seq}, " reply ", code, " ", why, " on ", *client, " venue=",
                    static_cast<int64_t>(client->upstream_slot));
        else
            LB_WARN(ReqId{client->req.f.req_seq}, " reply ", code, " ", why, " on ", *client,
                    peer.empty() ? "" : " peer=", peer);
        // A reply already begun cannot be followed by a second one: close instead.
        if (client->req.f.phase >= Phase::Streaming || !client->out.idle())
        {
            abort_pair(client);
            return;
        }
        // We're replying to the client ourselves, so drop any in-flight upstream.
        if (client->req.f.body_streaming) cut_body(client);
        else if (UpstreamConn* u = unpair(client)) _ops->close(*u);
        client->req.f.body_parked = false;
        append_error(client->out.stage(), code, detail, keep && client->msg.keep_alive);
        client->req.f.phase = Phase::Replying;
        // Closes once the reply flushes; kept, once the rest of the body is dropped.
        client->req.f.reply_kept = keep;
        client->close_after_resp = !keep;
        count_error(client);
        respond(client);
    }

} // namespace llmbridge::detail
