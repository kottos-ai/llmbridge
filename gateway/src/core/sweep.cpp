// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The periodic sweep: heartbeat, pool reaping and every timeout, in one pass.

#include "gateway/gateway.hpp"

#include "core/limits.hpp"
#include "core/registry.hpp"
#include "stream.hpp"

namespace llmbridge
{
    using namespace detail;

    // The sweep's three teardowns, on whichever backend runs. `doomed` keeps every
    // one of them a no-op on a connection already closed.
    void Gateway::close_conn(Connection* c) noexcept
    {
#ifdef LLMBRIDGE_HAVE_URING
        if (_active_backend == 2) { ur_close(c); return; }
#endif
        if (c->is_client) ep_close_client(c);
        else ep_close_upstream(c);
    }

    void Gateway::abort_request(Connection* client) noexcept
    {
#ifdef LLMBRIDGE_HAVE_URING
        if (_active_backend == 2) { ur_abort_pair(client); return; }
#endif
        ep_abort_pair(client);
    }

    void Gateway::fail_request(Connection* client, int code, const char* why) noexcept
    {
#ifdef LLMBRIDGE_HAVE_URING
        if (_active_backend == 2)
        {
            if (!ur_upstream_failed(client, code, why)) ur_error_respond(client, code, why);
            return;
        }
#endif
        if (!ep_upstream_failed(client, code, why)) ep_error_respond(client, code, why);
    }

    // Plaintext not yet on the wire or ciphertext not yet written, sends in flight included.
    static bool owes_client(const Connection* c) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        if (!c->tls_out.idle()) return true;
#endif
        return !c->out.idle();
    }

    // At most ~20 times a second, on the loop's periodic tick, so an idle gateway costs
    // one walk of its clients per tick. A client yet to be answered gets a real 504; a
    // live stream (headers already sent) is cut without a terminal [DONE], so the
    // client sees a truncated stream, not a fabricated clean finish.
    void Gateway::sweep_idle() noexcept
    {
        const int64_t now = now_ns();
        if (now - _last_sweep_ns < 50'000'000LL) return;
        _last_sweep_ns = now;

        // Counted before anything below closes, so the line reads as it did when it
        // was taken first; logged after the walk that counts it. It runs even with
        // every timeout off, and its write(2) happens 0.003 times a second.
        const bool beat = _heartbeat_ns > 0 &&
                          (_last_heartbeat_ns == 0 || now - _last_heartbeat_ns >= _heartbeat_ns);
        const size_t clients = _clients->size();
        const size_t pooled = pooled_upstream_count();
        size_t in_flight = 0;

        // A SIGUSR1 dump, serviced here so the worker prints its own stats and nobody
        // reads a Histogram it does not own.
        if (_dump.exchange(false, std::memory_order_relaxed))
            print_profile(stderr, "live profile (worker snapshot, gateway still running)");

        // Providers close idle keep-alives on their own schedule, and finding a corpse
        // costs a request its retry, so expired ones go first. Only those are visited.
        if (_pool_idle_ns > 0)
            _pool->reap(now - _pool_idle_ns, [this](Connection* u) { close_conn(u); });

        apply_reresolved();

        // Backwards, so the swap a close makes in the registry moves an already-visited
        // client into this slot. A teardown removes at most its own client.
        for (size_t i = _clients->size(); i-- > 0;)
        {
            if (i >= _clients->size()) continue;
            Connection* c = (*_clients)[i];
            const bool busy = c->peer != nullptr || c->req.f.streaming;
            in_flight += busy;
            sweep_client(c, now, busy);
        }

        if (beat)
        {
            _last_heartbeat_ns = now;
            LB_INFO("heartbeat clients=", clients, " in_flight=", in_flight,
                    " pooled_upstreams=", pooled, " requests=", _stats.requests);
        }
    }

    // Each deadline is independent of the others: the setup deadline holds even with
    // the upstream timeout off, so a peer cannot hold a connection by sending nothing.
    void Gateway::sweep_client(Connection* c, int64_t now, bool busy) noexcept
    {
        // Never framed a request. Measured before this existed: 50 half-open
        // connections, each holding a slot with a partial request, and none closed.
        if (!c->ever_framed)
        {
            if (c->ts_accepted == 0 || now - c->ts_accepted <= _client_setup_ns) return;
            ++_stats.client_setup_timeouts;
            LB_WARN("TIMEOUT client never framed a request ", *c,
                    " after_ns=", now - c->ts_accepted, " limit_ns=", _client_setup_ns);
            close_conn(c);
            return;
        }
        // Established and quiet. Skipped while a request is in flight, because a slow
        // provider is not an idle client.
        if (!busy)
        {
            if (_client_idle_ns <= 0 || c->ts_client_activity == 0 ||
                now - c->ts_client_activity <= _client_idle_ns)
                return;
            ++_stats.client_idle_timeouts;
            LB_WARN("TIMEOUT client idle ", *c, " after_ns=", now - c->ts_client_activity,
                    " limit_ns=", _client_idle_ns);
            close_conn(c);
            return;
        }
        // A fresh connect that never became wire-ready. Separate from the idle timeout,
        // which starts at the request write, never on a socket still retrying SYNs.
        if (Connection* u = c->peer;
            _connect_ns > 0 && u && !u->wire_ready && u->ts_accepted != 0 &&
            now - u->ts_accepted > _connect_ns)
        {
            ++_stats.connect_timeouts;
            LB_WARN(ReqId{c->req.f.req_seq}, " TIMEOUT upstream connect ", *u,
                    " after_ns=", now - u->ts_accepted, " limit_ns=", _connect_ns);
            note_connect_failure(u->upstream_slot, "timed out");
            unpair(u);
            close_conn(u);
            fail_request(c, 502, "upstream connect timeout");
            return;
        }
        if (_upstream_idle_ns <= 0 || c->req.f.ts_progress == 0 ||
            now - c->req.f.ts_progress <= _upstream_idle_ns)
            return;
        // Neither leg moved. With bytes still owed to the client it is the client that
        // stopped reading, and a paused upstream is waiting on it.
        const bool stalled = owes_client(c);
        ++(stalled ? _stats.client_idle_timeouts : _stats.upstream_timeouts);
        LB_WARN(ReqId{c->req.f.req_seq},
                stalled ? " TIMEOUT client stalled " : " TIMEOUT upstream silent ", *c,
                " after_ns=", now - c->req.f.ts_progress, " limit_ns=", _upstream_idle_ns,
                " streaming=", c->req.f.streaming);
        if (!c->req.f.streaming) { fail_request(c, 504, "upstream idle timeout"); return; }
        stream_truncate(c); // the head is out; truncate honestly, with no [DONE]
        abort_request(c);
    }

} // namespace llmbridge
