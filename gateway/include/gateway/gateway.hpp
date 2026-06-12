// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Gateway — the whole llmbridge proxy in one self-contained class. A single-
// threaded, non-blocking epoll event loop: accept clients, frame requests,
// (optionally) translate the provider dialect, forward over a keep-alive
// upstream pool, read the response, translate back, write to the client. No
// framework, no external dependencies — just net (sockets + HTTP framing) and
// provider (dialect translation).
//
// Per-request added latency (the headline metric) is four wall-clock stamps:
//   added = (upstream_request_sent - client_request_received)
//         + (client_response_sent  - upstream_response_received)
// i.e. request-path + response-path work, with the upstream wait excluded.

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gateway/metrics.hpp"
#include "net/http.hpp"
#include "net/uring.hpp" // self-guarded by LLMBRIDGE_HAVE_URING
#include "provider/translate.hpp"

namespace llmbridge
{
    // Dialect translation: None = byte-forward (OpenAI-compatible upstreams);
    // the rest translate OpenAI<->provider on the way out and back.
    enum class TranslateMode
    {
        None,
        Anthropic,
        Gemini,
        Cohere,
    };

    // Event-loop backend. Auto = io_uring when the kernel supports it, else epoll.
    // (Phase 0: the selection is plumbed and probed; the io_uring loop itself
    // arrives in Phase 1, so today every choice runs the epoll loop.)
    enum class IoBackend
    {
        Auto,
        Epoll,
        Uring,
    };

    // Per-fd connection state (client or upstream).
    struct Connection
    {
        // Live-instance counter — an assertable invariant: every Connection the
        // gateway allocates must eventually be freed (no acquired-upstream or
        // client leaks). Tests check s_live returns to baseline after a Gateway is
        // destroyed, catching leaks deterministically without a sanitizer.
        Connection() noexcept { s_live.fetch_add(1, std::memory_order_relaxed); }
        ~Connection() { s_live.fetch_sub(1, std::memory_order_relaxed); }
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        inline static std::atomic<long> s_live{0};

        int fd = -1;
        bool is_client = true;
        bool write_armed = false;     // EPOLLOUT currently registered
        bool connected = false;       // upstream-only: non-blocking connect done
        bool request_pending = false; // client-only: full request buffered, awaiting forward
        bool doomed = false;          // closed this epoll batch; deleted after the batch

        uint64_t id = 0; // client conns: stable id; upstream conns: 0

        std::string rbuf;
        std::string wbuf;
        size_t woff = 0;

        Connection* peer = nullptr; // linked counterpart for the in-flight request
        http::Message msg{};

        // Latency stamps (ns), held on the CLIENT conn for the active request.
        int64_t ts_req_recvd = 0;
        int64_t ts_up_sent = 0;
        int64_t ts_up_recvd = 0;

        // io_uring backend only: submitted-but-uncompleted SQEs referencing this
        // conn (it is freed only when this hits 0), and a fixed staging buffer the
        // in-flight RECV lands into (allocated once, then reused — no per-recv
        // zero-fill; the received bytes are appended to rbuf on completion).
        int inflight = 0;
        std::string rstage;
        // io_uring stale-connection handling: was this upstream reused from the
        // keep-alive pool (so a failure before any response is retry-eligible), and
        // has this request already been retried once on a fresh connection.
        bool from_pool = false;
        bool retried = false;
    };

    struct Stats
    {
        Histogram overhead;  // total proxy-added latency per request
        Histogram req_path;  // client-recv -> upstream-sent
        Histogram resp_path; // upstream-recv -> client-sent
        uint64_t requests = 0;
        uint64_t errors = 0;
        uint64_t upstream_conns_opened = 0;
        uint64_t upstream_retries = 0; // stale pooled connection -> resent on a fresh one
    };

    class Gateway
    {
    public:
        Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                int64_t warmup_ns = 0, TranslateMode translate = TranslateMode::None,
                IoBackend io = IoBackend::Auto);
        ~Gateway();

        Gateway(const Gateway&) = delete;
        Gateway& operator=(const Gateway&) = delete;

        // Run the event loop until request_stop() is called. Returns 0.
        int run();

        // Async-signal-safe-ish: flips a flag the loop observes within one poll tick.
        void request_stop() noexcept { _stop = true; }

        const Stats& stats() const noexcept { return _stats; }

        // The configured event-loop backend (the requested mode; Auto stays Auto).
        IoBackend io_backend() const noexcept { return _io; }

        // Actual bound listen port (resolves ephemeral when 0 was requested).
        uint16_t bound_port() const noexcept;

    private:
        void ep_add_read(Connection* c) noexcept;
        void ep_arm_write(Connection* c) noexcept;
        void ep_disarm_write(Connection* c) noexcept;

        void on_accept() noexcept;
        void on_client_readable(Connection* c) noexcept;
        void on_client_writable(Connection* c) noexcept;
        void on_upstream_writable(Connection* c) noexcept;
        void on_upstream_readable(Connection* c) noexcept;

        // Forward the client's buffered (and optionally translated) request to an
        // upstream connection. Called inline once a full request is framed.
        void forward(Connection* client) noexcept;
        // Write the buffered response to the client; close out accounting.
        void respond(Connection* client) noexcept;
        void finish_client_response(Connection* c) noexcept;

        Connection* acquire_upstream() noexcept;
        void release_upstream(Connection* u) noexcept;
        // Epoll: a pooled upstream failed before any response (provider dropped the
        // idle keep-alive). Resend the request once on a fresh connection.
        bool retry_upstream(Connection* u) noexcept;
        void close_client(Connection* c) noexcept;
        void close_upstream(Connection* u) noexcept;
        void abort_pair(Connection* client) noexcept;

        bool drain_read(Connection* c) noexcept;
        bool pump_write(Connection* c, bool* done) noexcept;
        Connection* client_by_id(uint64_t id) noexcept;

        // ── backend dispatch ────────────────────────────────────────────────
        int run_epoll();

#ifdef LLMBRIDGE_HAVE_URING
        // io_uring backend (Phase 1): completion-driven mirror of the epoll state
        // machine, sharing the Connection struct + translate/framing helpers. A
        // conn is freed only when its `inflight` SQEs all complete.
        int run_uring();
        bool u_next_sqe(struct io_uring_sqe** out) noexcept; // get an SQE, flushing if full
        void u_submit_accept() noexcept;
        void u_submit_timer() noexcept;
        bool u_submit_recv(Connection* c) noexcept;
        bool u_submit_send(Connection* c) noexcept;
        bool u_submit_connect(Connection* u) noexcept;
        void u_on_cqe(uint64_t user_data, int res, uint32_t flags) noexcept;
        void u_on_accept(int res, uint32_t flags) noexcept;
        void u_on_recv(Connection* c, int res) noexcept;
        void u_on_send(Connection* c, int res) noexcept;
        void u_on_connect(Connection* u, int res) noexcept;
        void u_forward(Connection* c) noexcept;
        void u_on_response(Connection* u, const http::Message& m) noexcept;
        void u_finish_client(Connection* c) noexcept;
        Connection* u_acquire_upstream() noexcept;
        void u_release_upstream(Connection* u) noexcept;
        // A pooled upstream failed before sending any response (it was almost
        // certainly closed idle by the provider). Resend the request once on a
        // fresh connection. Returns true if a retry was issued.
        bool u_retry_upstream(Connection* u) noexcept;
        void u_close(Connection* c) noexcept;
        void u_abort_pair(Connection* client) noexcept;
        void u_maybe_free(Connection* c) noexcept;

        net::uring::Ring _ring;
        sockaddr_in _upstream_addr{};
        struct __kernel_timespec _uring_ts{};
        long _uring_inflight = 0; // global in-flight SQEs (drain barrier on stop)
        bool _draining = false;   // post-stop: completions just decrement, no re-arm
#endif

        uint16_t _listen_port;
        std::string _upstream_ip;
        uint16_t _upstream_port;
        int64_t _warmup_ns;
        TranslateMode _translate;
        IoBackend _io;
        bool _uring_active = false;

        int _epfd = -1;
        int _listen_fd = -1;
        Connection* _listen_conn = nullptr;

        std::unordered_map<uint64_t, Connection*> _clients;
        std::vector<Connection*> _idle_upstreams;
        uint64_t _next_client_id = 1;
        std::vector<Connection*> _doomed; // closed mid-batch, freed after the batch

        int64_t _t_start = 0;
        Stats _stats;
        volatile bool _stop = false;
    };
} // namespace llmbridge
