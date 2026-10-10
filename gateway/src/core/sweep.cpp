// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The periodic sweep: heartbeat, pool reaping and every timeout.

#include "gateway/gateway.hpp"

#include "loop.hpp"
#include "stream.hpp"

#include <vector>

namespace llmbridge
{
    using namespace detail;

    // Abort requests whose upstream has gone silent. Runs on the loop's existing
    // periodic tick, so an idle gateway costs one cheap scan per tick. A client
    // that hasn't been answered yet gets a real 504; a live stream (headers already
    // sent) is closed without a terminal [DONE], so the client sees a truncated
    // stream instead of a fabricated clean finish.
    void Gateway::sweep_idle(bool uring) noexcept
    {
        const int64_t now = now_ns();
        if (now - _last_sweep_ns < 50'000'000LL) return; // at most ~20 sweeps/sec
        _last_sweep_ns = now;

        // Placed above every return below, so a build with the idle timeouts off still
        // reports. One line per interval against ~20 sweeps a second: the comparison is
        // the cost, and the write(2) happens 0.003 times a second.
        if (_heartbeat_ns > 0 &&
            (_last_heartbeat_ns == 0 || now - _last_heartbeat_ns >= _heartbeat_ns))
        {
            _last_heartbeat_ns = now;
            size_t in_flight = 0;
            for (const auto& [id, c] : _clients)
                if (!c->doomed && (c->peer != nullptr || c->req.f.streaming)) ++in_flight;
            LB_INFO("heartbeat clients=", _clients.size(), " in_flight=", in_flight,
                    " pooled_upstreams=", pooled_upstream_count(),
                    " requests=", _stats.requests);
        }

        // A SIGUSR1 dump, serviced here so the worker prints its own stats and nobody
        // reads a Histogram it does not own.
        if (_dump.exchange(false, std::memory_order_relaxed))
            print_profile(stderr, "live profile (worker snapshot, gateway still running)");


        // Reap idle pooled upstreams. Providers close idle keep-alives on their own
        // schedule, and discovering a corpse costs a request its retry, so drop them
        // first. Pooled conns have peer == nullptr, so the in-flight scan below skips
        // them and would otherwise hold them forever.
        for (auto& pool : _idle_upstreams)
        for (size_t i = 0; i < pool.size();)
        {
            Connection* u = pool[i];
            if (_pool_idle_ns > 0 && u->ts_pooled != 0 && now - u->ts_pooled > _pool_idle_ns)
            {
                pool.erase(pool.begin() + static_cast<long>(i));
#ifdef LLMBRIDGE_HAVE_URING
                if (uring) { ur_close(u); continue; }
#endif
                ep_close_upstream(u);
                continue;
            }
            ++i;
        }

        // Drop clients that never finished setting up. This runs before the
        // upstream-idle early return below, deliberately: the two are unrelated,
        // and a deployment with the upstream timeout disabled still must not let a
        // peer hold a connection open forever by sending nothing.
        //
        // Measured before this existed: 50 half-open connections, each holding a
        // slot with a partial request, and the gateway closed none of them. On a
        // loopback sidecar that is nearly harmless. On an internet-facing listener
        // it is a resource-exhaustion vector that costs an attacker one packet.
        {
            std::vector<Connection*> unfinished;
            for (auto& [id, c] : _clients)
            {
                if (c->doomed || c->ever_framed || c->ts_accepted == 0) continue;
                if (now - c->ts_accepted > _client_setup_ns) unfinished.push_back(c);
            }
            for (Connection* c : unfinished)
            {
                ++_stats.client_setup_timeouts;
                LB_WARN("TIMEOUT client never framed a request ", *c,
                        " after_ns=", now - c->ts_accepted, " limit_ns=", _client_setup_ns);
#ifdef LLMBRIDGE_HAVE_URING
                if (uring) { ur_close(c); continue; }
#endif
                ep_close_client(c);
            }
        }

        // An established client that has gone quiet. Separate from the setup deadline
        // above, which only reaps connections that never framed anything: once
        // ever_framed latches, that check stops applying for the connection's life, so
        // without this a client could send one request and then hold a descriptor
        // forever. Skipped while a request or stream is in flight, because a slow
        // provider is not an idle client.
        if (_client_idle_ns > 0)
        {
            std::vector<Connection*> quiet;
            for (auto& [id, c] : _clients)
            {
                if (c->doomed || !c->ever_framed || c->ts_client_activity == 0) continue;
                if (c->peer != nullptr || c->req.f.streaming) continue; // in flight
                if (now - c->ts_client_activity > _client_idle_ns) quiet.push_back(c);
            }
            for (Connection* c : quiet)
            {
                ++_stats.client_idle_timeouts;
                LB_WARN("TIMEOUT client idle ", *c, " after_ns=", now - c->ts_client_activity,
                        " limit_ns=", _client_idle_ns);
#ifdef LLMBRIDGE_HAVE_URING
                if (uring) ur_close(c);
                else
#endif
                    ep_close_client(c);
            }
        }

        apply_reresolved();

        // A fresh connect that never became wire-ready. Independent of the idle
        // timeout below, which starts at the request write and so never starts on a
        // socket the kernel is still retrying SYNs on.
        if (_connect_ns > 0)
        {
            std::vector<Connection*> stuck;
            for (auto& [id, c] : _clients)
            {
                if (c->doomed) continue;
                const Connection* u = c->peer;
                if (!u || u->wire_ready || u->ts_accepted == 0) continue;
                if (now - u->ts_accepted > _connect_ns) stuck.push_back(c);
            }
            for (Connection* c : stuck)
            {
                Connection* u = c->peer;
                ++_stats.connect_timeouts;
                LB_WARN(ReqId{c->req.f.req_seq}, " TIMEOUT upstream connect ", *u,
                        " after_ns=", now - u->ts_accepted, " limit_ns=", _connect_ns);
                note_connect_failure(u->upstream_slot, "timed out");
                c->peer = nullptr;
                u->peer = nullptr;
#ifdef LLMBRIDGE_HAVE_URING
                if (uring)
                {
                    ur_close(u);
                    if (!ur_upstream_failed(c, 502, "upstream connect timeout"))
                        ur_error_respond(c, 502, "upstream connect timeout");
                    continue;
                }
#endif
                ep_close_upstream(u);
                if (!ep_upstream_failed(c, 502, "upstream connect timeout"))
                    ep_error_respond(c, 502, "upstream connect timeout");
            }
        }

        // The in-flight abort below is gated on the upstream idle timeout; pool
        // eviction above is not; they are independent settings.
        if (_upstream_idle_ns <= 0) return;

        // Collect first: the teardown below erases from _clients.
        std::vector<Connection*> stale;
        for (auto& [id, c] : _clients)
        {
            if (c->doomed) continue;
            const bool in_flight = c->peer != nullptr || c->req.f.streaming;
            if (!in_flight || c->req.f.ts_up_activity == 0) continue;
            if (now - c->req.f.ts_up_activity > _upstream_idle_ns) stale.push_back(c);
        }
        for (Connection* c : stale)
        {
            ++_stats.upstream_timeouts;
            LB_WARN(ReqId{c->req.f.req_seq}, " TIMEOUT upstream silent ", *c,
                    " after_ns=", now - c->req.f.ts_up_activity, " limit_ns=", _upstream_idle_ns,
                    " streaming=", c->req.f.streaming);
            const bool streaming = c->req.f.streaming;
            if (streaming)
            {
                // Response headers are already out; truncate honestly (no [DONE]).
                stream_truncate(c);
            }
#ifdef LLMBRIDGE_HAVE_URING
            if (uring)
            {
                if (streaming) ur_abort_pair(c);
                else if (!ur_upstream_failed(c, 504, "upstream idle timeout")) ur_error_respond(c, 504, "upstream idle timeout");
                continue;
            }
#else
            (void)uring;
#endif
            if (streaming) ep_abort_pair(c);
            else if (!ep_upstream_failed(c, 504, "upstream idle timeout")) ep_error_respond(c, 504, "upstream idle timeout");
        }
    }

} // namespace llmbridge
