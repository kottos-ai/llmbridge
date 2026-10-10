// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Connection, the per-fd state both event loops share, and its log rendering.
// Which buffer holds what, who may free a connection on each backend, and how the
// two TLS legs fit: GATEWAY-INTERNALS.md.

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/req.hpp"
#include "gateway/venue.hpp"
#include "net/http.hpp"
#include "net/log.hpp"
#include "net/tls.hpp" // self-guarded by LLMBRIDGE_HAVE_TLS
#include "provider/openai.hpp"
#include "provider/sse.hpp"

namespace llmbridge
{
    /// Per-fd state, client or upstream (`is_client`); buffer names are the gateway's view:
    ///   c->rbuf request in   u->wbuf request out
    ///   u->rbuf response in  c->wbuf response out
    /// Ownership, and which backend may free a connection when: GATEWAY-INTERNALS.md 2, 5b, 7.
    struct Connection
    {
        /// Live instances, so a test can assert every Connection is freed.
        Connection() noexcept { s_live.fetch_add(1, std::memory_order_relaxed); }
        ~Connection() { s_live.fetch_sub(1, std::memory_order_relaxed); }
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        inline static std::atomic<long> s_live{0};

        int fd = -1;
        bool is_client = true;
        /// Index in the Registry that holds it: live clients, live upstreams or doomed.
        uint32_t slot = UINT32_MAX;

        /// Upstream table index, -1 for none: an upstream's own venue (for its pool), or the
        /// venue serving a client's request in flight (for the response's dialect).
        int upstream_slot = -1;
        sockaddr_in up_addr{};
        /// Log identity, process-unique for either kind, unlike `id` (0 on upstreams).
        uint64_t log_inst = net::log::next_instance();
        bool write_armed = false;     // epoll backend only: EPOLLOUT currently registered
        bool connected = false;       // upstream-only: non-blocking connect done
        bool wire_ready = false;
        /// Closed, not yet freed: GATEWAY-INTERNALS.md section 7.
        bool doomed = false;
        bool close_after_resp = false; // client-only: this is an error reply, so close once it flushes

        uint64_t id = 0; // client conns: stable id; upstream conns: 0

        /// Non-streaming chunked decode, fed only new bytes across reads.
        net::http::ResponseDecoder rdec;

        std::string rbuf;
        std::string wbuf;

        /// Bytes of wbuf on the socket, or fed to the Session: GATEWAY-INTERNALS.md 2b.
        size_t woff = 0;

        /// Accept and first request byte on a client; socket creation and first
        /// response byte on an upstream.
        int64_t ts_accepted = 0;
        int64_t ts_first_byte = 0;
        /// Client conns: when this connection last completed a request, for the idle
        /// deadline; the setup deadline covers one that never framed anything.
        int64_t ts_client_activity = 0;
        bool ever_framed = false;
        /// Total length of the request being buffered, learned from its headers on
        /// the first partial read, or 0 when none is in progress.
        size_t client_frame_want = 0;
        /// An interim `100 Continue`'s ciphertext has not fully left the socket.
        bool client_interim_inflight = false;

        Connection* peer = nullptr; // the other leg of the request in flight; pair()/unpair() only
        net::http::Message msg{};
        /// Client conns: the request in flight, reset by req.begin() at framing.
        RequestCtx req;

        /// io_uring only: SQEs still referencing this conn. GATEWAY-INTERNALS.md 5b.
        int inflight = 0;
        /// Reused from the pool, so a failure before any response may retry once.
        bool from_pool = false;
        bool retried = false;

        /// Epoll back-pressure, written only by ep_pause/resume_read. GATEWAY-INTERNALS.md 6c.
        bool read_paused = false;

        /// The SSE translator, reset per stream; by value, so a stream allocates none.
        provider::AnthropicToOpenAiSse sse_xlate;
        /// A translated reply body, before it is framed into `wbuf`; capacity kept.
        std::string xlate_scratch{};
        /// io_uring streams: output staged while a send SQE reads `wbuf`.
        std::string wpending;

        /// io_uring: a send SQE reads this connection's buffer, which must not move. Set
        /// only by ur_submit_send. GATEWAY-INTERNALS.md section 5b.
        bool send_inflight = false;

        /// Upstream conns only: when this connection entered the idle pool, reaped
        /// after _pool_idle_ns, and when this request took it, which bounds the retry.
        int64_t ts_pooled = 0;
        int64_t ts_pool_taken = 0;
        /// UpstreamPool's links and membership; nothing else writes them.
        Connection* pool_newer = nullptr;
        Connection* pool_older = nullptr;
        bool pooled = false;
        /// Upstream conns: the venue sent EOF, so this connection is never pooled.
        bool peer_eof = false;

#ifdef LLMBRIDGE_HAVE_TLS
        /// Null = plaintext. Kept across pool cycles: a pooled reuse pays no handshake.
        std::unique_ptr<net::tls::Session> tls;
        /// Ciphertext awaiting the socket; `woff` counts encrypted, this sent (10b).
        std::string tls_out;
        size_t tls_out_off = 0;
#endif
    };

    /// The only writers of `peer`, both directions at once, so a link is never one-sided.
    inline void pair(Connection* c, Connection* u) noexcept
    {
        c->peer = u;
        u->peer = c;
    }
    /// Either leg; returns the other, now unlinked, or null.
    inline Connection* unpair(Connection* c) noexcept
    {
        Connection* p = c->peer;
        if (p) p->peer = nullptr;
        c->peer = nullptr;
        return p;
    }

    /// Renders `ClientConnection#42(fd=17,cid=2)` for `LB_INFO("closed ", *c)`. Never prints
    /// rbuf or wbuf: they hold the customer's request, credential included.
    inline void log_put(net::log::Line& l, const Connection& c)
    {
        l.put(net::log::Id{c.is_client ? "ClientConnection" : "UpstreamConnection", c.log_inst});
        l.put("(fd=");
        l.put(static_cast<int64_t>(c.fd));
        if (c.is_client && c.id)
        {
            l.put(",cid=");
            l.put(c.id);
        }
        l.put(')');
    }

    /// A request is not an object with a lifetime, so it is named by the sequencer:
    /// `Request#123`, the same number the timing header carries.
    struct ReqId
    {
        uint64_t seq;
    };
    inline void log_put(net::log::Line& l, ReqId r)
    {
        l.put(net::log::Id{"Request", r.seq});
    }
} // namespace llmbridge
