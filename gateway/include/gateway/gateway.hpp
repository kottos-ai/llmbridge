// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Gateway: the whole llmbridge proxy in one class. A single-threaded, non-blocking
// event loop (epoll or io_uring): accept, frame, optionally translate the provider
// dialect, forward over a keep-alive upstream pool, read back, translate, reply.
// The per-socket state is in connection.hpp; how it all fits, GATEWAY-INTERNALS.md.
//
// Per-request added latency is the headline metric, and LATENCY.md is normative for
// every number below; change a stamp and that document changes with it.
//
//   t0 ts_req_recvd   client request fully framed
//   t1 ts_req_built   upstream request built (nothing sent yet)
//   t2 ts_wire_ready  socket can carry the request (== t1 when pooled)
//   t3 ts_up_sent     request handed to the kernel by write()
//   t4 ts_up_recvd    provider's response received
//   t5                response built, client write begins
//   t6                response fully flushed
//
//   added = (t1-t0) + (t3-t2) + (t4 -> t6)
//
// Excluded on purpose: the provider's own time (t3->t4) and the TCP+TLS handshake
// (t1->t2, recorded separately as Stats::connect), because the client's own stack
// pays that same handshake without us, so it cancels.

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gateway/connection.hpp"
#include "gateway/metrics.hpp"
#include "gateway/policy.hpp"
#include "gateway/sink.hpp"
#include "net/http.hpp"
#include "net/tls.hpp"   // self-guarded by LLMBRIDGE_HAVE_TLS
#include "net/uring.hpp" // self-guarded by LLMBRIDGE_HAVE_URING
#include "provider/translate.hpp"

namespace llmbridge
{
    struct Stats
    {
        /// The t0-t6 stamps grouped three ways (LATENCY.md section 4). `connect` is
        /// kept out of req_path and overhead because a cold TCP+TLS handshake is
        /// 50 ms and would otherwise sit inside a metric sized for microseconds.
        Histogram overhead;  // req_path + resp_path: everything the gateway does
        Histogram req_path;  // framing/translate/auth plus the write() to the upstream
        /// TCP + TLS handshake only, on its own range: the default (20 ns over 2.62 ms)
        /// put every cold handshake in overflow, and widening the bucket instead would
        /// charge a pooled conn one bucket width of invented cost. 1 us over 262 ms.
        Histogram connect{1'000, 262'144};
        /// Inbound handshake, accept to client handshake done. Ours, unlike `connect`:
        /// it exists only because the gateway is in the path. Empty unless --listen-tls.
        Histogram accept_tls{1'000'000, 32'768};
        Histogram resp_path; // upstream-recv -> client-sent
        /// Time to first token, streamed requests only: t0 to the first content chunk.
        Histogram first_token{100'000, 262'144};
        uint64_t requests = 0;
        uint64_t errors = 0;
        uint64_t upstream_conns_opened = 0;
        uint64_t upstream_retries = 0;  // stale pooled connection -> resent on a fresh one
        uint64_t upstream_reused = 0;   // requests served on a pooled keep-alive conn
        uint64_t upstream_unsent = 0;   // response beat our request out; conn closed, not pooled
        /// Peak unsent ciphertext staged for one connection: the part of tls_out not
        /// yet written plus whatever is still in the write BIO. The measurement seam
        /// for "can a client that never reads grow us without bound?"; counting
        /// tls_out.size() instead grew with total bytes streamed, not the backlog.
        uint64_t tls_buffered_peak = 0;
        uint64_t upstream_timeouts = 0; // requests/streams aborted on upstream inactivity
        uint64_t client_idle_timeouts = 0;  // established clients dropped after going quiet
        uint64_t client_setup_timeouts = 0; // clients dropped for never completing a
                                            // first request (stall, or the wrong protocol)
        /// Inbound handshakes that failed, mostly scanners speaking junk at 443. Their
        /// log line sits at DEBUG so production stays readable; this keeps them
        /// visible, and a step change when a customer connects is their TLS problem.
        uint64_t client_tls_handshake_failures = 0;
        uint64_t stream_pauses = 0;     // epoll: upstream reads paused for client backpressure
        uint64_t uring_enobufs = 0;     // io_uring: provided-buffer pool momentarily empty
        /// Requests an installed Policy refused; a subset of `errors`, not a sibling.
        /// Denials climbing while `errors - denials` stays flat is a brute-force
        /// attempt, not an outage.
        uint64_t policy_denied = 0;
        /// Re-dispatched to another venue after a failure; 0 unless a policy opts in.
        /// `upstream_retries` is the same venue on a fresh connection.
        uint64_t upstream_failovers = 0;
    };

    class Gateway
    {
    public:
        /// How long a request may sit with no bytes from the upstream before the
        /// gateway gives up (0 = disabled), so a stalled provider cannot pin a client
        /// and two fds forever. Once streaming, it bounds the gap between events.
        static constexpr int64_t kDefaultUpstreamIdleNs = 120LL * 1000 * 1000 * 1000; // 120 s

        /// Keep-alive pool bound. Too small and it stops bounding and starts killing
        /// reuse: 256 cost the non-streaming path 2.4x its throughput, git-bisected.
        /// Do not lower it without re-running ./bench/saturate.sh with BACKENDS=4.
        static constexpr size_t kMaxIdleUpstreams = 8192;
        /// How long a pooled upstream may sit unused. A compromise for one upstream
        /// and one worker; every pool sees ~1/(workers x venues) of the traffic and
        /// every crossing of this line costs the next request a full reconnect,
        /// hence a knob.
        static constexpr int64_t kDefaultPoolIdleNs = 30LL * 1000 * 1000 * 1000; // 30 s

        /// How often the gateway says how many connections it is holding.
        static constexpr int64_t kDefaultHeartbeatNs = 300LL * 1000 * 1000 * 1000; // 5 min
        /// How long a client may stay connected without completing one request: a
        /// handshake that never finishes, a half-sent request, the wrong protocol.
        /// Generous, because a slow mobile client on a cold handshake is real.
        static constexpr int64_t kClientSetupNs = 30LL * 1000 * 1000 * 1000; // 30 s
        /// An established client that goes quiet. Deliberately enormous: a descriptor
        /// bound for an exposed listener, never a load-balancing device. 0 disables it.
        static constexpr int64_t kDefaultClientIdleNs = 3LL * 24 * 3600 * 1000 * 1000 * 1000;

        /// `policy` is non-owning and must outlive the Gateway; null means none is
        /// consulted.
        Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                int64_t warmup_ns = 0, UpstreamDialect dialect = UpstreamDialect::OpenAI,
                IoBackend io = IoBackend::Auto,
                int64_t upstream_idle_ns = kDefaultUpstreamIdleNs,
                TlsConfig tls = {}, bool timing_headers = false,
                Policy* policy = nullptr, std::vector<std::string> strip_headers = {});

        /// Several upstreams, chosen per request by the installed policy. Must not be
        /// empty, and is fixed for the process lifetime because the pools and every live
        /// `upstream_slot` index it. `tls` here is the inbound leg only.
        Gateway(uint16_t listen_port, std::vector<Upstream> upstreams, int64_t warmup_ns,
                IoBackend io, int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers,
                Policy* policy, std::vector<std::string> strip_headers);
        ~Gateway();

        Gateway(const Gateway&) = delete;
        Gateway& operator=(const Gateway&) = delete;

        /// Run the event loop until request_stop() is called. Returns 0.
        int run();

        /// Flips a flag the loop observes within one poll tick.
        void request_stop() noexcept { _stop.store(true, std::memory_order_relaxed); }

        /// Print this worker's latency histograms on its next sweep; otherwise the
        /// shutdown profile is the only way to read `accept(TLS)`.
        void request_stats_dump() noexcept { _dump.store(true, std::memory_order_relaxed); }

        /// Write this worker's histograms to `out`. Safe from the owning worker, or
        /// from any thread once the worker has stopped and been joined.
        void print_profile(std::FILE* out, const char* title) const noexcept;

        const Stats& stats() const noexcept { return _stats; }

        /// The configured backend (the requested mode; Auto stays Auto).
        IoBackend io_backend() const noexcept { return _io; }

        /// Actual bound listen port (resolves ephemeral when 0 was requested).
        uint16_t bound_port() const noexcept;

        /// Test seams and operational dials; call before run(). 0 disables pool
        /// reaping and silences the heartbeat respectively.
        void set_client_setup_ns(int64_t ns) noexcept { _client_setup_ns = ns; }
        void set_client_idle_ns(int64_t ns) noexcept { _client_idle_ns = ns; }
        void set_pool_idle_ns(int64_t ns) noexcept { _pool_idle_ns = ns; }
        void set_heartbeat_ns(int64_t ns) noexcept { _heartbeat_ns = ns; }
        /// Bytes to reserve and then write through in every request buffer before it
        /// carries a request: the two scratch strings at run(), each upstream's
        /// `wbuf` when its connection opens. A reserve alone maps nothing, the first
        /// write still faults every page, so the pages are touched here, off the
        /// request path. 0 (the default) prefaults nothing; the buffers still grow
        /// and keep their capacity, so only the first pass through each is cold.
        void set_prefault_bytes(size_t n) noexcept { _prefault_bytes = n; }
        /// Optional per-request metadata sink (sink.hpp). Non-owning; call before
        /// run(). `capture` names up to kSinkCaptureMax request headers whose values
        /// are copied (bounded) and handed back in RequestRecord::captured.
        void set_request_sink(RequestSink* sink, std::vector<std::string> capture);

        /// Test seam: does any pooled upstream still hold `needle`? Yes/no only, never
        /// returning or logging buffer contents, which is the only shape a
        /// credential-adjacent accessor may have. Exists because a mutation sweep
        /// showed secure_clear() could be deleted with no test noticing.
        [[nodiscard]] bool pooled_buffer_contains(std::string_view needle) const noexcept;
        /// Test seam. Like stats(), reads state owned by the loop thread, so it is
        /// valid only once that thread has been joined.
        [[nodiscard]] size_t pooled_upstream_count() const noexcept
        {
            size_t n = 0;
            for (const auto& pool : _idle_upstreams) n += pool.size();
            return n;
        }

        /// Test seam: the SNI/verify host recorded for an upstream, pinning the
        /// single-upstream constructor against a read-after-move that once emptied it.
        [[nodiscard]] std::string_view upstream_sni_host(size_t i) const noexcept
        {
            return i < _upstreams.size() ? std::string_view(_upstreams[i].sni_host)
                                         : std::string_view{};
        }

        /// Size of the io_uring provided-buffer pool (power of two), 0 for the default;
        /// call before run(). Lets a test shrink the pool far enough to force -ENOBUFS,
        /// a branch never reached at the shipped size even at 8192 streams.
        void set_uring_buf_count_for_test(unsigned n) noexcept { _uring_buf_count = n; }
        /// Test seam: prefault the scratch buffers now, without run(), and report how
        /// many of `_rebuild`'s pages the kernel has resident (mincore), so a test
        /// can prove the touch mapped them and a reserve alone would not have.
        size_t prefault_resident_bytes_for_test();

    private:
        // Naming: ep_* is epoll-only, ur_* is io_uring-only, unprefixed is shared. A
        // call crossing the prefixes is a bug, because neither backend's teardown,
        // write-arming or completion handling is valid in the other.
        // scripts/check_conventions.py enforces it; DESIGN.md "Naming conventions".
        void ep_add_read(Connection* c) noexcept;
        void ep_arm_write(Connection* c) noexcept;
        void ep_disarm_write(Connection* c) noexcept;

        void ep_on_accept() noexcept;
        void ep_on_client_readable(Connection* c) noexcept;
        void ep_on_client_writable(Connection* c) noexcept;
        void ep_on_upstream_writable(Connection* c) noexcept;
        void ep_on_upstream_readable(Connection* c) noexcept;

        /// Forward the client's buffered, optionally translated, request to an
        /// upstream; called inline once a full request is framed.
        void ep_forward(Connection* client) noexcept;
        /// Write the buffered response to the client; close out accounting.
        void ep_respond(Connection* client) noexcept;
        void ep_finish_client(Connection* c) noexcept;
        void send_interim_continue(Connection* c, bool uring) noexcept;

        // Streaming pump, epoll.
        void ep_pause_read(Connection* c) noexcept;   // drop EPOLLIN (backpressure)
        void ep_resume_read(Connection* c) noexcept;  // restore EPOLLIN
        void ep_begin_stream(Connection* u, const net::http::ResponseHead& h) noexcept; // enter streaming
        void ep_stream_pump(Connection* u) noexcept;      // decode+translate new upstream body bytes
        void ep_stream_on_upstream_eof(Connection* u) noexcept; // upstream closed: finish the stream
        void ep_stream_flush(Connection* client) noexcept;      // write buffered SSE, apply backpressure
        void ep_finalize_stream(Connection* client) noexcept;   // stream done: tear down + count

        /// Ask the installed policy about a freshly framed request. Callers treat
        /// `allow == false` as terminal and reply with their own ep_/ur_ responder.
        /// Only called when a policy exists.
        Decision policy_decision(Connection* c, const net::http::Message& m) noexcept;

        /// Sink plumbing, shared verbatim by both loops: sink_capture runs at framing,
        /// capture_model separately because the policy reads the model before
        /// deciding, sink_emit at every completion, streams and error replies included.
        void sink_capture(Connection* c) noexcept;
        void capture_model(Connection* c) noexcept;
        void sink_emit(Connection* c, int status, bool streamed) noexcept;

        /// Ask the policy for another venue and re-dispatch there. True when the
        /// request was re-sent, in which case the caller must not also answer the
        /// client. Twins because the teardown and forward they call are backend-specific.
        bool ep_upstream_failed(Connection* client, int status, const char* why) noexcept;
        bool ur_upstream_failed(Connection* client, int status, const char* why) noexcept;

        /// Shared half: may this be re-dispatched, and where? No teardown happens here.
        [[nodiscard]] Retry failover_target(Connection* client, int status,
                                            const char* why) noexcept;

        /// At most this many venues per request, so one dead provider is not a
        /// latency multiplier under a policy that always names another.
        static constexpr int kMaxFailoverAttempts = 3;

        /// Most bytes one readable event may pull off a socket, epoll's equivalent of
        /// io_uring's 4 KB provided buffer. ep_drain_read loops to EAGAIN, so a cap in
        /// the pump is checked once per event after an unbounded pull: one event once
        /// staged 33 MB for a client that never read. Only a read budget bounds that;
        /// level-triggered epoll re-notifies, so stopping early costs one wakeup.
        static constexpr size_t kEpMaxReadPerEvent = 1 << 20; // 1 MiB

        /// The venue a connection is bound to; see Connection::upstream_slot.
        [[nodiscard]] const Upstream& upstream_of(const Connection* c) const noexcept
        {
            const size_t i = (c->upstream_slot >= 0) ? static_cast<size_t>(c->upstream_slot) : 0;
            return _upstreams[i < _upstreams.size() ? i : 0];
        }

        /// One shared SSL_CTX covers every venue: it holds the trust store, while SNI
        /// and the verified hostname are per-connection.
        [[nodiscard]] bool any_upstream_tls() const noexcept
        {
            for (const Upstream& u : _upstreams)
                if (u.tls) return true;
            return false;
        }

        /// Abort any request whose upstream has been silent longer than
        /// _upstream_idle_ns, on the loop's existing periodic tick. `uring` selects
        /// the matching teardown primitives.
        void sweep_idle(bool uring) noexcept;

        /// `slot` indexes the upstream table; the pool it draws from is that venue's.
        Connection* ep_acquire_upstream(int slot) noexcept;
        void ep_release_upstream(Connection* u) noexcept;
        /// A pooled upstream failed before any response, so the provider dropped the
        /// idle keep-alive: resend the request once on a fresh connection.
        bool ep_retry_upstream(Connection* u) noexcept;
        void ep_close_client(Connection* c) noexcept;
        void ep_close_upstream(Connection* u) noexcept;
        void ep_abort_pair(Connection* client) noexcept;
        /// Reply with `code` and abandon any upstream. `why` is for the log, so an
        /// incident can ask why and not only whether. `detail` is what the client
        /// sees, and only a 4xx shows it: pass one only when the reason is the
        /// caller's own request; a policy's deny reason is free reconnaissance.
        void ep_error_respond(Connection* client, int code, const char* why,
                                 const char* detail = nullptr) noexcept;

        bool ep_drain_read(Connection* c) noexcept;
        bool ep_pump_write(Connection* c, bool* done) noexcept;

        /// Keep what the provider said about its own limits, from the response head.
        static void note_quota(Connection* client, const net::http::ResponseHead& h) noexcept;
        /// Copy the venue's own name for a failure out of its error body, for the
        /// sink. Does nothing on a 2xx.
        static void note_upstream_error(Connection* client,
                                        const net::http::ResponseHead& h,
                                        std::string_view body) noexcept;
        /// Warn once when a stream arrives compressed.
        static void stream_warn_if_encoded(const Connection* client,
                                           const net::http::ResponseHead& h) noexcept;
        /// A finished stream's contribution to the latency profile. Shared: a
        /// histogram fed by one backend only means something different per build.
        void stream_record_latency(const Connection* client) noexcept;
        /// End a stream without a terminal [DONE], because it is being aborted. One
        /// call so the three mutations it needs cannot be split up.
        void stream_truncate(Connection* client) noexcept;
        /// Outside the TLS guard because the t2 stamp sites call it in every build; a
        /// constant `false` without TLS.
        bool upstream_is_tls(const Connection* u) const noexcept;
        /// Has this upstream's request left the machine on every transport it uses?
        /// Recv is armed before send, so a provider answering early can complete a
        /// response while our request is half-written; pooling it then truncates the
        /// next client's request. ProxyEarlyResponse reproduces it.
        bool upstream_request_sent(const Connection* u) const noexcept;

#ifdef LLMBRIDGE_HAVE_TLS
        // TLS plumbing, both directions. The parameter is `c`, not `u`: these run on
        // client conns too. GATEWAY-INTERNALS.md 10c.
        /// Does configuration say this connection must carry TLS? Read from the leg.
        [[nodiscard]] bool tls_required(const Connection* c) const noexcept;
        /// Backstop before any write: false means tear the connection down.
        /// Unreachable today, kept because its precondition is re-established by
        /// hand at six call sites.
        [[nodiscard]] bool tls_invariant_ok(Connection* c) noexcept;
        /// Attach a Session in client role to an upstream conn. False on failure,
        /// treated like a connect refusal.
        bool tls_attach_upstream(Connection* u) noexcept;
        /// Attach a Session in server role to a freshly accepted client conn. False on
        /// failure, and the caller must then drop the connection: a peer that dialled
        /// TLS must never be answered in plaintext.
        bool tls_attach_client(Connection* c) noexcept;
        /// Move whatever ciphertext the Session has pending into c->tls_out.
        void tls_pump_out(Connection* c) noexcept;
        /// Feed ciphertext from the socket; plaintext lands in c->rbuf and the
        /// handshake advances. On an upstream conn completing it also stamps t2 and
        /// pushes the pending request. False on a fatal error.
        bool tls_feed(Connection* c, const char* p, size_t n) noexcept;
        /// Push un-fed plaintext (wbuf[woff..]) into the Session.
        void tls_push_wbuf(Connection* c) noexcept;
        /// True when wbuf is fully on the wire: all plaintext fed and all ciphertext
        /// flushed. The TLS analogue of ep_pump_write's `done`.
        bool tls_wbuf_flushed(const Connection* c) const noexcept;

        /// Epoll only: write tls_out to the socket, arming EPOLLOUT on a partial
        /// write. False = socket error.
        bool ep_tls_flush(Connection* u, bool* done) noexcept;
        /// Epoll only: TLS-aware replacement for ep_drain_read on upstream conns.
        bool ep_tls_drain_read(Connection* u) noexcept;

#ifdef LLMBRIDGE_HAVE_URING
        /// io_uring only: submit a send for tls_out if non-empty and none in flight;
        /// handshake flights and request bytes must not interleave on the wire.
        void ur_tls_flush(Connection* u) noexcept;
#endif
#endif

        int run_epoll();

#ifdef LLMBRIDGE_HAVE_URING
        /// The completion-driven mirror of the epoll state machine. A conn is freed
        /// only when its `inflight` SQEs all complete.
        int run_uring();
        bool ur_next_sqe(struct io_uring_sqe** out) noexcept; // get an SQE, flushing if full
        void ur_submit_accept() noexcept;
        void ur_submit_timer() noexcept;
        bool ur_arm_recv(Connection* c) noexcept; // arm a multishot recv (provided buffers)
        bool ur_submit_send(Connection* c) noexcept;
        /// Send wbuf to a client conn. Plaintext goes straight out; a TLS conn passes
        /// through the Session first, because the SQE points at tls_out.
        void ur_client_send(Connection* c) noexcept;
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
        // Streaming pump, io_uring: serialized sends (one send SQE in flight) and a
        // bounded buffer.
        void ur_begin_stream(Connection* u, const net::http::ResponseHead& h) noexcept;
        void ur_stream_pump(Connection* u) noexcept;
        void ur_stream_on_upstream_eof(Connection* u) noexcept;
        void ur_stream_flush(Connection* client) noexcept; // send pending bytes, or finalize
        void ur_finalize_stream(Connection* client) noexcept;
        Connection* ur_acquire_upstream(int slot) noexcept;
        void ur_release_upstream(Connection* u) noexcept;
        /// A pooled upstream failed before any response: resend the request once on
        /// a fresh connection. True if a retry was issued.
        bool ur_retry_upstream(Connection* u) noexcept;
        void ur_close(Connection* c) noexcept;
        void ur_abort_pair(Connection* client) noexcept;
        /// Same contract as ep_error_respond.
        void ur_error_respond(Connection* client, int code, const char* why,
                                 const char* detail = nullptr) noexcept;
        void ur_maybe_free(Connection* c) noexcept;

        net::uring::Ring _ring;
        net::uring::BufRing _bufring; // provided-buffer pool for multishot recv
        /// One resolved address per upstream, in table order. An SQE holds a pointer
        /// to the entry for the life of the connect, so this is sized once at startup
        /// and never resized.
        std::vector<sockaddr_in> _upstream_addrs;
        struct __kernel_timespec _uring_ts{};
        long _uring_inflight = 0; // global in-flight SQEs (drain barrier on stop)
        bool _draining = false;   // post-stop: completions just decrement, no re-arm
#endif

        uint16_t _listen_port;
        /// Never empty: the single-upstream constructor builds one entry, so every
        /// path can index it without a special case.
        std::vector<Upstream> _upstreams;
        int64_t _warmup_ns;
        /// Settable only so tests can pick a deadline they can wait for: at 30 seconds
        /// a mutation sweep showed deleting the deadline entirely broke no test.
        int64_t _client_setup_ns = kClientSetupNs;
        int64_t _client_idle_ns = kDefaultClientIdleNs;
        int64_t _pool_idle_ns = kDefaultPoolIdleNs;
        size_t _prefault_bytes = 0;
        /// Reserve `_prefault_bytes` in `s` and touch every page. Shared by both backends.
        void prefault(std::string& s) const;
        IoBackend _io;
        int64_t _upstream_idle_ns;         // 0 = no idle timeout
        TlsConfig _tls; // both legs; each flag inits its own context in the ctor
        bool _timing_headers = false;      // emit x-llmbridge-* timing on responses
        /// `const` so there is no setter: swapping it while the loop runs would be a
        /// data race on the request path. See policy.hpp.
        Policy* const _policy = nullptr;
        RequestSink* _sink = nullptr;
        std::vector<std::string> _sink_capture_names; ///< lowercased, no colon
        uint8_t _active_backend = 0;                  ///< 1 epoll, 2 io_uring; set by run
        /// Header names dropped from every upstream request, lower-cased with the
        /// colon so they compare against a raw line. Empty in a stock build.
        std::vector<std::string> _strip_headers;
#ifdef LLMBRIDGE_HAVE_TLS
        /// One SSL_CTX per direction, configured oppositely: the upstream context
        /// verifies a peer, the client context presents a certificate. Named for the
        /// leg each serves, because on the client leg our TLS role is server.
        net::tls::Context _tls_upstream_ctx;
        net::tls::Context _tls_client_ctx;
#endif
        unsigned _uring_buf_count = 0;     // 0 = kUrBufCount default (test hook only)
        int64_t _last_sweep_ns = 0;
        int64_t _heartbeat_ns = kDefaultHeartbeatNs;
        /// 0 until the first sweep emits, which it does immediately, so an operator
        /// learns the heartbeat works without waiting an interval for it.
        int64_t _last_heartbeat_ns = 0;
        bool _uring_active = false;

        int _epfd = -1;
        int _listen_fd = -1;
        Connection* _listen_conn = nullptr;

        std::unordered_map<uint64_t, Connection*> _clients;
        std::vector<std::vector<Connection*>> _idle_upstreams; ///< one pool per upstream
        uint64_t _next_client_id = 1;
        std::vector<Connection*> _doomed; // closed mid-batch, freed after the batch
        /// The byte-forward rebuild's destination, swapped with the upstream's `wbuf`
        /// once the upstream is acquired, so the buffers rotate and keep their
        /// capacity instead of being allocated per request. See request_without.
        std::string _rebuild;
        /// The translated or override-rewritten body on its way into `_rebuild`.
        std::string _xlate;

        int64_t _t_start = 0;
        Stats _stats;
        /// Set cross-thread by the signal/timer thread, observed by the worker loop.
        /// Relaxed is enough: they gate loop continuation, no other state depends on them.
        std::atomic<bool> _stop{false};
        std::atomic<bool> _dump{false};
    };
} // namespace llmbridge
