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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gateway/metrics.hpp"
#include "net/http.hpp"
#include "net/uring.hpp" // self-guarded by LLMBRIDGE_HAVE_URING
#include "provider/sse.hpp"
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
        bool close_after_resp = false; // client-only: this is an error reply — close once it flushes

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
        // Last time this request saw ANY upstream progress (request forwarded, or
        // bytes received). The idle-timeout sweep measures against this.
        int64_t ts_up_activity = 0;

        // io_uring backend only: submitted-but-uncompleted SQEs referencing this
        // conn — it is freed only when this hits 0. (Multishot recv lands data in a
        // shared provided-buffer pool, so there is no per-connection recv buffer.)
        int inflight = 0;
        // io_uring stale-connection handling: was this upstream reused from the
        // keep-alive pool (so a failure before any response is retry-eligible), and
        // has this request already been retried once on a fresh connection.
        bool from_pool = false;
        bool retried = false;

        // ── Streaming (SSE), held on the CLIENT conn for the active request ──
        // Set when the upstream response is text/event-stream: the gateway then
        // pumps (decode chunked -> translate -> write to client) instead of
        // buffering a whole body. Streamed client responses are close-delimited.
        bool streaming = false;
        bool stream_chunked = false; // upstream body uses chunked transfer-encoding
        bool stream_ended = false;   // final [DONE] emitted; close once the client drains
        bool read_paused = false;    // upstream EPOLLIN paused (client-write backpressure)
        bool wants_usage = false;    // client set stream_options.include_usage
        std::unique_ptr<provider::AnthropicToOpenAiSse> sse; // Anthropic->OpenAI SSE translator
        http::ChunkDecoder chunkdec;                          // decodes the upstream chunked body
        // io_uring streaming only: translated output accumulates in `wpending`
        // while a client SEND SQE is in flight, so `wbuf` (the SEND's buffer) is
        // never reallocated under the kernel's feet. `send_inflight` serializes
        // sends (two concurrent SENDs on one fd would interleave).
        std::string wpending;
        bool send_inflight = false;
        // Upstream said keep-alive on the streaming response, so the connection may
        // be pooled once the body's terminal chunk has been consumed. Held on the
        // CLIENT conn alongside the rest of the per-request streaming state.
        bool stream_keep_alive = false;

        // Upstream conns only: when this connection entered the idle pool. Idle
        // keep-alives are reaped after kIdleUpstreamNs — providers drop them on
        // their own schedule, and a pooled corpse costs a retry to discover.
        int64_t ts_pooled = 0;
    };

    struct Stats
    {
        Histogram overhead;  // total proxy-added latency per request
        Histogram req_path;  // client-recv -> upstream-sent
        Histogram resp_path; // upstream-recv -> client-sent
        uint64_t requests = 0;
        uint64_t errors = 0;
        uint64_t upstream_conns_opened = 0;
        uint64_t upstream_retries = 0;  // stale pooled connection -> resent on a fresh one
        uint64_t upstream_reused = 0;   // requests served on a pooled keep-alive conn
        uint64_t upstream_timeouts = 0; // requests/streams aborted on upstream inactivity
        uint64_t stream_pauses = 0;     // epoll: upstream reads paused for client backpressure
        uint64_t uring_enobufs = 0;     // io_uring: provided-buffer pool momentarily empty
    };

    class Gateway
    {
    public:
        // `upstream_idle_ns` bounds how long a request may sit with NO bytes from
        // the upstream before the gateway gives up (0 = disabled). Without it a
        // stalled provider pins a client connection and two fds forever — the
        // classic slow-loris-by-upstream. Applies to the whole in-flight request
        // and, once streaming, to the gap between events.
        static constexpr int64_t kDefaultUpstreamIdleNs = 120LL * 1000 * 1000 * 1000; // 120 s

        // Keep-alive pool bounds. The pool was unbounded until streaming reuse landed;
        // a streaming gateway pools roughly one upstream per concurrent stream, so
        // without a bound it is an fd leak in slow motion (4k streams => 4k idle fds
        // pinned indefinitely). Excess conns are closed rather than pooled, and idle
        // entries are reaped after kIdleUpstreamNs on the loop's periodic tick.
        //
        // SIZE THIS GENEROUSLY. The cap must exceed the number of upstreams in flight
        // at peak, or it stops being a bound and becomes a reuse *killer*: once the
        // pool is full every release closes its connection, so the next request must
        // reconnect. A first cut of 256 did exactly that and cost the non-streaming
        // path 2.4x its throughput (90k RPS target: 32,210 achieved at 256 versus
        // 77,282 at 8192 and 78,445 before the pool existed) — the gateway was opening
        // more upstream connections than it served requests. git-bisected; do not lower
        // this without re-running ./bench/saturate.sh with BACKENDS=4.
        //
        // The real reclaim mechanism is the idle timeout, not the cap: the cap only
        // has to stop pathological growth, so err high.
        static constexpr size_t kMaxIdleUpstreams = 8192;
        static constexpr int64_t kIdleUpstreamNs = 30LL * 1000 * 1000 * 1000; // 30 s

        Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                int64_t warmup_ns = 0, TranslateMode translate = TranslateMode::None,
                IoBackend io = IoBackend::Auto,
                int64_t upstream_idle_ns = kDefaultUpstreamIdleNs);
        ~Gateway();

        Gateway(const Gateway&) = delete;
        Gateway& operator=(const Gateway&) = delete;

        // Run the event loop until request_stop() is called. Returns 0.
        int run();

        // Async-signal-safe-ish: flips a flag the loop observes within one poll tick.
        void request_stop() noexcept { _stop.store(true, std::memory_order_relaxed); }

        const Stats& stats() const noexcept { return _stats; }

        // The configured event-loop backend (the requested mode; Auto stays Auto).
        IoBackend io_backend() const noexcept { return _io; }

        // Actual bound listen port (resolves ephemeral when 0 was requested).
        uint16_t bound_port() const noexcept;

        // Size of the io_uring provided-buffer pool (power of two), or 0 for the default.
        // Must be called before run(). Exists so a test can shrink the pool far enough to
        // force -ENOBUFS deterministically — at the shipped size that branch was never
        // reached even at 8192 concurrent streams, so without this hook the recovery path
        // would be untestable and could rot silently. Not a tuning knob users need.
        void set_uring_buf_count_for_test(unsigned n) noexcept { _uring_buf_count = n; }

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

        // ── Streaming pump (epoll) ──────────────────────────────────────────
        void ep_pause_read(Connection* c) noexcept;   // drop EPOLLIN (backpressure)
        void ep_resume_read(Connection* c) noexcept;  // restore EPOLLIN
        void begin_stream(Connection* u, const http::ResponseHead& h) noexcept; // enter streaming
        void stream_pump(Connection* u) noexcept;      // decode+translate new upstream body bytes
        void stream_on_upstream_eof(Connection* u) noexcept; // upstream closed: finish the stream
        void stream_flush(Connection* client) noexcept;      // write buffered SSE, apply backpressure
        void finalize_stream(Connection* client) noexcept;   // stream done: tear down + count

        // Abort any request whose upstream has been silent for longer than
        // _upstream_idle_ns. Runs on the loop's existing periodic tick (epoll's
        // epoll_wait timeout / io_uring's timer CQE), so it costs nothing extra.
        // `uring` selects the matching teardown primitives.
        void sweep_idle(bool uring) noexcept;

        Connection* acquire_upstream() noexcept;
        void release_upstream(Connection* u) noexcept;
        // Epoll: a pooled upstream failed before any response (provider dropped the
        // idle keep-alive). Resend the request once on a fresh connection.
        bool retry_upstream(Connection* u) noexcept;
        void close_client(Connection* c) noexcept;
        void close_upstream(Connection* u) noexcept;
        void abort_pair(Connection* client) noexcept;
        // Reply to the client with a structured HTTP error (400 malformed request /
        // 502 upstream failure) and close, instead of a bare TCP reset. Tears down
        // any in-flight upstream peer.
        void error_respond(Connection* client, int code) noexcept;

        bool drain_read(Connection* c) noexcept;
        bool pump_write(Connection* c, bool* done) noexcept;

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
        bool u_arm_recv(Connection* c) noexcept; // arm a multishot recv (provided buffers)
        bool u_submit_send(Connection* c) noexcept;
        bool u_submit_connect(Connection* u) noexcept;
        void u_submit_cancel(int fd) noexcept; // cancel all in-flight ops on a fd
        void u_on_cqe(uint64_t user_data, int res, uint32_t flags) noexcept;
        void u_on_accept(int res, uint32_t flags) noexcept;
        void u_on_recv(Connection* c, int res, uint32_t flags) noexcept;
        void u_on_send(Connection* c, int res) noexcept;
        void u_on_connect(Connection* u, int res) noexcept;
        void u_forward(Connection* c) noexcept;
        void u_try_forward_buffered(Connection* c) noexcept; // forward a framed request if idle
        void u_on_response(Connection* u, const http::Message& m) noexcept;
        void u_finish_client(Connection* c) noexcept;
        // io_uring streaming pump — completion-driven mirror of the epoll pump,
        // with serialized sends (one SEND SQE in flight) and a bounded buffer.
        void u_begin_stream(Connection* u, const http::ResponseHead& h) noexcept;
        void u_stream_pump(Connection* u) noexcept;
        void u_stream_on_eof(Connection* u) noexcept;
        void u_stream_kick(Connection* client) noexcept; // send pending bytes, or finalize
        void u_finalize_stream(Connection* client) noexcept;
        Connection* u_acquire_upstream() noexcept;
        void u_release_upstream(Connection* u) noexcept;
        // A pooled upstream failed before sending any response (it was almost
        // certainly closed idle by the provider). Resend the request once on a
        // fresh connection. Returns true if a retry was issued.
        bool u_retry_upstream(Connection* u) noexcept;
        void u_close(Connection* c) noexcept;
        void u_abort_pair(Connection* client) noexcept;
        void u_error_respond(Connection* client, int code) noexcept; // uring mirror of error_respond
        void u_maybe_free(Connection* c) noexcept;

        net::uring::Ring _ring;
        net::uring::BufRing _bufring; // provided-buffer pool for multishot recv
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
        int64_t _upstream_idle_ns;         // 0 = no idle timeout
        unsigned _uring_buf_count = 0;     // 0 = kBufCount default (test hook only)
        int64_t _last_sweep_ns = 0;
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
        // Set cross-thread by the signal/timer thread, observed by the worker loop:
        // a std::atomic (not volatile) for a correct memory-model tripwire. Relaxed
        // is enough — it gates loop continuation, no other state depends on it.
        std::atomic<bool> _stop{false};
    };
} // namespace llmbridge
