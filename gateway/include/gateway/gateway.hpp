// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Gateway: the whole llmbridge proxy in one self-contained class. A single-
// threaded, non-blocking epoll event loop: accept clients, frame requests,
// (optionally) translate the provider dialect, forward over a keep-alive
// upstream pool, read the response, translate back, write to the client. No
// framework, no external dependencies: just net (sockets + HTTP framing) and
// provider (dialect translation).
//
// Per-request added latency (the headline metric). LATENCY.md is the normative
// definition of every number below; this is the summary. Seven stamps:
//
//   t0 ts_req_recvd   client request fully framed
//   t1 ts_req_built   upstream request built (nothing sent yet)
//   t2 ts_wire_ready  socket can carry the request (== t1 when pooled)
//   t3 ts_up_sent     request handed to the kernel by write()
//   t4 ts_up_recvd    provider's response received
//   t5                response built, client write begins
//   t6                response fully flushed
//
//   added = (t1-t0) + (t3-t2)   request path: our compute + the upstream write
//         + (t4 -> t6)          response path: translate back + write + flush
//
// Excluded on purpose: the provider's own time (t3->t4), and the TCP connect +
// TLS handshake (t1->t2, recorded separately as Stats::connect). Without the
// gateway the client's own stack pays that same handshake, so it cancels in the
// subtraction that defines "added", see LATENCY.md §1. An earlier version
// folded connect into the request path, which was harmless against a warm pooled
// mock and badly wrong against a cold real provider. A live single-request run
// reported 52.66 ms of "request path" that was 99.9% handshake.

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gateway/metrics.hpp"
#include "net/http.hpp"
#include "net/tls.hpp"   // self-guarded by LLMBRIDGE_HAVE_TLS
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

    // TLS towards the upstream. Declared unconditionally so callers don't need
    // ifdefs; when the build has no TLS support (LLMBRIDGE_TLS=OFF), enabling it
    // makes run() fail fast with an error instead of silently speaking plaintext.
    //
    // The DESIGN INVARIANT the whole integration hangs off: `Connection::rbuf` and
    // `Connection::wbuf` hold PLAINTEXT always. TLS interposes at the socket edge
    // only. Ciphertext lives in `Connection::tls_out` on the way out and inside
    // the Session's BIO on the way in. Nothing downstream (HTTP framing, SSE pump,
    // retry-resend of wbuf on a stale pooled conn) knows TLS exists.
    struct TlsConfig
    {
        bool enabled = false;
        std::string sni_host; // DNS name for SNI + certificate hostname verification
        std::string ca_file;  // empty = system trust store (tests pass their own CA)
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
        // Live-instance counter, an assertable invariant: every Connection the
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
        bool write_armed = false;     // epoll backend only: EPOLLOUT currently registered
        bool connected = false;       // upstream-only: non-blocking connect done
        bool request_pending = false; // client-only: full request buffered, awaiting forward
        bool doomed = false;          // closed this epoll batch; deleted after the batch
        bool close_after_resp = false; // client-only: this is an error reply, so close once it flushes

        uint64_t id = 0; // client conns: stable id; upstream conns: 0

        // Non-streaming chunked decode state, PER CONNECTION instead of shared:
        // it must persist across reads so the decoder is fed only new bytes
        // (see net::http::ResponseDecoder; the shared-scratch form was quadratic).
        net::http::ResponseDecoder rdec;

        std::string rbuf;
        std::string wbuf;
        size_t woff = 0;

        Connection* peer = nullptr; // linked counterpart for the in-flight request
        net::http::Message msg{};

        // Latency stamps (ns), held on the CLIENT conn for the active request.
        int64_t ts_req_recvd = 0;
        // When the upstream request bytes were BUILT (translated + auth applied),
        // i.e. the end of our request-side compute. Separates our work from the
        // TCP connect + TLS handshake that may follow before the bytes go out:
        // measured live, a cold connection to api.anthropic.com put 56 ms of setup
        // inside what was labelled "gateway overhead", against 47-63 us once the
        // connection was pooled. Reporting those as one number is indefensible.
        int64_t ts_req_built = 0;
        // When the upstream socket was READY to carry the request: equal to
        // ts_req_built for a pooled connection, later by the TCP+TLS handshake for a
        // fresh one. Splitting here matters because ts_req_built -> ts_up_sent is
        // NOT one thing: on a pooled conn it is purely the write() syscall (measured
        // 4.4 us p50), which IS our cost, while on a cold conn it is dominated by a
        // ~50 ms handshake, which is not. Folding them together either inflates the
        // added-latency claim or flatters it, depending which case you sample.
        int64_t ts_wire_ready = 0;
        int64_t ts_up_sent = 0;
        int64_t ts_up_recvd = 0;
        // Last time this request saw ANY upstream progress (request forwarded, or
        // bytes received). The idle-timeout sweep measures against this.
        int64_t ts_up_activity = 0;

        // io_uring backend only: submitted-but-uncompleted SQEs referencing this
        // conn; it is freed only when this hits 0. (Multishot recv lands data in a
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
        bool read_paused = false;    // epoll backend only: upstream EPOLLIN paused
                                     // (client-write backpressure; the uring pump
                                     // bounds wpending with kStreamBufCap instead)
        bool wants_usage = false;    // client set stream_options.include_usage
        std::unique_ptr<provider::AnthropicToOpenAiSse> sse; // Anthropic->OpenAI SSE translator
        net::http::ChunkDecoder chunkdec;                          // decodes the upstream chunked body
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
        // keep-alives are reaped after kIdleUpstreamNs. Providers drop them on
        // their own schedule, and a pooled corpse costs a retry to discover.
        int64_t ts_pooled = 0;

#ifdef LLMBRIDGE_HAVE_TLS
        // ── TLS state (upstream conns only; null = plaintext) ────────────────
        // The Session outlives requests: it stays attached across keep-alive pool
        // cycles, so a pooled reuse pays no second handshake.
        std::unique_ptr<net::tls::Session> tls;
        // Ciphertext awaiting the socket. wbuf (plaintext) is NOT what gets
        // written for a TLS conn. For these, `woff` counts plaintext bytes fed
        // into the Session, and tls_out/tls_out_off track the encrypted write.
        // The request's plaintext stays intact in wbuf for stale-conn retry.
        std::string tls_out;
        size_t tls_out_off = 0;
#endif
    };

    struct Stats
    {
        // The t0-t6 stamps, grouped three ways (LATENCY.md §4, note this grouping
        // splits at t2, where the header grouping does not). Keeping
        // `connect` OUT of req_path and overhead is the whole point: a cold TCP+TLS
        // handshake is 50 ms and would otherwise sit inside a metric that claims to
        // measure OUR work and is sized for microseconds. Measured live: a single
        // cold request reported req-path p50 = 52.66 ms **[overflow!]**, of which
        // ~52.6 ms was the handshake and ~60 us was the gateway.
        Histogram overhead;  // req_path + resp_path: everything the gateway does
        Histogram req_path;  // framing/translate/auth PLUS the write() to the upstream
        Histogram connect;   // TCP + TLS handshake only; exactly 0 on a pooled conn
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
        // stalled provider pins a client connection and two fds forever, the
        // classic slow-loris-by-upstream. Applies to the whole in-flight request
        // and, once streaming, to the gap between events.
        static constexpr int64_t kDefaultUpstreamIdleNs = 120LL * 1000 * 1000 * 1000; // 120 s

        // Keep-alive pool bounds. The pool was unbounded until streaming reuse landed;
        // a streaming gateway pools roughly one upstream per concurrent stream, so
        // without a bound it is an fd leak in slow motion (4k streams => 4k idle fds
        // pinned indefinitely). Excess conns are closed instead of pooled, and idle
        // entries are reaped after kIdleUpstreamNs on the loop's periodic tick.
        //
        // SIZE THIS GENEROUSLY. The cap must exceed the number of upstreams in flight
        // at peak, or it stops being a bound and becomes a reuse *killer*: once the
        // pool is full every release closes its connection, so the next request must
        // reconnect. A first cut of 256 did exactly that and cost the non-streaming
        // path 2.4x its throughput (90k RPS target: 32,210 achieved at 256 versus
        // 77,282 at 8192 and 78,445 before the pool existed); the gateway was opening
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
                int64_t upstream_idle_ns = kDefaultUpstreamIdleNs,
                TlsConfig tls = {}, bool timing_headers = false);
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
        // force -ENOBUFS deterministically. At the shipped size that branch was never
        // reached even at 8192 concurrent streams, so without this hook the recovery path
        // would be untestable and could rot silently. Not a tuning knob users need.
        void set_uring_buf_count_for_test(unsigned n) noexcept { _uring_buf_count = n; }

    private:
        // NAMING: the backend a method belongs to is part of its name:
        //   ep_*   epoll-only    (reachable only from run_epoll)
        //   ur_*   io_uring-only (reachable only from run_uring)
        //   plain  shared by both loops. sweep_idle, the tls_* pump helpers
        //
        // A call that crosses the prefixes is a bug: neither backend's teardown,
        // write-arming or completion handling is valid in the other. The payoff is
        // that the check is a grep instead of a call-graph walk
        //   grep -n 'ur_[a-z_]*(' gateway.cpp | grep ep_
        // Exactly one crossing existed when this convention was introduced
        // (ur_forward calling the epoll error responder); it had been invisible for
        // as long as the epoll half of the class was unprefixed. Twins share a verb
        // so the counterpart is greppable: ep_stream_flush / ur_stream_flush.
        //
        // The uring prefix is ur_, not u_, because `u` is the parameter name for an
        // upstream connection and `void u_tls_kick_send(Connection* u)` used both
        // meanings at once. See DESIGN.md "Naming conventions".
        void ep_add_read(Connection* c) noexcept;
        void ep_arm_write(Connection* c) noexcept;
        void ep_disarm_write(Connection* c) noexcept;

        void ep_on_accept() noexcept;
        void ep_on_client_readable(Connection* c) noexcept;
        void ep_on_client_writable(Connection* c) noexcept;
        void ep_on_upstream_writable(Connection* c) noexcept;
        void ep_on_upstream_readable(Connection* c) noexcept;

        // Forward the client's buffered (and optionally translated) request to an
        // upstream connection. Called inline once a full request is framed.
        void ep_forward(Connection* client) noexcept;
        // Write the buffered response to the client; close out accounting.
        void ep_respond(Connection* client) noexcept;
        void ep_finish_client(Connection* c) noexcept;

        // ── Streaming pump (epoll) ──────────────────────────────────────────
        void ep_pause_read(Connection* c) noexcept;   // drop EPOLLIN (backpressure)
        void ep_resume_read(Connection* c) noexcept;  // restore EPOLLIN
        void ep_begin_stream(Connection* u, const net::http::ResponseHead& h) noexcept; // enter streaming
        void ep_stream_pump(Connection* u) noexcept;      // decode+translate new upstream body bytes
        void ep_stream_on_upstream_eof(Connection* u) noexcept; // upstream closed: finish the stream
        void ep_stream_flush(Connection* client) noexcept;      // write buffered SSE, apply backpressure
        void ep_finalize_stream(Connection* client) noexcept;   // stream done: tear down + count

        // Abort any request whose upstream has been silent for longer than
        // _upstream_idle_ns. Runs on the loop's existing periodic tick (epoll's
        // epoll_wait timeout / io_uring's timer CQE), so it costs nothing extra.
        // `uring` selects the matching teardown primitives.
        void sweep_idle(bool uring) noexcept;

        Connection* ep_acquire_upstream() noexcept;
        void ep_release_upstream(Connection* u) noexcept;
        // Epoll: a pooled upstream failed before any response (provider dropped the
        // idle keep-alive). Resend the request once on a fresh connection.
        bool ep_retry_upstream(Connection* u) noexcept;
        void ep_close_client(Connection* c) noexcept;
        void ep_close_upstream(Connection* u) noexcept;
        void ep_abort_pair(Connection* client) noexcept;
        // Reply to the client with a structured HTTP error (400 malformed request /
        // 502 upstream failure) and close, instead of a bare TCP reset. Tears down
        // any in-flight upstream peer.
        void ep_error_respond(Connection* client, int code) noexcept;

        bool ep_drain_read(Connection* c) noexcept;
        bool ep_pump_write(Connection* c, bool* done) noexcept;

#ifdef LLMBRIDGE_HAVE_TLS
        // ── TLS plumbing (upstream side only; every helper is a no-op-safe
        //     building block the two backends share) ──────────────────────────
        // Attach a fresh Session to a new upstream conn (SNI + hostname from
        // _tls.sni_host). Returns false on failure, treated like connect refusal.
        bool tls_attach(Connection* u) noexcept;
        // Move whatever ciphertext the Session has pending into u->tls_out.
        void tls_pump_out(Connection* u) noexcept;
        // Feed ciphertext from the socket. Drains resulting plaintext into u->rbuf,
        // advances the handshake, and pushes wbuf plaintext once the handshake
        // completes. Returns false on a fatal TLS error.
        bool tls_feed(Connection* u, const char* p, size_t n) noexcept;
        // Push un-fed request plaintext (wbuf[woff..]) into the Session.
        void tls_push_request(Connection* u) noexcept;
        // True when the request is fully on the wire: all plaintext fed AND all
        // ciphertext flushed. This is the TLS analogue of ep_pump_write's `done`,
        // and the point where ts_up_sent is stamped.
        bool tls_request_flushed(const Connection* u) const noexcept;

        // Epoll only: write tls_out to the socket (non-blocking), arming EPOLLOUT
        // on a partial write. False = socket error.
        bool ep_tls_flush(Connection* u, bool* done) noexcept;
        // Epoll only: TLS-aware replacement for ep_drain_read on upstream conns.
        bool ep_tls_drain_read(Connection* u) noexcept;

#ifdef LLMBRIDGE_HAVE_URING
        // io_uring only: submit a SEND for tls_out if non-empty and none in
        // flight (send_inflight serializes; handshake flights and request bytes
        // must not interleave on the wire).
        void ur_tls_flush(Connection* u) noexcept;
#endif
#endif

        // ── backend dispatch ────────────────────────────────────────────────
        int run_epoll();

#ifdef LLMBRIDGE_HAVE_URING
        // io_uring backend (Phase 1): completion-driven mirror of the epoll state
        // machine, sharing the Connection struct + translate/framing helpers. A
        // conn is freed only when its `inflight` SQEs all complete.
        int run_uring();
        bool ur_next_sqe(struct io_uring_sqe** out) noexcept; // get an SQE, flushing if full
        void ur_submit_accept() noexcept;
        void ur_submit_timer() noexcept;
        bool ur_arm_recv(Connection* c) noexcept; // arm a multishot recv (provided buffers)
        bool ur_submit_send(Connection* c) noexcept;
        bool ur_submit_connect(Connection* u) noexcept;
        void ur_submit_cancel(int fd) noexcept; // cancel all in-flight ops on a fd
        void ur_on_cqe(uint64_t user_data, int res, uint32_t flags) noexcept;
        void ur_on_accept(int res, uint32_t flags) noexcept;
        void ur_on_recv(Connection* c, int res, uint32_t flags) noexcept;
        void ur_on_send(Connection* c, int res) noexcept;
        void ur_on_connect(Connection* u, int res) noexcept;
        void ur_forward(Connection* c) noexcept;
        void ur_try_forward_buffered(Connection* c) noexcept; // forward a framed request if idle
        void ur_on_response(Connection* u, const net::http::ResponseHead& h,
                           std::string_view body_buf, size_t total_len) noexcept;
        void ur_finish_client(Connection* c) noexcept;
        // io_uring streaming pump: a completion-driven mirror of the epoll pump,
        // with serialized sends (one SEND SQE in flight) and a bounded buffer.
        void ur_begin_stream(Connection* u, const net::http::ResponseHead& h) noexcept;
        void ur_stream_pump(Connection* u) noexcept;
        void ur_stream_on_upstream_eof(Connection* u) noexcept;
        void ur_stream_flush(Connection* client) noexcept; // send pending bytes, or finalize
        void ur_finalize_stream(Connection* client) noexcept;
        Connection* ur_acquire_upstream() noexcept;
        void ur_release_upstream(Connection* u) noexcept;
        // A pooled upstream failed before sending any response (it was almost
        // certainly closed idle by the provider). Resend the request once on a
        // fresh connection. Returns true if a retry was issued.
        bool ur_retry_upstream(Connection* u) noexcept;
        void ur_close(Connection* c) noexcept;
        void ur_abort_pair(Connection* client) noexcept;
        void ur_error_respond(Connection* client, int code) noexcept; // uring mirror of ep_error_respond
        void ur_maybe_free(Connection* c) noexcept;

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
        TlsConfig _tls;                    // upstream TLS (enabled => _tls_ctx inited in ctor)
        bool _timing_headers = false;      // emit x-llmbridge-* timing on responses
        std::string _upstream_host_hdr;    // Host: value for rebuilt upstream requests
        // Decode buffer for CHUNKED upstream responses (see net::http::parse_response).
        // One per loop, not per connection: the loop is single-threaded and a
        // response is framed and consumed entirely within one event, so there is no
        // overlap. Reused across requests so the chunked path, which is the REAL
        // provider path, not an edge case. Does not allocate per response.
#ifdef LLMBRIDGE_HAVE_TLS
        net::tls::Context _tls_ctx;        // one SSL_CTX shared by all upstream sessions
        bool _tls_ctx_ok = false;
#endif
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
        // is enough; it gates loop continuation, no other state depends on it.
        std::atomic<bool> _stop{false};
    };
} // namespace llmbridge
