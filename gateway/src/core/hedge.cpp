// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// A hedged request (GATEWAY-INTERNALS.md 13): each leg is a request of its own on a client
// connection with no socket, run by the same lifecycle as any other, and cut once the
// decision's own leg has its answer.

#include "engine.hpp"

#include "core/io.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"

#include <algorithm>
#include <iterator>

namespace llmbridge::detail
{
    // Every leg is checked before any is sent: a bad one is the policy's 500, and no venue
    // has heard of the request.
    bool Engine::apply_hedge(ClientConn* c, const Decision& d) noexcept
    {
        ReqState& f = c->req.f;
        f.hedge_n = 0;
        if (d.hedge_legs == 0) return true;
        const char* bad = nullptr;
        if (!d.hedge || d.hedge_legs > kMaxHedgeLegs) bad = "policy hedge unusable";
        // One client key would otherwise reach every venue, each another company's.
        else if (d.credential_header.empty()) bad = "policy hedge names no key for its own venue";
        else if (!d.substitute_model.empty()) bad = "policy hedge with a model substitution";
        // A path names the model (Gemini): each leg's goes in a request line, so all are
        // checked here, before any leg is sent, as dispatch checks a single venue's.
        else if (f.model_in_path && !d.model.empty() && !provider::path_model_ok(d.model))
            bad = "policy model unusable in a path";
        for (uint8_t i = 0; !bad && i < d.hedge_legs; ++i)
        {
            const HedgeLeg& l = d.hedge[i];
            HedgeLegCopy& k = f.hedge[i];
            k.venue = l.upstream_index;
            if (l.upstream_index < 0 || static_cast<size_t>(l.upstream_index) >= _upstreams.size())
                bad = "policy hedge leg past the table";
            else if (!key_header_ok(l.credential_header))
                bad = "policy hedge leg names no usable key";
            else if (f.model_in_path && !l.model.empty() && !provider::path_model_ok(l.model))
                bad = "policy hedge leg model unusable in a path";
            else if (!l.anthropic_beta.empty() && !beta_token_ok(l.anthropic_beta))
                bad = "policy hedge leg anthropic_beta unusable";
            else if (!k.model.set(l.model) || !k.tier.set(l.service_tier) || !k.speed.set(l.speed) ||
                     !k.geo.set(l.inference_geo))
                bad = "policy hedge leg override longer than the gateway holds";
            k.beta.set(l.anthropic_beta);
            k.key.set(l.credential_header);
        }
        if (bad) return error_respond(c, 500, bad), false;
        f.hedge_n = d.hedge_legs;
        return true;
    }

    // Leg `i` of `c`'s hedge, 0 being the decision's own: the request as framed and the
    // client's state (the same arrival: stamps, head facts, captures and model) with the leg's
    // own seq, venue, overrides and key. Copied from checked values, so nothing here refuses.
    ClientConn* Engine::new_leg(ClientConn* c, uint8_t i) noexcept
    {
        auto* s = new ClientConn();
        s->shadow = true;
        s->hedge_of = c;
        s->id = c->id;
        s->ever_framed = true;
        s->ts_accepted = c->ts_accepted;
        s->ts_client_activity = now_ns();
        s->msg = c->msg;
        s->rbuf.assign(c->rbuf, 0, c->msg.total_len);
        s->req.f = c->req.f;
        ReqState& f = s->req.f;
        f.req_seq = g_seq.fetch_add(1, std::memory_order_relaxed);
        f.hedge_leg = i;
        f.hedge_n = 0; // a leg has no legs
        s->upstream_slot = c->upstream_slot;
        if (i > 0)
        {
            const HedgeLegCopy& l = c->req.f.hedge[i - 1];
            s->upstream_slot = l.venue;
            f.model_override = l.model;
            f.tier_override = l.tier;
            f.speed_override = l.speed;
            f.geo_override = l.geo;
            f.beta_override = l.beta;
            f.key_header = l.key;
        }
        _clients->add(s);
        ++_stats.hedge_legs;
        return s;
    }

    // Every leg, the decision's own included, is a request of its own; the client sends
    // nothing until one of them wins it (adopt_leg).
    bool Engine::spawn_hedge(ClientConn* c) noexcept
    {
        ReqState& f = c->req.f;
        // Held to its last byte by construction (decide_on_head, confirm_body); a leg given
        // part of a body would run a request nobody sent, so this is refused, never sent.
        if (c->rbuf.size() < c->msg.total_len)
        {
            LB_ERROR(ReqId{f.req_seq}, " hedge reached dispatch with its body incomplete; refusing");
            f.hedge_n = 0;
            error_respond(c, 500, "hedge without the whole body");
            return false;
        }
        f.hedged = true;
        f.hedge_group = f.req_seq;
        ++_stats.hedged_requests;
        // All made and linked before any is sent, so a leg failing at once is never taken for
        // the last one standing.
        const uint8_t n = static_cast<uint8_t>(f.hedge_n + 1);
        ClientConn* legs[kMaxHedgeLegs + 1] = {};
        for (uint8_t i = 0; i < n; ++i) legs[i] = c->hedge_legs[i] = new_leg(c, i);
        // The request leaves the client's buffer; a pipelined one behind it waits there.
        c->rbuf.erase(0, c->msg.total_len);
        f.body_in_rbuf = f.body_parked = false;
        f.phase = Phase::Dispatched;
        c->hedge_open = true;
        c->ever_framed = true;
        c->ts_client_activity = now_ns();
        for (ClientConn* s : legs)
            if (s && !s->doomed) dispatch(s); // doomed: the race was settled while sending
        return true;
    }

    // Leg `s` has won: its request, its venue and what it staged become the client's, and
    // every other leg is cut.
    ClientConn* Engine::adopt_leg(ClientConn* s) noexcept
    {
        ClientConn* c = s->hedge_of;
        for (ClientConn*& l : c->hedge_legs)
            if (l == s) l = nullptr;
        s->hedge_of = nullptr;
        s->req.f.hedge_won = true;
        // What the client's connection still owes or drops belongs to it, not to a request.
        const uint64_t discard = c->req.discard;
        const bool close_on_drain = c->req.close_on_drain;
        std::swap(c->req, s->req);
        c->req.discard = discard;
        c->req.close_on_drain = close_on_drain;
        std::swap(c->sse_xlate, s->sse_xlate);
        c->out.stage().append(s->out.wire()); // a leg holds its bytes until it wins
        s->out.clear();
        c->upstream_slot = s->upstream_slot;
        c->close_after_resp = s->close_after_resp;
        if (UpstreamConn* u = unpair(s)) pair(c, u);
        s->req.f.sunk = true; // it holds the client's own state now, no leg's
        _ops->close(*s);
        settle_hedge(c);
        if (c->req.f.error_owed) ++_stats.errors; // the client got the failure
        LB_DEBUG(ReqId{c->req.f.req_seq}, " hedge leg ", static_cast<int64_t>(c->req.f.hedge_leg), " won on venue ",
                 static_cast<int64_t>(c->upstream_slot), " for ", *c);
        return c;
    }

    bool Engine::leg_alone(const ClientConn* s) noexcept
    {
        for (const ClientConn* l : s->hedge_of->hedge_legs)
            if (l && l != s) return false;
        return true;
    }

    void Engine::settle_hedge(ClientConn* c) noexcept
    {
        c->hedge_open = false;
        ClientConn* legs[kMaxHedgeLegs + 1];
        std::copy(std::begin(c->hedge_legs), std::end(c->hedge_legs), legs); // each cut unlinks itself
        for (ClientConn* s : legs)
            if (s) cut_leg(s);
    }

    void Engine::count_error(ClientConn* c) noexcept
    {
        if (c->shadow) c->req.f.error_owed = true; // counted only if it wins its client
        else ++_stats.errors;
    }

    void Engine::cut_leg(ClientConn* s) noexcept
    {
        s->req.f.hedge_cut = true;
        ++_stats.hedge_legs_cut;
        LB_DEBUG(ReqId{s->req.f.req_seq}, " hedge leg cut on venue ", static_cast<int64_t>(s->upstream_slot));
        // Mid-request: never pooled, and a reset is how the venue learns to stop.
        if (s->up) reset_venue(s->up);
        _ops->close(*s);
    }

    void Engine::close_shadow(ClientConn* s) noexcept
    {
        if (s->doomed) return;
        // Every leg leaves a record, whatever ended it; a cut one was answered nothing.
        ReqState& f = s->req.f;
        if (_sink && !f.sunk && f.phase != Phase::Idle)
            sink_emit(s, f.hedge_cut ? 0 : f.streaming ? 200 : 502, f.streaming);
        ClientConn* c = s->hedge_of;
        if (ConnBase* u = forget(s)) _shadow_io.inner->close(*u);
        if (!c || !c->hedge_open) return;
        for (const ClientConn* l : c->hedge_legs)
            if (l) return;
        // The last leg gone with nothing to show the client: the gateway's 502, and no record
        // of it, since every leg left its own.
        c->hedge_open = false;
        c->req.f.sunk = true;
        error_respond(c, 502, "no hedge leg answered");
    }

    void Engine::bury_shadows() noexcept
    {
        for (ClientConn* s : _dead_shadows) delete s;
        _dead_shadows.clear();
    }

} // namespace llmbridge::detail
