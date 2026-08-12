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
        bool write_armed = false;     // epoll backend only: EPOLLOUT currently registered
        bool connected = false;       // upstream-only: non-blocking connect done
        bool request_pending = false; // client-only: full request buffered, awaiting forward
        // Closed, but not yet freed. The DEFERRAL RULE DIFFERS BY BACKEND, and the
        // old one-line comment described only the epoll half:
        //
        //   epoll     freed at the end of the current event batch. The deferral
        //             exists because one event can close a pair, and a later event
        //             in the SAME batch would then dereference a freed pointer.
        //             `inflight` is never incremented here, so the free is
        //             unconditional.
        //   io_uring  freed when `inflight` reaches 0, which may be several loop
        //             iterations later. A submitted SQE references this object, and
        //             the kernel does not care that we decided to close.
        //
        // `doomed` and `inflight` are therefore ONE protocol on io_uring, not two
        // independent fields: doomed says we want it gone, inflight says whether the
        // kernel agrees yet. Freeing on the first without checking the second is a
        // use-after-free in a process holding customer credentials.
        //
        // The two UNCONDITIONAL frees, in the epoll batch loop and the destructor,
        // are safe for reasons worth writing down, because a guard was added there
        // and then removed once these were established:
        //   epoll     never increments `inflight`, so it is always 0
        //   io_uring  run_uring() ends with `while (_uring_inflight > 0)`, draining
        //             to zero before it returns. _uring_inflight moves in lockstep
        //             with every conn's `inflight` (both bumped at the three submit
        //             sites, both dropped at the single completion site), so a zero
        //             total means every per-conn count is zero too.
        // Both are structural guarantees enforced in one place each, which is why a
        // runtime check there was unreachable and untestable. Contrast
        // tls_invariant_ok(), whose precondition is re-established by hand at six
        // call sites and which is therefore kept.
        bool doomed = false;
        bool close_after_resp = false; // client-only: this is an error reply, so close once it flushes

        uint64_t id = 0; // client conns: stable id; upstream conns: 0

        // Non-streaming chunked decode state, PER CONNECTION instead of shared:
        // it must persist across reads so the decoder is fed only new bytes
        // (see net::http::ResponseDecoder; the shared-scratch form was quadratic).
        net::http::ResponseDecoder rdec;

        // THE BUFFERS. `r` and `w` are named from the GATEWAY's point of view on
        // THIS socket: rbuf is what we read from the peer, wbuf is what we write
        // to it. So the same field name holds the REQUEST on one connection and
        // the RESPONSE on the other, and which is which depends on `is_client`.
        // That is the single most confusing thing about this struct, so here is
        // one request crossing all four buffers:
        //
        //     client                      gateway                      provider
        //       |                                                         |
        //       |  request                                                |
        //       |------------> c->rbuf --[frame, translate]--> u->wbuf    |
        //       |                                                 |-------|-->
        //       |                                                         |
        //       |                                                response |
        //       |  c->wbuf <--[translate back]-- u->rbuf <-----------------|
        //       |<------------|                                           |
        //
        //   c->rbuf   bytes the CLIENT sent us      (the request)
        //   u->wbuf   bytes we send the PROVIDER    (the request, translated)
        //   u->rbuf   bytes the PROVIDER sent us    (the response)
        //   c->wbuf   bytes we send the CLIENT      (the response, translated)
        //
        // Both buffers are PLAINTEXT on both legs, always, whether or not TLS is
        // in use. TLS interposes at the socket edge only.
        //
        // There are five buffers on a Connection in total. The other three are
        // declared further down with their own notes, listed here so the full set
        // is visible in one place:
        //   tls_out    ciphertext heading for the socket (TLS conns only)
        //   wpending   io_uring streaming staging area, because wbuf must not move
        //              while a SEND SQE points into it
        //   rdec       chunked-decode state, not a byte buffer
        std::string rbuf;
        std::string wbuf;

        // How much of wbuf has been dealt with. Its meaning shifts with the
        // transport, which is a real trap:
        //   plaintext:  bytes actually written to the socket
        //   TLS:        bytes fed into the Session, which is NOT the same as bytes
        //               on the wire. The wire progress is tls_out_off. woff can
        //               reach wbuf.size() while nothing has left the machine yet.
        // The WRITE PATH deliberately does not clear wbuf when woff catches up;
        // callers do, at points they choose. That is what keeps an upstream
        // request available for a resend when a pooled connection turns out to be
        // dead (see ep_retry_upstream / ur_retry_upstream, which resend only when
        // the connection came from the pool and no response byte has arrived).
        size_t woff = 0;

        // Client conns: when we accepted it, and whether it has ever produced a
        // complete request. Together they bound the SETUP phase: a peer that
        // connects and then stalls, deliberately or through a protocol mismatch,
        // must not hold a slot forever. Once a request has framed the peer has
        // proved it speaks the protocol, and ordinary keep-alive rules apply.
        int64_t ts_accepted = 0;
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

        // io_uring backend only: submitted-but-uncompleted SQEs referencing this
        // conn. See `doomed` above: the two are one protocol.
        //
        // OWNERSHIP:
        //   INCREMENTED by the three ur_submit_* functions, each at its tail,
        //              immediately before `return true`, so an early return cannot
        //              leak a slot and strand the object forever
        //   DECREMENTED in exactly one place, ur_on_cqe(), and only when the
        //              completion is NOT armed: a multishot op carrying F_MORE is
        //              still outstanding and has not released its slot
        //   READ       by ur_maybe_free(), which is the only thing allowed to
        //              conclude that freeing is safe
        //
        // (Multishot recv lands data in a shared provided-buffer pool, so there is
        // no per-connection recv buffer to worry about.)
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

        // No further stream output will be produced. It becomes true TWO ways, and
        // the old comment here ("final [DONE] emitted") described only the first:
        //
        //   clean      stream_step() saw the end of the body and the SSE translator
        //              emitted its terminal [DONE]. close_after_resp stays false.
        //   truncated  the stream is being aborted, so NO [DONE] is emitted, on
        //              purpose: a client must see a cut-off stream instead of a
        //              fabricated clean finish. Always via stream_truncate().
        //
        // So `stream_ended && close_after_resp` is the truncated case and
        // `stream_ended && !close_after_resp` the clean one. That pairing is the
        // difference between a client believing it got the whole answer and knowing
        // it did not, which makes it worth stating here and never inferring.
        bool stream_ended = false;

        // Epoll only, and it is the ONE place the two backends deliberately behave
        // differently: under a slow client epoll pauses upstream EPOLLIN and applies
        // back-pressure, while the io_uring pump instead bounds wpending with
        // kStreamBufCap and drops the stream past the cap. Know which one you are
        // reasoning about before quoting streaming behaviour to a customer.
        //
        // Owned entirely by ep_pause_read / ep_resume_read, which are the only
        // writers and both guard on the current value. io_uring never touches it,
        // and the ep_ prefix is what keeps that true.
        bool read_paused = false;
        bool wants_usage = false;    // client set stream_options.include_usage
        std::unique_ptr<provider::AnthropicToOpenAiSse> sse; // Anthropic->OpenAI SSE translator
        net::http::ChunkDecoder chunkdec;                          // decodes the upstream chunked body
        // io_uring streaming only: translated output accumulates in `wpending`
        // while a client SEND SQE is in flight, so `wbuf` (the SEND's buffer) is
        // never reallocated under the kernel's feet.
        std::string wpending;

        // io_uring: an SQE referencing this connection's send buffer is
        // outstanding. Two concurrent SENDs on one fd would interleave, and an
        // SQE is immutable once submitted, so the buffer it points at must not
        // move while this is set.
        //
        // OWNERSHIP, and it is worth stating because getting it wrong cost a
        // silently hung stream:
        //   SET    only by ur_submit_send(), the only place an SQE is submitted
        //   CLEARED only by ur_on_send() on completion, and by
        //           ur_release_upstream() when per-request state is reset
        //   READ   by anyone about to touch a send buffer, to decide whether to
        //          wait
        //
        // Callers must never set it. Two of them used to, under two different
        // rules, and ur_stream_flush() setting it before calling into
        // ur_tls_flush() (whose first line refuses to run when it is already set)
        // meant the first SSE flush on a TLS connection did nothing at all. The
        // stream then hung forever, on io_uring only.
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
        //
        // TWO OFFSETS, TWO QUESTIONS, and confusing them is the trap here:
        //
        //   woff         how much of wbuf has been HANDED TO THE TRANSPORT
        //                  plaintext: bytes written to the socket
        //                  TLS:       bytes fed into the Session, which have NOT
        //                             necessarily left the machine
        //   tls_out_off  how much ciphertext has actually reached the socket
        //
        // So on a TLS connection `woff >= wbuf.size()` means "fully encrypted",
        // never "fully sent". Ask tls_wbuf_flushed() for the second question.
        //
        // Every existing `woff` comparison is plaintext-only and sits after a TLS
        // early return, which is correct but invisible at the line itself. A
        // transport-agnostic wrapper was tried and reverted: every caller is
        // already inside a TLS-only branch, so its plaintext half was unreachable
        // and it resolved to tls_wbuf_flushed() at every site. It read like a
        // safety net while changing nothing, which is worse than no wrapper.
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
        uint64_t client_setup_timeouts = 0; // clients dropped for never completing a
                                            // first request (stall, or a client
                                            // speaking the wrong protocol at us)
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
        // How long a client may stay connected without completing one request.
        // Covers a TLS handshake that never finishes, a half-sent request, and a
        // client speaking the wrong protocol (TLS at a plaintext listener frames
        // as garbage that never completes). Generous on purpose: a slow mobile
        // client on a cold TLS handshake is a real thing, and this only has to
        // bound the hold, never be tight.
        static constexpr int64_t kClientSetupNs = 30LL * 1000 * 1000 * 1000; // 30 s

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

        // Test seam. Production never calls this; the default is kClientSetupNs.
        void set_client_setup_ns(int64_t ns) noexcept { _client_setup_ns = ns; }

        // Test seam: does any POOLED upstream still hold `needle` in its request
        // buffer? A pooled connection idles for up to 30 s and is then handed to
        // whichever client asks next, so a credential left in that buffer outlives
        // the request that supplied it.
        //
        // It answers yes or no and NEVER returns or logs buffer contents, which is
        // the only shape a credential-adjacent accessor should have. It exists
        // because a mutation sweep showed secure_clear() could be deleted with no
        // test noticing, and the invariant is not observable from outside the
        // process by any other means.
        [[nodiscard]] bool pooled_buffer_contains(std::string_view needle) const noexcept;
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

        // Is this upstream carrying TLS? Shared by both backends (hence no
        // ep_/ur_ prefix) and declared outside the TLS guard because the t2
        // stamp sites call it in every build; it is a constant `false` when the
        // project is built without TLS.
        // End a stream WITHOUT a terminal [DONE], because it is being aborted.
        // One call so the three mutations it needs cannot be split up; five sites
        // across both backends used to write them out by hand.
        void stream_truncate(Connection* client) noexcept;
        bool upstream_is_tls(const Connection* u) const noexcept;

#ifdef LLMBRIDGE_HAVE_TLS
        // ── TLS plumbing, BOTH directions ───────────────────────────────────
        // These are no-op-safe building blocks shared by the two backends. The
        // parameter is `c` and not `u` on purpose: since inbound TLS landed these
        // run on client connections too, and `u` means upstream everywhere else in
        // this file. Direction is read from `c->is_client`, never assumed.
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
        // Defaults to kClientSetupNs. Settable ONLY so tests can pick a deadline
        // they can wait for: at 30 seconds the behaviour was unverifiable, and a
        // mutation sweep confirmed that deleting the deadline entirely broke no
        // test. An untested deadline on an internet-facing listener is the same as
        // no deadline, because nothing would tell us when it stopped working.
        int64_t _client_setup_ns = kClientSetupNs;
        TranslateMode _translate;
        IoBackend _io;
        int64_t _upstream_idle_ns;         // 0 = no idle timeout
        TlsConfig _tls; // BOTH legs; each flag inits its own context in the ctor
        bool _timing_headers = false;      // emit x-llmbridge-* timing on responses
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
