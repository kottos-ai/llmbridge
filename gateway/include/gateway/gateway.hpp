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
//
// How it works inside, and the questions a reader actually has (which buffer holds
// what, who may free a connection on each backend, how pooling and the two TLS legs
// fit together): GATEWAY-INTERNALS.md.
//
// Per-request added latency, the headline metric. LATENCY.md is NORMATIVE for every
// number below; change a stamp and that document changes with it.
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
// pays that same handshake without us, so it cancels. Folding connect in once made a
// live single-request run report 52.66 ms of "request path" that was 99.9% handshake.

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gateway/metrics.hpp"
#include "gateway/policy.hpp"
#include "net/http.hpp"
#include "net/log.hpp"
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
        // Outbound: gateway -> provider. We are the TLS client and we verify them.
        //
        // Named for the LEG, not for a bare "enabled": with two legs, "enabled"
        // could not say which one it meant. The pair matches the vocabulary the
        // rest of the gateway uses for the two sides, client and upstream.
        bool upstream_tls = false;
        std::string sni_host; // DNS name for SNI + certificate hostname verification
        std::string ca_file;  // empty = system trust store (tests pass their own CA)

        // Inbound: client -> gateway. We are the TLS server and we prove identity.
        //
        // ONE LISTENER, ONE MODE (decided 2026-08-10): setting this makes the
        // single listener TLS-only. There is no second plaintext port, so the
        // answer to "am I exposed in the clear?" can be read off the command line
        // instead of inferred from which port a client happened to use.
        bool client_tls = false;  // set by --listen-tls
        //
        // What stops a TLS-required connection ever receiving plaintext is that
        // BOTH accept paths close it outright when tls_attach_client() fails, and
        // both upstream paths do the same for tls_attach_upstream(). A separate
        // runtime guard on the write path, tls_invariant_ok(), backs that up. It
        // cannot fire today and no test distinguishes it from `return true`; it is
        // kept because six call sites must each stay right for it to stay
        // unreachable, and two of those six are recent.
        std::string cert_file; // PEM chain, leaf first (Let's Encrypt fullchain.pem)
        std::string key_file;  // PEM key, mode 600 or startup refuses it
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
        // Log identity, distinct from `id` below: `id` is the client-map KEY and is 0
        // on every upstream, so it cannot name an upstream in a log line. This one is
        // process-unique for every Connection of either kind and never changes, which
        // is what makes a connection's whole life greppable as "Connection#42".
        uint64_t log_inst = net::log::next_instance();
        // CLIENT conns: the sequencer value for the request currently in flight.
        // Assigned once at framing and consumed by both the log lines and the
        // x-llmbridge-seq header, so the two surfaces name the same request.
        uint64_t req_seq = 0;
        // Provider-reported token counts for the request in flight, -1 when the
        // provider did not report them. Non-streaming: scanned out of the translated
        // body. Streaming: read off the SSE translator at finalize. Metadata, never
        // content, which is the same rule the recorder and the policy seam follow.
        long long tok_in = -1;
        long long tok_out = -1;
        bool write_armed = false;     // epoll backend only: EPOLLOUT currently registered
        bool connected = false;       // upstream-only: non-blocking connect done
        bool request_pending = false; // client-only: full request buffered, awaiting forward
        // Closed, not yet freed. THE DEFERRAL RULE DIFFERS BY BACKEND: epoll frees at
        // the end of the event batch, io_uring only once `inflight` hits 0, because a
        // submitted SQE still references this object. So on io_uring `doomed` and
        // `inflight` are ONE protocol: doomed says we want it gone, inflight says
        // whether the kernel agrees. Freeing on the first alone is a use-after-free in
        // a process holding customer credentials.
        // Full rules, including why the two unconditional frees are safe without a
        // guard: GATEWAY-INTERNALS.md section 7.
        bool doomed = false;
        bool close_after_resp = false; // client-only: this is an error reply, so close once it flushes

        uint64_t id = 0; // client conns: stable id; upstream conns: 0

        // Non-streaming chunked decode state, PER CONNECTION instead of shared:
        // it must persist across reads so the decoder is fed only new bytes
        // (see net::http::ResponseDecoder; the shared-scratch form was quadratic).
        net::http::ResponseDecoder rdec;

        // THE BUFFERS, named from the GATEWAY's point of view on THIS socket: rbuf is
        // what we read from the peer, wbuf what we write to it. So the same field name
        // holds the REQUEST on a client conn and the RESPONSE on an upstream one, which
        // is the most confusing thing about this struct.
        //   c->rbuf request in   u->wbuf request out
        //   u->rbuf response in  c->wbuf response out
        // Always PLAINTEXT on both legs; TLS interposes at the socket edge only. The
        // other three buffers (tls_out, wpending, rdec) are declared below. Diagram and
        // full ownership rules: GATEWAY-INTERNALS.md section 2.
        std::string rbuf;
        std::string wbuf;

        // How much of wbuf has been dealt with. MEANING SHIFTS WITH THE TRANSPORT:
        // plaintext counts bytes on the socket, TLS counts bytes fed into the Session,
        // which is not the same thing (wire progress is tls_out_off). The write path
        // deliberately never clears wbuf, which is what keeps a request resendable on a
        // dead pooled connection. GATEWAY-INTERNALS.md section 2b.
        size_t woff = 0;

        // Client conns: when we accepted it, and whether it has ever produced a
        // complete request. Together they bound the SETUP phase: a peer that
        // connects and then stalls, deliberately or through a protocol mismatch,
        // must not hold a slot forever. Once a request has framed the peer has
        // proved it speaks the protocol, and ordinary keep-alive rules apply.
        int64_t ts_accepted = 0;
        // CLIENT conns: when this connection last completed a request. Distinct from
        // ts_accepted, which never moves: the setup deadline reaps a client that never
        // framed anything, and this reaps one that framed something long ago and then
        // sat on the descriptor. Not part of the t0-t6 scheme; it is per CONNECTION.
        int64_t ts_client_activity = 0;
        bool ever_framed = false;

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

        // io_uring only: submitted-but-uncompleted SQEs referencing this conn. One
        // protocol with `doomed` above. Ownership rules (three writers, one reader,
        // and why each sits where it does): GATEWAY-INTERNALS.md section 5b.
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
        // A ONE-WAY LATCH, never cleared. A streamed client response is
        // close-delimited, so the connection ends with the stream and there is no
        // state to return to. Set in ep_begin_stream / ur_begin_stream only.
        bool streaming = false;
        bool stream_chunked = false; // upstream body uses chunked transfer-encoding

        // No further stream output will be produced. TWO WAYS, and the pair with
        // close_after_resp tells them apart: set means truncated (no [DONE], so the
        // client can see it was cut off), clear means clean. GATEWAY-INTERNALS.md 6b.
        bool stream_ended = false;

        // Epoll only, and THE one place the backends deliberately differ: epoll applies
        // back-pressure to a slow client, io_uring drops the stream past kStreamBufCap.
        // Written only by ep_pause_read / ep_resume_read. GATEWAY-INTERNALS.md 6c.
        bool read_paused = false;
        bool wants_usage = false;    // client set stream_options.include_usage
        std::unique_ptr<provider::AnthropicToOpenAiSse> sse; // Anthropic->OpenAI SSE translator
        net::http::ChunkDecoder chunkdec;                          // decodes the upstream chunked body
        // io_uring streaming only: translated output accumulates in `wpending`
        // while a client SEND SQE is in flight, so `wbuf` (the SEND's buffer) is
        // never reallocated under the kernel's feet.
        std::string wpending;

        // io_uring: an SQE referencing this connection's SEND buffer is outstanding,
        // so that buffer must not move (an SQE is immutable once submitted). CALLERS
        // MUST NEVER SET IT; two once did, and one of them hung every TLS stream.
        // Ownership: GATEWAY-INTERNALS.md section 5b.
        bool send_inflight = false;
        // Upstream said keep-alive on the streaming response, so the connection may
        // be pooled once the body's terminal chunk has been consumed. Held on the
        // CLIENT conn alongside the rest of the per-request streaming state.
        bool stream_keep_alive = false;

        // Upstream conns only: when this connection entered the idle pool. Idle
        // keep-alives are reaped after _pool_idle_ns (--pool-idle). Providers drop them on
        // their own schedule, and a pooled corpse costs a retry to discover.
        int64_t ts_pooled = 0;

#ifdef LLMBRIDGE_HAVE_TLS
        // ── TLS state (upstream conns only; null = plaintext) ────────────────
        // The Session outlives requests: it stays attached across keep-alive pool
        // cycles, so a pooled reuse pays no second handshake.
        std::unique_ptr<net::tls::Session> tls;
        // Ciphertext awaiting the socket; wbuf keeps the plaintext for stale-conn
        // retry. TWO OFFSETS, TWO QUESTIONS: woff is plaintext into the Session,
        // tls_out_off is ciphertext onto the wire, so `woff >= wbuf.size()` means
        // "fully encrypted", never "fully sent". GATEWAY-INTERNALS.md 10b.
        std::string tls_out;
        size_t tls_out_off = 0;
#endif
    };

    /// Connection's "print method". A free function, found by ADL, so logging a
    /// connection costs one call and no iostream: `LB_INFO("closed ", *c)` renders
    /// `Connection#42(fd=17,client)`. Deliberately prints NOTHING from rbuf or wbuf:
    /// those hold the customer's request, including its credential.
    inline void log_put(net::log::Line& l, const Connection& c)
    {
        // The class name leads, so one grep separates the two halves of the proxy:
        // ClientConnection#3(fd=6,cid=2) against UpstreamConnection#2(fd=7).
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

    /// The third subject. A request is not an object with a lifetime, so it is named
    /// by the sequencer: `Request#123`. One number, assigned once when the request
    /// frames, reused by the timing header, so a log line and a response header
    /// cannot disagree about which request they mean.
    struct ReqId
    {
        uint64_t seq;
    };
    inline void log_put(net::log::Line& l, ReqId r)
    {
        l.put(net::log::Id{"Request", r.seq});
    }

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
        // TCP + TLS handshake only, and NOT the default range. The default (20 ns over
        // 2.62 ms) is sized for the sub-ms overhead claim, but a cold handshake is
        // 50-80 ms and landed every sample in overflow, where percentile() returns the
        // running max and p50/p99/max become one number wearing three labels. Widening
        // the bucket instead would be worse: percentile() reports a bucket's UPPER
        // edge, so a pooled conn that paid no handshake would read as one bucket width
        // of invented cost. 1 us over 262 ms keeps that below the noise. 2 MB/worker.
        Histogram connect{1'000, 262'144};
        // INBOUND handshake: accept -> client handshake done. Unlike `connect`,
        // this one exists ONLY because the gateway is in the path, so the
        // reasoning that excludes the upstream handshake does not apply. See
        // LATENCY.md section 1. Empty unless --listen-tls.
        //
        // Same range as `connect`, and for the same reasons; see the note there.
        Histogram accept_tls{1'000, 262'144};
        Histogram resp_path; // upstream-recv -> client-sent
        uint64_t requests = 0;
        uint64_t errors = 0;
        uint64_t upstream_conns_opened = 0;
        uint64_t upstream_retries = 0;  // stale pooled connection -> resent on a fresh one
        uint64_t upstream_reused = 0;   // requests served on a pooled keep-alive conn
        uint64_t upstream_unsent = 0;   // response beat our request out; conn closed, not pooled
        // Peak ciphertext staged for ONE connection: tls_out plus whatever is still
        // in the write BIO. Measurement seam for the question "can a client that
        // never reads grow us without bound?", which is answerable only by a number.
        uint64_t tls_buffered_peak = 0;
        uint64_t upstream_timeouts = 0; // requests/streams aborted on upstream inactivity
        uint64_t client_idle_timeouts = 0;  // established clients dropped after going quiet
        uint64_t client_setup_timeouts = 0; // clients dropped for never completing a
                                            // first request (stall, or a client
                                            // speaking the wrong protocol at us)
        uint64_t stream_pauses = 0;     // epoll: upstream reads paused for client backpressure
        uint64_t uring_enobufs = 0;     // io_uring: provided-buffer pool momentarily empty
        // Requests an installed Policy refused; 0 in a stock build. A SUBSET of
        // `errors`, not a sibling: the refusal goes out through the same responder, so
        // both counters move. Denials climbing while `errors - denials` stays flat is
        // a brute-force attempt, not an outage.
        uint64_t policy_denied = 0;
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

        // Keep-alive pool bound: without one, a streaming gateway pins roughly one idle
        // fd per concurrent stream forever. SIZE IT GENEROUSLY. Too small and it stops
        // bounding and starts killing reuse; 256 cost the non-streaming path 2.4x its
        // throughput, git-bisected. Do not lower it without re-running
        // ./bench/saturate.sh with BACKENDS=4. Numbers: GATEWAY-INTERNALS.md 8b.
        static constexpr size_t kMaxIdleUpstreams = 8192;
        // How long a POOLED upstream may sit unused before it is closed. 30 s is a
        // compromise for one upstream and one worker; it is the wrong number as soon
        // as either multiplies. Traffic divides across N workers (each has its own
        // pool) and, once routing lands, across M upstreams, so each pool sees ~1/(N*M)
        // of the requests and crosses this line far more often. Every crossing costs
        // the next request a full reconnect. Hence a knob, not a constant.
        static constexpr int64_t kDefaultPoolIdleNs = 30LL * 1000 * 1000 * 1000; // 30 s
        // How long a client may stay connected without completing one request.
        // Covers a TLS handshake that never finishes, a half-sent request, and a
        // client speaking the wrong protocol (TLS at a plaintext listener frames
        // as garbage that never completes). Generous on purpose: a slow mobile
        // client on a cold TLS handshake is a real thing, and this only has to
        // bound the hold, never be tight.
        static constexpr int64_t kClientSetupNs = 30LL * 1000 * 1000 * 1000; // 30 s
        // An ESTABLISHED client that goes quiet. Deliberately enormous: this is a
        // descriptor bound for an exposed listener, never a load-balancing device.
        // Anything short would charge a reconnecting client a fresh TCP+TLS handshake
        // to solve a problem the client cannot see. 0 disables it.
        static constexpr int64_t kDefaultClientIdleNs = 3LL * 24 * 3600 * 1000 * 1000 * 1000;

        // `policy` is non-owning and must outlive the Gateway; null (the default)
        // means none is consulted. Last parameter so no existing call changes meaning.
        Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                int64_t warmup_ns = 0, TranslateMode translate = TranslateMode::None,
                IoBackend io = IoBackend::Auto,
                int64_t upstream_idle_ns = kDefaultUpstreamIdleNs,
                TlsConfig tls = {}, bool timing_headers = false,
                Policy* policy = nullptr, std::vector<std::string> strip_headers = {});
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

        // Test seam. Production never calls this; the default is kClientSetupNs.
        void set_client_setup_ns(int64_t ns) noexcept { _client_setup_ns = ns; }
        /// Test seam, same contract as set_client_setup_ns: call BEFORE run().
        void set_client_idle_ns(int64_t ns) noexcept { _client_idle_ns = ns; }
        /// Test seam, same contract: call BEFORE run(). 0 disables pool reaping.
        void set_pool_idle_ns(int64_t ns) noexcept { _pool_idle_ns = ns; }

        // Test seam: does any POOLED upstream still hold `needle`? A pooled conn is
        // handed to whichever client asks next, so a credential left in its buffer
        // outlives the request that supplied it. Yes/no only, NEVER returning or
        // logging buffer contents, which is the only shape a credential-adjacent
        // accessor may have. Exists because a mutation sweep showed secure_clear()
        // could be deleted with no test noticing.
        [[nodiscard]] bool pooled_buffer_contains(std::string_view needle) const noexcept;
        // Test seam. Like stats(), this reads state owned by the loop thread, so it
        // is valid only once that thread has been joined. Reading it live is a data
        // race, which TSan reports; production obeys this, see app/main.cpp.
        [[nodiscard]] size_t pooled_upstream_count() const noexcept
        {
            return _idle_upstreams.size();
        }

        // Size of the io_uring provided-buffer pool (power of two), or 0 for the default.
        // Must be called before run(). Exists so a test can shrink the pool far enough to
        // force -ENOBUFS deterministically. At the shipped size that branch was never
        // reached even at 8192 concurrent streams, so without this hook the recovery path
        // would be untestable and could rot silently. Not a tuning knob users need.
        void set_uring_buf_count_for_test(unsigned n) noexcept { _uring_buf_count = n; }

    private:
        // NAMING: ep_* is epoll-only, ur_* is io_uring-only, unprefixed is shared. A
        // call crossing the prefixes is a bug, because neither backend's teardown,
        // write-arming or completion handling is valid in the other. Exactly one such
        // crossing existed when the convention landed, invisible while the epoll half
        // was unprefixed. scripts/check_conventions.py enforces it; the rationale is in
        // DESIGN.md "Naming conventions".
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

        // Ask the installed policy about a freshly framed request. Unprefixed: the
        // question has nothing to do with the event loop. Callers must treat
        // `allow == false` as terminal and reply with their own ep_/ur_ responder.
        // Only called when a policy exists; the caller checks first.
        Decision policy_decision(const Connection* c, const net::http::Message& m) noexcept;

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
        /// Reply with `code` and abandon any upstream. `why` is a SHORT literal naming
        /// the cause, logged with the request id: 28 call sites collapse into three
        /// status codes, so without it a log can say a request failed but never why,
        /// which is the question an incident actually asks.
        void ep_error_respond(Connection* client, int code, const char* why) noexcept;

        bool ep_drain_read(Connection* c) noexcept;
        bool ep_pump_write(Connection* c, bool* done) noexcept;

        // Is this upstream carrying TLS? Shared by both backends (hence no
        // ep_/ur_ prefix) and declared outside the TLS guard because the t2
        // stamp sites call it in every build; it is a constant `false` when the
        // project is built without TLS.
        // End a stream WITHOUT a terminal [DONE], because it is being aborted.
        // One call so the three mutations it needs cannot be split up; five sites
        // across both backends used to write them out by hand.
        void stream_truncate(Connection* client) noexcept;
        bool upstream_is_tls(const Connection* u) const noexcept;
        // Has this upstream's request left the machine on every transport it uses? A
        // pooled upstream MUST satisfy this. Recv is armed before send, so a provider
        // answering early can complete a response while our request is half-written;
        // pooling it then truncates the next client's request, and under io_uring
        // rewrites a buffer a live SQE points at. ProxyEarlyResponse reproduces it.
        bool upstream_request_sent(const Connection* u) const noexcept;

#ifdef LLMBRIDGE_HAVE_TLS
        // ── TLS plumbing, BOTH directions ───────────────────────────────────
        // No-op-safe helpers shared by both backends. The parameter is `c`, not `u`:
        // these run on client conns too. GATEWAY-INTERNALS.md 10c.
        //
        // Attach a Session in CLIENT role to an upstream conn (SNI and hostname
        // from _tls.sni_host). False on failure, treated like a connect refusal.
        // Does configuration say this connection must carry TLS? Read from the leg,
        // so the two directions cannot be confused.
        [[nodiscard]] bool tls_required(const Connection* c) const noexcept;
        // Backstop before any write: false means tear the connection down instead.
        // Unreachable today, kept because its precondition is re-established by
        // hand at six call sites. See the definition.
        [[nodiscard]] bool tls_invariant_ok(Connection* c) noexcept;
        bool tls_attach_upstream(Connection* u) noexcept;
        // Attach a Session in SERVER role to a freshly accepted CLIENT conn. Note
        // the roles are crossed on purpose: our TLS role is server precisely
        // because the peer is the client. False on failure, and the caller must
        // then drop the connection, because a peer that dialled TLS must never be
        // answered in plaintext.
        bool tls_attach_client(Connection* c) noexcept;
        // Move whatever ciphertext the Session has pending into c->tls_out.
        void tls_pump_out(Connection* c) noexcept;
        // Feed ciphertext from the socket. Drains resulting plaintext into c->rbuf
        // and advances the handshake. On an UPSTREAM conn, completing the handshake
        // also stamps t2 and pushes the pending request; on a client conn there is
        // nothing pending, because the client speaks first. False on a fatal error.
        bool tls_feed(Connection* c, const char* p, size_t n) noexcept;
        // Push un-fed plaintext (wbuf[woff..]) into the Session. On an upstream
        // conn that is the request; on a client conn it is the response.
        void tls_push_wbuf(Connection* c) noexcept;
        // True when wbuf is fully on the wire: all plaintext fed AND all ciphertext
        // flushed. The TLS analogue of ep_pump_write's `done`.
        bool tls_wbuf_flushed(const Connection* c) const noexcept;

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
        // Send wbuf to a CLIENT conn. Plaintext goes straight out; a TLS conn must
        // pass through the Session first, because the SQE points at tls_out.
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
        void ur_error_respond(Connection* client, int code, const char* why) noexcept;
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
        // Defaults to kClientSetupNs. Settable ONLY so tests can pick a deadline
        // they can wait for: at 30 seconds the behaviour was unverifiable, and a
        // mutation sweep confirmed that deleting the deadline entirely broke no
        // test. An untested deadline on an internet-facing listener is the same as
        // no deadline, because nothing would tell us when it stopped working.
        int64_t _client_setup_ns = kClientSetupNs;
        int64_t _client_idle_ns = kDefaultClientIdleNs;
        int64_t _pool_idle_ns = kDefaultPoolIdleNs;
        TranslateMode _translate;
        IoBackend _io;
        int64_t _upstream_idle_ns;         // 0 = no idle timeout
        TlsConfig _tls; // BOTH legs; each flag inits its own context in the ctor
        bool _timing_headers = false;      // emit x-llmbridge-* timing on responses
        // `const` so there is no setter: swapping this while the loop runs would be
        // a data race on the request path. See policy.hpp.
        Policy* const _policy = nullptr;
        // Header names dropped from every upstream request, lower-cased with the
        // colon so they can be compared against a raw line. Empty in a stock build,
        // where the passthrough copy stays a single memcpy.
        std::vector<std::string> _strip_headers;
        std::string _upstream_host_hdr;    // Host: value for rebuilt upstream requests
        // Decode buffer for CHUNKED upstream responses (see net::http::parse_response).
        // One per loop, not per connection: the loop is single-threaded and a
        // response is framed and consumed entirely within one event, so there is no
        // overlap. Reused across requests so the chunked path, which is the REAL
        // provider path, not an edge case. Does not allocate per response.
#ifdef LLMBRIDGE_HAVE_TLS
        // One SSL_CTX per direction, each shared by every Session in that
        // direction. They are separate objects because they are configured
        // oppositely: the client context verifies a peer, the server context
        // presents a certificate and verifies nobody.
        // Named for the LEG each one serves, matching TlsConfig::upstream_tls /
        // client_tls and the tls_attach_* pair. Naming one of them for its TLS
        // ROLE instead would collide: on the client leg our role is server.
        net::tls::Context _tls_upstream_ctx; // upstream leg; our role is TLS client
        net::tls::Context _tls_client_ctx;   // client leg;   our role is TLS server
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
