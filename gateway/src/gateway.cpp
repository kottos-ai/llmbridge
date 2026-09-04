// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "gateway/gateway.hpp"

#include "net/secure.hpp"
#include "request.hpp"
#include "response.hpp"
#include "scan.hpp"
#include "stream.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string_view>

#include "net/socket_util.hpp"
#include "net/uring.hpp" // self-guarded by LLMBRIDGE_HAVE_URING

namespace llmbridge
{
    // The helpers the methods below call live beside this file, by concern:
    // request.hpp, response.hpp, scan.hpp and stream.hpp. Pulled in unqualified so
    // the method bodies read as they did when the helpers were in this file.
    using namespace detail;

    namespace
    {
        constexpr size_t kInitialBuf = 4096;
        constexpr int kEpMaxEvents = 1024;
        constexpr int kPollTickMs = 200; // so request_stop() is observed promptly

        // Build the auth/extra header lines to inject into the translated
        // upstream request, from the client's request headers.
        //
        // Whitelist, not passthrough: the gateway rebuilds the upstream request,
        // and only the credential headers the target dialect understands may
        // cross the translation boundary. Echoing arbitrary client headers
        // through a rebuilt request is a smuggling surface (and our own framing
        // headers must stay authoritative). A byte-forward is untouched by all of
        // this: it already carries every client header.
        //
        // Values re-emitted here cannot contain CR/LF: find_header() bounds each
        // value by its own line's CRLF, so injection via a crafted credential is
        // structurally impossible instead of filtered.
        //
        // The credential is handled as a transient string_view over the client's
        // request buffer and written straight into the upstream bytes; it is
        // never copied anywhere that outlives the request, and never logged.
        // Erase a credential-bearing buffer before it is released or pooled.
        // See net/secure.hpp for why this is not just memset, and why it is a
        // detected platform primitive instead of a compiler trick.
        //
        // Scope is deliberately narrow: the only place a credential outlives its
        // request is a pooled upstream, which idles up to _pool_idle_ns (30 s default)
        // holding the request that carried the key. Transient buffers are
        // overwritten microseconds later and are not worth a hot-path memset.
        // Measured: 2.4 ns for a typical ~96 B request buffer, once per request
        // (~0.02% of one core at 84k RPS).
        using llmbridge::net::secure_clear;

        // Total order across all workers, independent of any clock.
        //
        // std::atomic, not volatile: volatile provides neither atomicity nor
        // inter-thread ordering in C++ (it is for memory-mapped I/O), and workers are
        // std::threads sharing this process, so a plain or volatile counter would be a
        // data race that hands two requests the same number.
        //
        // relaxed is sufficient and is the cheapest correct choice: every atomic has a
        // single total modification order, so fetch_add yields unique, increasing
        // values in the order the increments occurred. We need uniqueness and
        // ordering of the counter itself, not ordering of surrounding memory, so no
        // fences are warranted.
        //
        // Why this exists at all: two requests can share a nanosecond, and clocks on
        // different hosts cannot be trusted to sub-millisecond agreement without PTP.
        // (t0, seq) is a total order that needs neither. This is the sequencer
        // pattern: an exchange defines order by arrival at a sequencing point, not by
        // comparing timestamps, and it is why the tape for inference should sequence
        // instead of timestamp.
        std::atomic<uint64_t> g_seq{0};

    } // namespace

    // The single-upstream form, kept because it is what most callers want and because
    // rewriting every existing call site to build a one-entry vector would be churn
    // with no reader benefit. Outbound TLS still arrives through TlsConfig here; the
    // table form takes it per upstream.
    Gateway::Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                     int64_t warmup_ns, UpstreamDialect dialect, IoBackend io,
                     int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers,
                     Policy* policy, std::vector<std::string> strip_headers)
        : Gateway(listen_port,
                  std::vector<Upstream>{Upstream{.ip = std::move(upstream_ip),
                                                 .port = upstream_port,
                                                 .tls = tls.upstream_tls,
                                                 .sni_host = tls.sni_host,
                                                 .dialect = dialect}},
                  warmup_ns, io, upstream_idle_ns, tls, timing_headers, policy,
                  std::move(strip_headers))
    {
    }

    Gateway::Gateway(uint16_t listen_port, std::vector<Upstream> upstreams, int64_t warmup_ns,
                     IoBackend io, int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers,
                     Policy* policy, std::vector<std::string> strip_headers)
        : _listen_port(listen_port), _upstreams(std::move(upstreams)), _warmup_ns(warmup_ns),
          _io(io), _upstream_idle_ns(upstream_idle_ns), _tls(std::move(tls)),
          _timing_headers(timing_headers), _policy(policy)
    {
        // Every path below indexes the table without a bounds special case, so an empty
        // one is a programming error caught here and not a crash on the first request.
        if (_upstreams.empty()) throw std::runtime_error("Gateway: no upstreams configured");
        for (Upstream& u : _upstreams)
        {
            u.host_hdr = host_header_for(u);
            u.aws_region = aws_region_for(u);
            if (!u.query.empty() && u.dialect != UpstreamDialect::Azure)
                throw std::runtime_error(
                    "upstream '" + u.sni_host + "' has a query (" + u.query +
                    ") but its mode does not build its own request target; only "
                    "--translate azure may carry one");
        }
        _idle_upstreams.resize(_upstreams.size());
        // Normalize once, at construction: lower-case with the colon, so the hot path
        // compares against a raw header line with no per-request work.
        for (std::string& h : strip_headers)
        {
            for (char& c : h) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            if (h.empty()) continue;
            if (h.back() != ':') h.push_back(':');
            _strip_headers.push_back(std::move(h));
        }

#ifdef LLMBRIDGE_HAVE_TLS
        if (any_upstream_tls())
        {
            // Setup path: a bad trust store must fail construction, not the first
            // request. Same throw discipline as the listener below.
            net::tls::Context::ClientOptions o;
            o.ca_file = _tls.ca_file;
            if (!_tls_upstream_ctx.init_client(o))
                throw std::runtime_error("TLS context init failed: " + _tls_upstream_ctx.last_error());
        }
        if (_tls.client_tls)
        {
            // Same discipline as above and for a sharper reason: a missing or
            // unreadable certificate must stop the process, never downgrade the
            // listener to plaintext. A client that dialled https and got a
            // plaintext answer would send its credential in the clear.
            net::tls::Context::ServerOptions so;
            so.cert_file = _tls.cert_file;
            so.key_file = _tls.key_file;
            if (!_tls_client_ctx.init_server(so))
                throw std::runtime_error("inbound TLS init failed: " + _tls_client_ctx.last_error());
        }
#else
        if (any_upstream_tls())
            throw std::runtime_error("TLS upstream requested but built without LLMBRIDGE_TLS");
#endif
        // Linux has no SO_NOSIGPIPE; ignore SIGPIPE process-wide so a write to a
        // peer-closed socket returns EPIPE instead of killing us. Idempotent, so
        // safe to set here (covers the daemon and the test harness alike).
        std::signal(SIGPIPE, SIG_IGN);
        _epfd = ::epoll_create1(0);
        if (_epfd < 0) throw std::runtime_error("epoll_create1() failed");
        _listen_fd = net::make_listener(_listen_port);
        if (_listen_fd < 0) throw std::runtime_error("failed to bind listen port");
        _listen_conn = new Connection();
        _listen_conn->fd = _listen_fd;
        ep_add_read(_listen_conn);
        LB_INFO("listening port=", _listen_port, " upstreams=",
                static_cast<int64_t>(_upstreams.size()),
                _tls.client_tls ? " inbound=tls" : " inbound=plaintext");
        if (!_tls.client_tls)
            LB_WARN("inbound TLS is off and the listener accepts every interface, so a "
                    "client's Authorization crosses the network in clear. Safe as a "
                    "loopback sidecar behind something that terminates TLS; unsafe as a "
                    "remote endpoint. Build -DLLMBRIDGE_TLS=ON and run --listen-tls "
                    "--tls-cert --tls-key to terminate it here.");
        for (size_t i = 0; i < _upstreams.size(); ++i)
            LB_INFO("  upstream[", static_cast<int64_t>(i), "] ", _upstreams[i].ip, ":",
                    _upstreams[i].port, _upstreams[i].tls ? " tls" : " plaintext",
                    " dialect=", dialect_name(_upstreams[i].dialect));

        // Resolve the event-loop backend: io_uring for Uring/Auto when the kernel
        // supports it, else epoll. Uring requested but unavailable -> epoll.
#ifdef LLMBRIDGE_HAVE_URING
        const bool uring_ok = net::uring::available();
        if (_io == IoBackend::Uring || _io == IoBackend::Auto) _uring_active = uring_ok;
        if (_io == IoBackend::Uring && !uring_ok)
            LB_WARN("io_uring requested but unavailable; falling back to epoll");
        else if (_io == IoBackend::Auto && !uring_ok)
            LB_WARN("io_uring unavailable, using epoll. Inside a container this is "
                    "usually the default seccomp profile blocking io_uring_setup: run "
                    "with --security-opt seccomp=unconfined, or a profile that permits "
                    "the io_uring syscalls, to get the faster backend");
#endif
        const char* want = _io == IoBackend::Uring ? "uring" : _io == IoBackend::Epoll ? "epoll" : "auto";
        LB_INFO("backend requested=", want, " active=", _uring_active ? "io_uring" : "epoll");
    }

    Gateway::~Gateway()
    {
        for (auto& [id, c] : _clients)
        {
            // An in-flight (acquired, not pooled) upstream is reachable only via
            // peer: free it too, or it leaks when we stop mid-request. (The
            // io_uring loop already nulls these during its drain.)
            if (Connection* u = c->peer) { if (u->fd >= 0) ::close(u->fd); delete u; }
            if (c->fd >= 0) ::close(c->fd);
            delete c;
        }
        for (auto& pool : _idle_upstreams)
            for (Connection* u : pool) { if (u->fd >= 0) ::close(u->fd); delete u; }
        for (Connection* d : _doomed) delete d;
        if (_listen_fd >= 0) ::close(_listen_fd);
        if (_epfd >= 0) ::close(_epfd);
        delete _listen_conn;
    }

    uint16_t Gateway::bound_port() const noexcept
    {
        if (_listen_fd < 0) return 0;
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) return 0;
        return ntohs(addr.sin_port);
    }

    // epoll is level-triggered here (no EPOLLET): every readable handler first
    // drains the socket into rbuf, so EPOLLIN won't re-fire on unread bytes, and
    // EPOLLIN stays armed for the connection's whole life. Write interest is
    // toggled via EPOLL_CTL_MOD on top of that always-on EPOLLIN.
    void Gateway::ep_add_read(Connection* c) noexcept
    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_ADD, c->fd, &ev);
    }

    void Gateway::ep_arm_write(Connection* c) noexcept
    {
        if (c->write_armed) return;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->write_armed = true;
    }

    void Gateway::ep_disarm_write(Connection* c) noexcept
    {
        if (!c->write_armed) return;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->write_armed = false;
    }

    // Backpressure: pause/resume reading a connection. Used to stop pulling
    // upstream SSE bytes while the client's write buffer is draining, so a slow
    // client can't make us buffer an unbounded stream. (EPOLLHUP/EPOLLERR still
    // fire while paused, so an upstream close is never missed.)
    void Gateway::ep_pause_read(Connection* c) noexcept
    {
        if (c->read_paused) return;
        epoll_event ev{};
        ev.events = c->write_armed ? static_cast<uint32_t>(EPOLLOUT) : 0u;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->read_paused = true;
        ++_stats.stream_pauses; // observability: proves backpressure actually engaged
        // Both guards above are edge-triggered (`read_paused` early-returns), so this
        // is one line per episode, not per event. WARN like every other cap: at the
        // default INFO floor a DEBUG line is compiled out, which would make the epoll
        // half of this behaviour invisible in production while the io_uring half
        // (the 8 MiB drop) stayed visible. Same event, two backends, one log.
        LB_WARN("CAP backpressure, pausing upstream reads ", *c,
                " (the client is slower than the provider)");
    }

    void Gateway::ep_resume_read(Connection* c) noexcept
    {
        if (!c->read_paused) return;
        epoll_event ev{};
        ev.events = static_cast<uint32_t>(EPOLLIN) | (c->write_armed ? static_cast<uint32_t>(EPOLLOUT) : 0u);
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->read_paused = false;
        // Same level as the pause: a log that opens an episode and never closes it
        // reads as still stuck.
        LB_WARN("CAP backpressure cleared, resuming upstream reads ", *c);
    }

    bool Gateway::ep_drain_read(Connection* c) noexcept
    {
        char tmp[16384];
        size_t pulled = 0;
        for (;;)
        {
            ssize_t n = ::read(c->fd, tmp, sizeof(tmp));
            if (n > 0)
            {
                if (c->ts_first_byte == 0) c->ts_first_byte = now_ns();
                c->rbuf.append(tmp, static_cast<size_t>(n));
                pulled += static_cast<size_t>(n);
                // Level-triggered: whatever is left re-notifies. Stopping here is what
                // lets back-pressure engage between events instead of after an
                // unbounded burst. See kEpMaxReadPerEvent.
                if (pulled >= kEpMaxReadPerEvent) return true;
                if (static_cast<size_t>(n) < sizeof(tmp)) return true;
                continue;
            }
            if (n == 0) return false;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;
        }
    }

    bool Gateway::ep_pump_write(Connection* c, bool* done) noexcept
    {
        *done = false;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_invariant_ok(c)) return false; // never plaintext on a TLS conn
        // Branching here and not at the call sites is deliberate: ep_respond,
        // ep_on_client_writable and the SSE stream flush all reach the socket
        // through this one function, so inbound TLS covers streaming for free
        // instead of needing a fourth copy of the logic.
        if (c->tls)
        {
            if (c->tls->handshake_done()) tls_push_wbuf(c);
            bool flushed = false;
            if (!ep_tls_flush(c, &flushed)) return false;
            *done = flushed && tls_wbuf_flushed(c);
            return true;
        }
#endif
        while (c->woff < c->wbuf.size())
        {
            ssize_t n = ::write(c->fd, c->wbuf.data() + c->woff, c->wbuf.size() - c->woff);
            if (n > 0) { c->woff += static_cast<size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        // Leave wbuf intact on completion: the request stays available for a
        // stale-connection resend until the response is read; callers clear it.
        *done = true;
        return true;
    }

    // Shared by both backends, hence no ep_/ur_ prefix: is this upstream carrying
    // TLS? Compiles to `false` in a build without TLS support.
    // A compressed stream is forwarded correctly and measured not at all: the token
    // counts live in bytes we cannot read. request_without drops Accept-Encoding so
    // this should not happen, so if it does the provider compressed unasked and the
    // operator needs to know why the tape went quiet, and not read a dash and guess.
    // What the provider said about its own limits, kept from the response head.
    // Shared by both backends and by the streaming and non-streaming paths, so the
    // four cannot disagree about what a refusal meant.
    void Gateway::note_quota(Connection* client, const net::http::ResponseHead& h) noexcept
    {
        client->quota_exhausted = static_cast<uint8_t>(h.quota_exhausted);
        client->retry_after_s = h.retry_after_s;
    }

    void Gateway::note_upstream_error(Connection* client, const net::http::ResponseHead& h,
                                      std::string_view body) noexcept
    {
        client->upstream_error_len = 0;
        // 0 is a head we never framed, and a 3xx is not the venue naming a failure.
        if (h.status < 400) return;
        const std::string_view t = scan_error_type(body);
        const size_t n = t.size() < sizeof client->upstream_error
                             ? t.size()
                             : sizeof client->upstream_error;
        client->upstream_error_len = static_cast<uint8_t>(n);
        if (n) std::memcpy(client->upstream_error, t.data(), n);
    }

    void Gateway::stream_warn_if_encoded(const Connection* client,
                                         const net::http::ResponseHead& h) noexcept
    {
        if (!h.encoded) return;
        LB_WARN(ReqId{client->req_seq},
                " upstream compressed the response, so token counts are not readable "
                "for this request; Accept-Encoding was not forwarded");
    }

    // request-path and the handshake do not care what shape the response takes, so a
    // stream records them like any other request. resp_path and added-total it cannot
    // have: both end at the instant a response was built. See LATENCY.md section 4.
    void Gateway::stream_record_latency(const Connection* client) noexcept
    {
        if (now_ns() - _t_start < _warmup_ns) return;
        // t4 stands in for the absent t5; only the request-side fields are read.
        const TimingSplit sp = timing_split(client->ts_req_recvd, client->ts_req_built,
                                            client->ts_wire_ready, client->ts_up_sent,
                                            client->ts_up_recvd, client->ts_up_recvd);
        if (sp.req_path_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(sp.req_path_ns));
        if (sp.connect_ns >= 0) _stats.connect.record(static_cast<uint64_t>(sp.connect_ns));
        // Zero means no content chunk ever arrived, not an instant answer.
        if (client->ts_first_token > 0 && client->ts_req_recvd > 0 &&
            client->ts_first_token >= client->ts_req_recvd)
            _stats.first_token.record(
                static_cast<uint64_t>(client->ts_first_token - client->ts_req_recvd));
    }

    void Gateway::stream_truncate(Connection* client) noexcept
    {
        // Abort a stream honestly. No terminal [DONE] is emitted, deliberately:
        // fabricating one would tell the client it received a complete answer when
        // it did not, and for an agent loop that is a silent wrong result rather
        // than a visible failure.
        //
        // The three mutations are one decision, so they live in one place. Five
        // sites across both backends wrote them out by hand, which is how a sixth
        // ends up setting two of the three.
        // Load-bearing, and now guarded. {ep,ur}_stream_flush call finalize_stream
        // Only when this is set; without it they resume reading from an upstream
        // that will never speak again, and the client socket is held until the
        // 120 s idle sweep. A mutation sweep found no test that noticed, because
        // every mock closed promptly and the client went away either way.
        // CorruptStreamFromAProviderThatHoldsTheConnectionStillClosesTheClient
        // supplies the missing provider: it corrupts the chunked framing inside
        // valid TLS and then holds the connection, so the only difference left is
        // When the client is closed. Deleting this line makes that test fail.
        // Logged here, in the one shared place, and not at the five call sites: a
        // truncated stream is the one outcome a client cannot tell from a clean
        // finish, because the honest absence of [DONE] looks like a connection that
        // simply stopped. If it is not recorded on our side, nobody can answer
        // "did my stream complete?" afterwards. The abort paths bypass
        // finalize_stream, so its completion line never runs for these.
        LB_WARN(ReqId{client->req_seq}, " stream TRUNCATED (no [DONE] emitted)",
                " tokens_in=", stream_tokens(client).in,
                " tokens_out=", stream_tokens(client).out,
                " on ", *client);
        client->stream_ended = true;      // no more output will be produced
        client->close_after_resp = true;  // close once the client drains what we have
        ++_stats.errors;                  // and it counts as a failure, not a finish
    }

    bool Gateway::pooled_buffer_contains(std::string_view needle) const noexcept
    {
        if (needle.empty()) return false;
        for (const auto& pool : _idle_upstreams)
            for (const Connection* u : pool)
                if (u->wbuf.find(needle) != std::string::npos) return true;
        return false;
    }

    bool Gateway::upstream_is_tls(const Connection* u) const noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        return u->tls != nullptr;
#else
        (void)u;
        return false;
#endif
    }

#ifdef LLMBRIDGE_HAVE_TLS
    // ── TLS plumbing ────────────────────────────────────────────────────────
    // The invariant (see gateway.hpp): rbuf/wbuf are plaintext, always. TLS lives
    // strictly between the socket and those buffers. For a TLS upstream, `woff`
    // counts plaintext fed into the Session (so retry-resend still works from
    // wbuf), and tls_out/tls_out_off track the encrypted bytes towards the socket.

    bool Gateway::tls_required(const Connection* c) const noexcept
    {
        return c->is_client ? _tls.client_tls : upstream_of(c).tls;
    }

    bool Gateway::tls_invariant_ok(Connection* c) noexcept
    {
        if (c->tls || !tls_required(c)) return true;

        // A connection the configuration says must be encrypted has no Session, so
        // refuse the write and let the caller tear it down. On the client leg those
        // bytes would be a response to a peer that dialled TLS; on the upstream leg
        // the next thing sent is the provider credential. Both are disclosures.
        //
        // This cannot fire today, and a mutation sweep confirms no test can tell it
        // from `return true`. It is kept anyway, and the reason is specific rather
        // than defensive-in-general: what makes it unreachable is that six separate
        // call sites each close the connection when tls_attach_* fails. That is an
        // invariant re-established by hand, in six places, and two of the six were
        // added the week inbound TLS landed. A seventh is not a hypothetical, it is
        // the established pattern of this file, and this is the one line that would
        // catch it.
        //
        // Contrast a guard whose precondition is a single structural fact: that one
        // is noise and was deleted. The test is not "can I prove this fires", it is
        // "how many places must stay right for it to stay unreachable".
        ++_stats.errors;
        return false;
    }

    bool Gateway::tls_attach_upstream(Connection* u) noexcept
    {
        // The venue's own hostname, never _tls.sni_host. init_client drives both SNI
        // and SSL_set1_host from this, so a global value would verify every venue's
        // certificate against the first venue's name: a valid-cert-wrong-server hole
        // across the table. u->upstream_slot is set at every caller before this runs.
        u->tls = std::make_unique<net::tls::Session>();
        if (!u->tls->init_client(_tls_upstream_ctx, upstream_of(u).sni_host))
        {
            u->tls.reset();
            return false;
        }
        return true;
    }

    bool Gateway::tls_attach_client(Connection* c) noexcept
    {
        c->tls = std::make_unique<net::tls::Session>();
        if (!c->tls->init_server(_tls_client_ctx))
        {
            c->tls.reset();
            return false;
        }
        // Accept state is already set by init_server. The handshake advances on the
        // first readable event: the client speaks first with a ClientHello, so there
        // is nothing to emit here and nothing to wait for.
        return true;
    }

    void Gateway::tls_pump_out(Connection* u) noexcept
    {
        // io_uring caution: callers there must only pump while no send SQE is in
        // flight on this conn; appending can reallocate tls_out under the kernel.
        // Reserved once for what this pass moves. Growing by append cost a 4 MB
        // request 1.6 ms of reallocation on a connection's first use, more than the
        // encryption itself.
        const size_t need = u->tls_out.size() + u->tls->pending_output_bytes() + 256;
        if (u->tls_out.capacity() < need) u->tls_out.reserve(need);
        uint8_t buf[65536];
        size_t n;
        while ((n = u->tls->pull_ciphertext({buf, sizeof buf})) > 0)
            u->tls_out.append(reinterpret_cast<const char*>(buf), n);
        // Unsent only: tls_out keeps the prefix already written until a full drain
        // clears it, so counting size() measured total throughput, not backlog.
        const uint64_t unsent = static_cast<uint64_t>(u->tls_out.size() - u->tls_out_off) +
                                u->tls->pending_output_bytes();
        if (unsent > _stats.tls_buffered_peak) _stats.tls_buffered_peak = unsent;
    }

    void Gateway::tls_push_wbuf(Connection* u) noexcept
    {
        // Feed as much request plaintext as the Session accepts. The ciphertext
        // lands in tls_out directly, except while an io_uring send SQE points into
        // tls_out.
        const bool direct = !u->send_inflight;
        if (direct)
        {
            tls_pump_out(u); // staged handshake flights go first: order is the wire's
            const size_t todo = u->wbuf.size() > u->woff ? u->wbuf.size() - u->woff : 0;
            const size_t need = u->tls_out.size() + todo + todo / 512 + 256;
            if (u->tls_out.capacity() < need) u->tls_out.reserve(need);
            u->tls->set_sink(&u->tls_out);
        }
        while (u->woff < u->wbuf.size())
        {
            const auto* p = reinterpret_cast<const uint8_t*>(u->wbuf.data()) + u->woff;
            const size_t n = u->tls->write_plaintext({p, u->wbuf.size() - u->woff});
            if (n == 0) break; // handshake not done, or session back-pressured
            u->woff += n;
        }
        u->tls->set_sink(nullptr);
    }

    bool Gateway::tls_wbuf_flushed(const Connection* u) const noexcept
    {
        return u->woff >= u->wbuf.size() && u->tls_out_off >= u->tls_out.size() &&
               !u->tls->has_pending_output();
    }

    bool Gateway::tls_feed(Connection* u, const char* p, size_t n) noexcept
    {
        const bool hs_was_done = u->tls->handshake_done();
        if (n > 0 &&
            u->tls->feed_ciphertext({reinterpret_cast<const uint8_t*>(p), n}) != n)
        {
            LB_WARN("TLS input refused ", *u, " handshake_done=", hs_was_done,
                    " err=", u->tls->last_error());
            return false;
        }
        if (u->tls->want() == net::tls::Want::Error)
        {
            // Session::last_error() was consumed nowhere in shipped code until now, so
            // a failed handshake produced a closed socket and no diagnostic at all.
            // Wrong CA, wrong hostname and a protocol mismatch all looked identical
            // from outside, which is the first thing a new deployment hits. The string
            // is an OpenSSL reason plus a file path; it carries no key material.
            //
            // A CLIENT-side handshake failure on a public listener is almost always an
            // internet scanner speaking junk TLS, and at WARN it floods the journal and
            // buries real signal. Log it at DEBUG so a production (info-floor) build
            // stays quiet and a debug build still surfaces it when diagnosing one real
            // client. Everything else, an upstream handshake to a provider or a
            // mid-session failure on either leg, stays WARN because it is actionable.
            const bool scanner_noise = u->is_client && !hs_was_done;
            if (scanner_noise)
            {
                ++_stats.client_tls_handshake_failures;
                // Counted, not merely silenced. The line is noise; the rate is a
                // diagnostic, and a customer who cannot handshake shows up as a step
                // in it, and not as nothing at all.
                LB_DEBUG("TLS failed ", *u, " during=handshake err=", u->tls->last_error());
            }
            else
                LB_WARN("TLS failed ", *u, " during=", hs_was_done ? "session" : "handshake",
                        " err=", u->tls->last_error());
            return false;
        }

        // Drain whatever plaintext became available into rbuf. From here on the
        // existing HTTP framing / SSE pump code takes over unchanged.
        uint8_t buf[16384];
        size_t r;
        while ((r = u->tls->read_plaintext({buf, sizeof buf})) > 0)
        {
            // Client-side TLS lands here too, so the arrival stamp belongs here as
            // much as on the plaintext reads. Harmless on an upstream connection,
            // whose `client_upload_ns` nobody reads.
            if (u->ts_first_byte == 0) u->ts_first_byte = now_ns();
            u->rbuf.append(reinterpret_cast<const char*>(buf), r);
        }
        if (u->tls->want() == net::tls::Want::Error)
        {
            LB_WARN("TLS read failed ", *u, " err=", u->tls->last_error());
            return false;
        }

        // Handshake completed on this feed. On an upstream conn the request has
        // been waiting in wbuf and can finally go through the Session. On an
        // Inbound conn there is nothing pending, because the client speaks first
        // and its request arrives as plaintext out of this very call; t2 is an
        // upstream concept and stamping it here would be meaningless.
        if (!hs_was_done && u->tls->handshake_done() && u->is_client)
        {
            // The inbound handshake finishes before t0 (t0 is the framed request),
            // so it is invisible to every other number this file reports. Recorded
            // here or it cannot be seen at all. Warm-up gated like the others.
            const int64_t now = now_ns();
            LB_DEBUG("TLS established ", *u);
            if (u->ts_accepted > 0 && now - _t_start >= _warmup_ns)
                _stats.accept_tls.record(static_cast<uint64_t>(now - u->ts_accepted));
        }
        if (!hs_was_done && u->tls->handshake_done() && !u->is_client)
        {
            // t2 belongs here for TLS, not at TCP connect. The wire cannot carry
            // the request until the handshake is done, so stamping t2 earlier put
            // the entire handshake inside upwrite-us (t3-t2) and left connect-us
            // reporting the TCP leg alone. See the attribution test in
            // gateway/tests/gateway_tls_test.cpp.
            if (u->peer) u->peer->ts_wire_ready = now_ns();
            if (u->woff < u->wbuf.size()) tls_push_wbuf(u);
        }
        return true;
    }

    bool Gateway::ep_tls_flush(Connection* u, bool* done) noexcept
    {
        *done = false;
        tls_pump_out(u); // epoll: no in-flight send to worry about; always safe
        while (u->tls_out_off < u->tls_out.size())
        {
            const ssize_t n = ::write(u->fd, u->tls_out.data() + u->tls_out_off,
                                      u->tls_out.size() - u->tls_out_off);
            if (n > 0) { u->tls_out_off += static_cast<size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                ep_arm_write(u);
                return true;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        u->tls_out.clear();
        u->tls_out_off = 0;
        *done = true;
        return true;
    }

    bool Gateway::ep_tls_drain_read(Connection* u) noexcept
    {
        char tmp[16384];
        size_t pulled = 0;
        for (;;)
        {
            const ssize_t n = ::read(u->fd, tmp, sizeof tmp);
            if (n > 0)
            {
                if (!tls_feed(u, tmp, static_cast<size_t>(n))) return false;
                pulled += static_cast<size_t>(n);
                if (pulled >= kEpMaxReadPerEvent) break; // see ep_drain_read
                if (static_cast<size_t>(n) < sizeof tmp) break;
                continue;
            }
            if (n == 0) return false; // EOF
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
        // Feeding may have produced output (handshake flights, the request itself
        // once the handshake completed). Put it on the wire before returning to
        // the parse logic, and stamp the request-sent time on the transition.
        const bool was_flushed = u->peer && tls_wbuf_flushed(u);
        bool done = false;
        if (!ep_tls_flush(u, &done)) return false;
        if (u->peer && !was_flushed && tls_wbuf_flushed(u))
            u->peer->ts_up_sent = now_ns();
        return true;
    }
#endif // LLMBRIDGE_HAVE_TLS

    namespace
    {
        // May this streaming upstream go back into the keep-alive pool?
        //
        // Same spirit as the non-streaming rule ("pool only what will stay open"),
        // plus the framing conditions that make the end of the body knowable. Every
        // clause is load-bearing:
        //
        //   stream_keep_alive  the provider didn't say Connection: close; pooling a
        //                      conn it is about to close just buys a retry later
        //   stream_chunked     a close-delimited body has no end marker except EOF,
        //                      so "the response finished" and "the connection died"
        //                      are indistinguishable, so never reuse one
        //   chunkdec.done()    the terminal 0-length chunk was consumed, so we are
        //                      at a real message boundary instead of mid-body
        //   rbuf empty         no trailing/pipelined bytes left over; anything still
        //                      buffered would be mis-read as the next response
        //   !close_after_resp  aborted, corrupt, or idle-timed-out streams are never
        //                      pooled; we don't trust framing we already distrusted
        //
        // Conservative by construction: any doubt falls through to close, which is
        // exactly the behaviour that shipped before reuse existed.
        bool stream_upstream_reusable(const Connection* client, const Connection* u) noexcept
        {
            return client != nullptr && u != nullptr && !u->doomed && u->fd >= 0
                   && client->stream_keep_alive && client->stream_chunked
                   && client->chunkdec.done() && u->rbuf.empty() && !client->close_after_resp;
        }
    } // namespace

    bool Gateway::ep_upstream_failed(Connection* client, int status, const char* why) noexcept
    {
        const Retry r = failover_target(client, status, why);
        if (!r.retry) return false;

        LB_WARN(ReqId{client->req_seq}, " venue ", static_cast<int64_t>(client->upstream_slot),
                " failed (", why, "); retrying on ", static_cast<int64_t>(r.upstream_index));
        if (Connection* u = client->peer) { client->peer = nullptr; u->peer = nullptr; ep_close_upstream(u); }
        ++client->failover_attempts;
        client->upstream_slot = r.upstream_index;
        // Put the original request back at the front of rbuf. ep_forward rebuilds it for
        // the new venue's dialect and erases exactly these bytes again, so a pipelined
        // request already queued behind it keeps its place.
        client->rbuf.insert(0, client->failover_req);
        ++_stats.upstream_failovers;
        ep_forward(client);
        return true;
    }

    Connection* Gateway::ep_acquire_upstream(int slot) noexcept
    {
        const Upstream& up = _upstreams[static_cast<size_t>(slot)];
        auto& pool = _idle_upstreams[static_cast<size_t>(slot)];
        // Only this venue's pool: a connection to one provider cannot serve a request
        // bound for another, and handing one over would send the request, and its
        // credential, to the wrong company.
        if (!pool.empty())
        {
            Connection* u = pool.back();
            pool.pop_back();
            u->from_pool = true; // reused -> a pre-response failure is retry-eligible
            u->retried = false;  // fresh request: one retry available again
            ++_stats.upstream_reused;
            return u;
        }
        int fd = net::start_connect(up.ip.c_str(), up.port);
        if (fd < 0) return nullptr;
        Connection* u = new Connection();
        u->fd = fd;
        u->is_client = false;
        u->from_pool = false;
        u->upstream_slot = slot;
        u->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (up.tls && !tls_attach_upstream(u))
        {
            ::close(fd);
            delete u;
            return nullptr;
        }
#endif
        ep_add_read(u);
        ep_arm_write(u); // learn when the non-blocking connect completes
        ++_stats.upstream_conns_opened;
        // Pairs with "upstream reuse": without it an upstream first appears in
        // the log at its second request, never at its birth.
        LB_DEBUG("upstream open ", *u, " pool=", _idle_upstreams.size());
        return u;
    }

    bool Gateway::ep_retry_upstream(Connection* u) noexcept
    {
        // Stale pooled connection: reused from the keep-alive pool, not yet retried,
        // and failed before sending any response. The provider almost certainly
        // dropped it idle without processing. Resend the request once on a fresh
        // connection instead of failing the client. (Same rule as the io_uring path.)
        if (!u->from_pool || u->retried || !u->rbuf.empty()) return false;
        Connection* client = u->peer;
        if (!client) return false;
        // The same venue: a retry that lands elsewhere is a silent reroute, and the
        // request was translated for this dialect and carries its credential.
        const Upstream& up = upstream_of(u);
        int fd = net::start_connect(up.ip.c_str(), up.port);
        if (fd < 0) return false;

        Connection* uf = new Connection();
        uf->fd = fd;
        uf->is_client = false;
        uf->from_pool = false;
        uf->upstream_slot = u->upstream_slot;
        uf->retried = true; // this request's one allowed retry is now spent
        uf->wbuf = std::move(u->wbuf); // plaintext, re-pushed through the new session
        uf->woff = 0;
        uf->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (up.tls && !tls_attach_upstream(uf))
        {
            ::close(fd);
            delete uf;
            return false;
        }
#endif
        ep_add_read(uf);
        ep_arm_write(uf); // learn when connect completes, then send the request
        ++_stats.upstream_conns_opened;
        ++_stats.upstream_retries;
        // WARN, not DEBUG: it is not a cap, it is a recovered failure. A pooled
        // connection was dead when we used it, and the client never learns. That is
        // the point of the retry and also the reason it must be visible: a rising
        // rate here means the provider is dropping keep-alives faster than
        // --pool-idle reaps them, which is a tuning signal nobody can see otherwise.
        LB_WARN("upstream stale, resending on a fresh connection ", *u);

        u->peer = nullptr;
        ep_close_upstream(u); // discard the dead connection
        client->peer = uf;
        uf->peer = client;
        return true;
    }

    bool Gateway::upstream_request_sent(const Connection* u) const noexcept
    {
        if (u->send_inflight) return false;      // an SQE still owns the send buffer
        if (u->woff < u->wbuf.size()) return false; // plaintext not fully handed over
#ifdef LLMBRIDGE_HAVE_TLS
        // On TLS, woff only means "fed to the Session". Ciphertext may still be
        // queued in tls_out or inside OpenSSL.
        if (u->tls && (u->tls_out_off < u->tls_out.size() || u->tls->has_pending_output()))
            return false;
#endif
        return true;
    }

    void Gateway::ep_release_upstream(Connection* u) noexcept
    {
        // Bounded pool: past the cap, close instead of accumulate. A streaming
        // gateway pools roughly one upstream per concurrent stream, so an unbounded
        // pool would pin an fd per stream forever.
        if (_idle_upstreams.size() >= kMaxIdleUpstreams)
        {
            LB_WARN("CAP pool full, closing instead of pooling ", *u,
                    " limit=", kMaxIdleUpstreams, " (reuse stops here)");
            ep_close_upstream(u);
            return;
        }
        // Fail closed: the response arrived before our request finished going out, so
        // the provider saw a truncated request and this connection's state is not ours
        // to reason about. Close it; the client keeps the response it already has.
        // See upstream_request_sent().
        if (!upstream_request_sent(u))
        {
            LB_WARN("closing instead of pooling, request was still going out ", *u);
            ++_stats.upstream_unsent;
            ep_close_upstream(u);
            return;
        }
        u->peer = nullptr;
        u->rbuf.clear();
        u->rdec.reset();
        // wbuf held the rebuilt request, including the client's credential, and this
        // connection may now idle for 30 s before being handed to whichever client
        // asks next. Scrub instead of clear.
        //
        // Guarded by ProxyAuth.CredentialIsScrubbedFromAPooledUpstreamBuffer.
        // A mutation sweep found this line could be deleted with nothing failing:
        // every other auth test inspects what reached the upstream, and the leak
        // is in what stays behind.
        secure_clear(u->wbuf);
        u->woff = 0;
        u->msg = net::http::Message{};
#ifdef LLMBRIDGE_HAVE_TLS
        u->tls_out.clear(); // per-request ciphertext; the Session itself is kept
        u->tls_out_off = 0; // pooled reuse must not pay a second handshake
#endif
        u->ts_pooled = now_ns(); // idle-eviction baseline
        ep_disarm_write(u);
        // A pooled connection must be readable. A slow client pauses its upstream's
        // reads, and ep_stream_flush's stream_ended branch returns before the resume
        // below it, so an upstream can reach here with its interest mask at 0. Handed
        // to the next client it would never report readable again: that client's
        // request goes out, the response never arrives, and it hangs until the idle
        // timeout. Resumed here, and not at that one call site, because this is
        // the only door into the pool and the invariant is about what is in it.
        //
        // Not a repair for a reproduced failure: the interleaving needs the final
        // flush to be the one that paused, and no test provokes it. It is an
        // invariant, and pooling a read-disarmed connection cannot be right.
        ep_resume_read(u);
        _idle_upstreams[static_cast<size_t>(u->upstream_slot)].push_back(u);
    }

    // close_* defer the free to the end of the epoll batch: an earlier event in
    // the same batch can close a conn that a later event still references via
    // data.ptr (e.g. a peer aborted while its own fd is also ready), so freeing
    // inline would dangle that pointer. Events for doomed conns are skipped in
    // run().
    void Gateway::ep_close_client(Connection* c) noexcept
    {
        if (c->doomed) return;
        if (c->id)
        {
            _clients.erase(c->id);
            LB_DEBUG("close ", *c, " clients=", _clients.size());
        }
        if (c->fd >= 0) { ::close(c->fd); c->fd = -1; }
        c->doomed = true;
        _doomed.push_back(c);
    }

    void Gateway::ep_close_upstream(Connection* u) noexcept
    {
        if (u->doomed) return;
        // Its own pool only: a connection is never in another venue's.
        if (u->upstream_slot >= 0)
        {
            auto& pool = _idle_upstreams[static_cast<size_t>(u->upstream_slot)];
            for (auto it = pool.begin(); it != pool.end(); ++it)
                if (*it == u) { pool.erase(it); break; }
        }
        // After the erase, so `pool=` excludes this connection.
        LB_DEBUG("upstream close ", *u, " pool=", pooled_upstream_count());
        if (u->fd >= 0) { ::close(u->fd); u->fd = -1; }
        u->doomed = true;
        _doomed.push_back(u);
    }

    void Gateway::ep_abort_pair(Connection* client) noexcept
    {
        Connection* u = client->peer;
        if (u) { u->peer = nullptr; ep_close_upstream(u); }
        ep_close_client(client);
        ++_stats.errors;
    }

    void Gateway::ep_error_respond(Connection* client, int code, const char* why,
                                   const char* detail) noexcept
    {
        // Null-tolerant to match ur_error_respond exactly. Every current epoll call
        // site already guarantees non-null, but twins with different contracts are
        // a trap: a caller copied from one side to the other inherits the wrong
        // assumption silently.
        if (!client || client->doomed) return;
        // A client-caused refusal is DEBUG; anything we or the provider caused is WARN.
        // Getting that backwards means drowning in 4xx noise at scale or missing an
        // outage, so the level is derived from the code, never chosen per site.
        // An authentication refusal names where it came from.
        const std::string peer = (code == 401 || code == 403) ? peer_of(client->fd)
                                                              : std::string{};
        LB_WARN(ReqId{client->req_seq}, " reply ", code, " ", why, " on ", *client,
                peer.empty() ? "" : " peer=", peer);
        // We're replying to the client ourselves, so drop any in-flight upstream.
        if (Connection* u = client->peer) { client->peer = nullptr; u->peer = nullptr; ep_close_upstream(u); }
        client->wbuf = build_error(code, detail);
        client->woff = 0;
        client->close_after_resp = true; // ep_finish_client closes once it flushes
        ++_stats.errors;
        ep_respond(client);
    }

    void Gateway::ep_on_accept() noexcept
    {
        for (;;)
        {
            int fd = ::accept(_listen_fd, nullptr, nullptr);
            if (fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                return;
            }
            net::set_nonblocking(fd);
            net::set_nodelay(fd);
            net::set_nosigpipe(fd);
            Connection* c = new Connection();
            c->fd = fd;
            c->is_client = true;
            c->id = _next_client_id++;
            c->ts_accepted = now_ns();
        c->ts_client_activity = c->ts_accepted;
            c->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
            if (_tls.client_tls && !tls_attach_client(c))
            {
                // Fail closed. Answering in plaintext because the Session would
                // not attach is how a credential ends up on the wire in the clear.
                ::close(fd);
                delete c;
                ++_stats.errors;
                continue;
            }
#endif
            _clients[c->id] = c;
            LB_DEBUG("accept ", *c, " clients=", _clients.size());
            ep_add_read(c);
        }
    }

    void Gateway::ep_on_client_readable(Connection* c) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        // Inbound TLS: ciphertext off the socket, plaintext into rbuf. Everything
        // below this point, framing included, is unchanged and unaware of TLS.
        const bool ok = c->tls ? ep_tls_drain_read(c) : ep_drain_read(c);
#else
        const bool ok = ep_drain_read(c);
#endif
        if (!ok)
        {
            // EOF/error: mid-request close is a real abort; idle close is normal.
            const bool in_flight = c->peer != nullptr || !c->wbuf.empty();
            if (in_flight) ep_abort_pair(c);
            else ep_close_client(c);
            return;
        }
        // One request in flight at a time per client.
        if (c->peer != nullptr || !c->wbuf.empty()) return;
        if (c->rbuf.empty()) return;

        // Stamp just before framing so the (completing) HTTP parse is counted in
        // the request-path overhead. On a partial read parse returns NeedMore and
        // we discard t0 and return, so inter-packet network wait is never charged
        // to the gateway.
        // A request whose length is already known is not re-parsed per read: the headers
        // were walked once, and the answer does not change until the bytes are in.
        if (c->client_frame_want && c->rbuf.size() < c->client_frame_want) return;
        const int64_t t0 = now_ns();
        net::http::Message m;
        auto st = net::http::parse_request(c->rbuf, m);
        if (st == net::http::FrameStatus::NeedMore)
        {
            if (m.total_len && !c->client_frame_want)
            {
                c->client_frame_want = m.total_len;
                c->rbuf.reserve(m.total_len);
                // No body byte yet: the client is waiting on us, not the network.
                if (c->rbuf.size() <= m.header_len &&
                    expects_continue(std::string_view(c->rbuf.data(), m.header_len)))
                    send_interim_continue(c, /*uring=*/false);
            }
            return;
        }
        c->client_frame_want = 0;
        if (st == net::http::FrameStatus::Error) { ep_error_respond(c, 400, "request framing"); return; }

        c->msg = m;
        c->ts_req_recvd = t0;
        c->client_upload_ns = span_since(c->ts_first_byte, t0);
        c->client_conn_reused = c->ever_framed;
        c->client_conn_setup_ns = c->client_conn_reused ? 0 : span_since(c->ts_accepted, t0);
        // A pipelining client's next request is already here, so its arrival begins
        // now; otherwise the next read stamps it.
        c->ts_first_byte = c->rbuf.size() > m.total_len ? t0 : 0;
        // One assignment point for the sequencer, here and not later: the upstream
        // and status lines below quote it, and ep_forward erases the request out of
        // rbuf, so a log placed after it prints an empty request line and a stale seq.
        c->req_seq = g_seq.fetch_add(1, std::memory_order_relaxed);
        // A new request: the previous one's saved copy and failover budget are spent.
        // Cleared here, at the one place a request begins, and not at the many
        // places one can end.
        c->failover_req.clear();
        c->failover_attempts = 0;
        // Fail closed on a body we cannot read. Nothing here inflates, and every
        // reader downstream scans bytes assuming JSON: wants_stream, model_of and
        // the translator.
        if (c->msg.encoded) { ep_error_respond(c, 415, "compressed request body"); return; }
        c->policy_tag = 0;
        if (_sink) sink_capture(c);
        if (_sink || _policy) capture_model(c);
        LB_DEBUG(ReqId{c->req_seq}, " ", request_line(c->rbuf), " on ", *c);
        // The policy seam, one call site per backend. Here because framing has
        // succeeded but nothing is translated, no credential mapped and no upstream
        // acquired, so a refusal reaches no provider.
        // Default to the first upstream, so a build with no policy, and a policy that
        // only authenticates, both behave exactly as the single-upstream gateway did.
        c->upstream_slot = 0;
        if (_policy)
        {
            const Decision d = policy_decision(c, m);
            if (!d.allow) { ep_error_respond(c, d.deny_status, d.reason); return; }
            // Out of range is the default, not an error: a policy that does not route
            // leaves this at -1, and one that names a venue that has since left the
            // table must not send the request somewhere arbitrary.
            if (d.upstream_index >= 0 &&
                static_cast<size_t>(d.upstream_index) < _upstreams.size())
                c->upstream_slot = d.upstream_index;
            else if (d.upstream_index >= 0)
                LB_WARN(ReqId{c->req_seq}, " policy chose upstream ",
                        static_cast<int64_t>(d.upstream_index), " of ",
                        static_cast<int64_t>(_upstreams.size()), "; using 0");
            // Valid only through the forward below, which runs in this call stack.
            c->model_override = d.model;
            c->tier_override = d.service_tier;
        }
        ep_forward(c); // resolves the translation for the chosen venue; see ep_forward
    }

    // The header names that carry a credential, which is the set this gateway extracts
    // and re-emits: authorization and x-api-key (Bearer / Anthropic), x-goog-api-key
    // (Gemini), and api-key (Azure).
    static bool is_credential_header(std::string_view lowered) noexcept
    {
        return lowered == "authorization" || lowered == "x-api-key" ||
               lowered == "x-goog-api-key" || lowered == "api-key";
    }

    void Gateway::set_request_sink(RequestSink* sink, std::vector<std::string> capture)
    {
        _sink = sink;
        _sink_capture_names.clear();
        for (std::string& h : capture)
        {
            if (_sink_capture_names.size() == kSinkCaptureMax) break;
            for (char& ch : h)
                ch = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
            if (is_credential_header(h))
            {
                LB_ERROR("sink capture refused for header ", h.c_str(),
                         ": it carries a credential, which must never reach a sink");
                continue;
            }
            _sink_capture_names.push_back(std::move(h));
        }
    }

    void Gateway::sink_capture(Connection* c) noexcept
    {
        // The request buffer is reused long before completion, so the sink's header
        // values are copied now, bounded, and the wall clock (the cross-process
        // merge key; the ts_* stamps are monotonic) is taken in the same breath.
        timespec tw{};
        clock_gettime(CLOCK_REALTIME, &tw);
        c->wall_t0 = static_cast<int64_t>(tw.tv_sec) * 1000000000 + tw.tv_nsec;
        const std::string_view head(c->rbuf.data(), c->msg.header_len);
        for (size_t i = 0; i < kSinkCaptureMax; ++i)
        {
            c->sink_cap_len[i] = 0;
            if (i >= _sink_capture_names.size()) continue;
            const std::string_view v = net::http::find_header(head, _sink_capture_names[i]);
            const size_t n = v.size() < kSinkCaptureBytes ? v.size() : kSinkCaptureBytes;
            // Guarded, because an absent header is the common case and find_header
            // returns a null view for it: memcpy's arguments are declared non-null
            // even when the length is 0, so the unguarded form is undefined
            // behaviour.
            if (n) std::memcpy(c->sink_cap[i], v.data(), n);
            c->sink_cap_len[i] = static_cast<uint8_t>(n);
        }
    }

    // The one body read on the byte-forward path, and it happens only with a sink or
    // a policy installed. A stock build parses nothing here, as before.
    // Split out of sink_capture because a policy needs it too and needs it before the
    // decision, while sink_capture's other work is only ever read at the end.
    void Gateway::capture_model(Connection* c) noexcept
    {
        c->sink_model_len = 0;
        const std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
        const std::string_view model = provider::model_of(body);
        if (!model.empty() && model.size() <= sizeof(c->sink_model))
        {
            std::memcpy(c->sink_model, model.data(), model.size());
            c->sink_model_len = static_cast<uint8_t>(model.size());
        }
    }

    void Gateway::sink_emit(Connection* c, int status, bool streamed) noexcept
    {
        RequestRecord r;
        r.seq = c->req_seq;
        r.wall_t0_ns = c->wall_t0;
        r.client_upload_ns = c->client_upload_ns;
        r.client_conn_reused = c->client_conn_reused;
        r.client_conn_setup_ns = c->client_conn_setup_ns;
        r.request_bytes = c->msg.total_len;
        r.client_encoded = c->msg.encoded;
        r.ts_req_recvd = c->ts_req_recvd;
        r.ts_req_built = c->ts_req_built;
        r.ts_wire_ready = c->ts_wire_ready;
        r.ts_up_sent = c->ts_up_sent;
        r.ts_up_recvd = c->ts_up_recvd;
        r.ts_first_token = c->ts_first_token;
        r.ts_first_thinking = c->ts_first_thinking;
        r.max_chunk_gap_ns = c->max_chunk_gap_ns;
        r.quota_exhausted = c->quota_exhausted;
        r.retry_after_s = c->retry_after_s;
        r.ts_done = now_ns();
        r.tag = c->policy_tag;
        r.status = status;
        r.upstream_index = c->upstream_slot;
        r.attempts = c->failover_attempts;
        r.streamed = streamed;
        r.error_reply = !streamed && c->close_after_resp;
        r.truncated = streamed && c->close_after_resp;
        r.translated = c->translate_body;
        r.backend = _active_backend;
        r.model = std::string_view(c->sink_model, c->sink_model_len);
        r.asked_tier = std::string_view(c->asked_tier, c->asked_tier_len);
        r.upstream_error = std::string_view(c->upstream_error, c->upstream_error_len);
        r.served_tier = std::string_view(c->served_tier, c->served_tier_len);
        r.from_pool = c->upstream_pooled;
        if (streamed && c->sse_xlate)
        {
            r.tokens_in = c->sse_xlate->input_tokens();
            r.tokens_out = c->sse_xlate->output_tokens();
            r.cached_tokens = static_cast<int32_t>(c->sse_xlate->cached_tokens());
            r.cache_write_tokens = static_cast<int32_t>(c->sse_xlate->cache_write_tokens());
            r.cache_write_5m_tokens = static_cast<int32_t>(c->sse_xlate->cache_write_5m_tokens());
            r.cache_write_1h_tokens = static_cast<int32_t>(c->sse_xlate->cache_write_1h_tokens());
        }
        else if (streamed)
        {
            // Byte-forwarded: nothing parsed the events, so the counts come from the
            // usage chunk kept in the tail. All three stay -1 when the client did not
            // ask for usage, which is "not reported" and not "zero".
            const BodyUsage u = stream_tokens(c);
            r.tokens_in = static_cast<int32_t>(u.in);
            r.tokens_out = static_cast<int32_t>(u.out);
            r.cached_tokens = static_cast<int32_t>(u.cached);
            r.cache_write_tokens = static_cast<int32_t>(u.cache_write);
            r.cache_write_5m_tokens = static_cast<int32_t>(u.cache_write_5m);
            r.cache_write_1h_tokens = static_cast<int32_t>(u.cache_write_1h);
        }
        else if (!streamed)
        {
            r.tokens_in = static_cast<int32_t>(c->tok_in);
            r.tokens_out = static_cast<int32_t>(c->tok_out);
            r.cached_tokens = static_cast<int32_t>(c->tok_cached);
            r.cache_write_tokens = static_cast<int32_t>(c->tok_cache_write);
            r.cache_write_5m_tokens = static_cast<int32_t>(c->tok_cw_5m);
            r.cache_write_1h_tokens = static_cast<int32_t>(c->tok_cw_1h);
        }
        for (size_t i = 0; i < kSinkCaptureMax; ++i)
            r.captured[i] = std::string_view(c->sink_cap[i], c->sink_cap_len[i]);
        _sink->on_request(r);
        // Consumed. A framing error never reaches the per-request reset (it fails
        // before framing succeeds), so without this its 400's record would carry the
        // Previous request's captures and tag on a keep-alive connection.
        c->sink_cap_len[0] = c->sink_cap_len[1] = 0;
        c->wall_t0 = 0;
        c->policy_tag = 0;
        // Streaming usage, for the same reason and found by the same argument: a
        // keep-alive client's second stream inherited the first one's token counts,
        // because nothing cleared them between requests.
        c->stream_tail.clear();
        c->sse_scratch.clear();
        c->sse_scratch.shrink_to_fit();
        c->usage_in = c->usage_out = c->usage_cached = c->usage_cache_write = -1;
        c->usage_cw_5m = c->usage_cw_1h = -1;
        // The non-streaming counters. They are assigned only where a body is scanned,
        // so a keep-alive request that fails before that (an upstream non-200, a translate failure)
        //  emitted a record carrying the token counts of the request before it.
        c->tok_in = c->tok_out = c->tok_cached = c->tok_cache_write = -1;
        c->tok_cw_5m = c->tok_cw_1h = -1;
        c->ts_first_token = 0;
        // Per request, like the stamps around it: a pooled connection serving the next
        // caller must not report the last one's tier.
        c->tier_override = {};
        c->asked_tier_len = 0;
        c->upstream_error_len = 0;
        c->served_tier_len = 0;
        c->served_tier_tries = 0;
        c->upstream_pooled = false;
        c->ts_first_thinking = 0;
        c->ts_last_chunk = 0;
        c->max_chunk_gap_ns = 0;
        c->quota_exhausted = 0;
        c->retry_after_s = 0;
        // Defensive, not a fixed bug: the only reader of `wants_usage` runs on a
        // translated Anthropic request, and that translation writes the field.
        c->wants_usage = false;
    }

    Decision Gateway::policy_decision(Connection* c, const net::http::Message& m) noexcept
    {
        // Head, plus the model identifier and nothing else from the body. This is the
        // line where "metadata only, no prompt text".
        const RequestFacts facts{std::string_view(c->rbuf.data(), m.header_len), m.body_len,
                                 std::string_view(c->sink_model, c->sink_model_len)};

        Decision d = _policy->decide(facts);
        if (d.allow)
        {
            c->policy_tag = d.tag; // handed back verbatim in FailureFacts
            return d;
        }

        // A status the responder cannot render would emit a misleading reply, so
        // substitute and say so loudly. The refusal still stands: fail closed.
        if (d.deny_status < 400 || d.deny_status > 599)
        {
            LB_WARN(ReqId{c->req_seq}, " policy returned out-of-range status ", d.deny_status,
                    " (", d.reason, "); refusing with 403");
            d.deny_status = 403;
        }
        // Never log `facts`: the head carries the client's Authorization.
        ++_stats.policy_denied;
        return d;
    }

    Retry Gateway::failover_target(Connection* client, int status, const char* why) noexcept
    {
        // Every precondition here is about safety, not policy. Re-sending a request the
        // client has already begun receiving would duplicate output; re-sending one we
        // no longer hold is impossible; and an unbounded chain turns one dead provider
        // into a latency multiplier.
        if (!_policy || _upstreams.size() < 2) return {};
        if (!client || client->doomed) return {};
        // Nothing may have reached the client, or a re-send duplicates output.
        //
        // The `streaming` half is unreachable today and is kept deliberately: all six
        // call sites already divert a streaming client to abort_pair or
        // stream_on_upstream_eof before they get here, so no test can distinguish it
        // from `true`. It stays for the same reason tls_invariant_ok() does, six call
        // sites must each keep being right for it to remain unreachable, and the cost
        // of being wrong is a client receiving one answer twice. The `wbuf` half is
        // reachable: a pipelined earlier response can still be draining.
        if (client->streaming || !client->wbuf.empty()) return {};
        if (client->failover_req.empty()) return {};
        if (client->failover_attempts >= kMaxFailoverAttempts - 1) return {};

        const FailureFacts f{client->upstream_slot, status, why, client->failover_attempts,
                             client->policy_tag};
        const Retry r = _policy->on_failure(f);
        if (!r.retry) return {};
        if (r.upstream_index < 0 || static_cast<size_t>(r.upstream_index) >= _upstreams.size())
        {
            LB_WARN(ReqId{client->req_seq}, " failover to out-of-range upstream ",
                    static_cast<int64_t>(r.upstream_index), "; giving up");
            return {};
        }
        // Sending it back to the venue that just failed is the one answer that cannot
        // help, and it is how a policy accidentally writes an infinite loop.
        if (r.upstream_index == client->upstream_slot)
        {
            LB_WARN(ReqId{client->req_seq}, " failover named the venue that just failed; "
                                            "giving up");
            return {};
        }
        return r;
    }

    void Gateway::ep_forward(Connection* c) noexcept
    {
        // The venue was chosen by the policy (or defaulted to 0) before this ran; it is
        // stamped on the client so the response leg can still find the dialect after
        // the upstream has been released back to its pool.
        const Upstream& up = upstream_of(c);
        // Resolve the translation for the venue this attempt chose, on every forward, so a
        // lands on a venue of a different dialect rebuilds correctly instead of reusing
        // the first venue's mode. The client dialect is read off the request line, which
        // is in rbuf on the initial call and restored there before a failover retry. An
        // unbuilt client/venue pair is refused here, before anything reaches the upstream.
        {
            const TranslationPlan plan = resolve_dialect(c, up);
            if (!plan.ok) { ep_error_respond(c, 400, plan.why); return; }
            c->translate_body = plan.translate;
            c->effective_dialect = plan.venue;
        }
        // Build the bytes to send upstream (translate first, before acquiring an
        // upstream, so a bad body can't leak a pooled connection).
        std::string upstream_bytes;
        if (c->translate_body)
        {
            std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
            // Remember whether the client asked for a final usage chunk. The
            // request bytes are consumed below, but the stream needs it later.
            // `wants_usage` is an output of the translation below, not a separate
            // question: the translator parses this body anyway.
            const std::string_view client_hdrs(c->rbuf.data(), c->msg.header_len);
            const char* why = "";
            // Either failure => 400, and nothing goes upstream.
            if (!build_translated_request(up, c->effective_dialect, body, client_hdrs, _strip_headers,
                                          upstream_bytes, why, c->model_override,
                                          &c->wants_usage))
            {
                if (why[0] == 't')
                    ep_error_respond(c, 400, "translate", translate_failure(body));
                // The caller's own header, so the caller is told. The value is
                // never echoed: it is a credential, and this string lands in a JSON
                // body with no escaping.
                else
                    ep_error_respond(c, 400, "malformed credential",
                                     refuse::kCredential);
                return;
            }
        }
        else
        {
            // No `wants_usage` on this path, deliberately. Its only reader is the
            // Anthropic-to-OpenAI SSE translator, built only when `translate_body`
            // is set.
            // Byte-forward still rebuilds, even with nothing to strip: Host must name
            // the venue, not the client's idea of us.
            // Byte-forward rewrites the model by splicing the value, so every other
            // byte the client sent survives; the derived Content-Length then describes
            // the spliced body without anyone having to remember to update it.
            std::string rewritten;
            if (!c->model_override.empty() || !c->tier_override.empty())
            {
                std::string_view had;
                rewritten = provider::apply_overrides(
                    std::string_view(c->rbuf.data() + c->msg.header_len, c->msg.body_len),
                    c->model_override, c->tier_override, &had);
                if (rewritten.empty())
                { ep_error_respond(c, 400, "cannot apply route overrides"); return; }
                // Copied, not held: `had` points into the request buffer, which is
                // reused before the sink runs. Truncated and not refused, because a
                // tier too long to be one is still worth reporting.
                c->asked_tier_len = static_cast<uint8_t>(
                    had.size() < sizeof c->asked_tier ? had.size() : sizeof c->asked_tier);
                // Guarded, because the common case is a caller that named no tier and
                // an empty string_view's data() is null.
                if (c->asked_tier_len)
                    std::memcpy(c->asked_tier, had.data(), c->asked_tier_len);
            }
            upstream_bytes = request_without(std::string_view(c->rbuf.data(), c->msg.total_len),
                                             c->msg.header_len, _strip_headers, up.host_hdr,
                                             up.base_path, rewritten);
            // Refused, not repaired: a venue with a base path needs an origin-form
            // target to prefix, and nothing has been sent upstream at this point.
            if (upstream_bytes.empty())
            { ep_error_respond(c, 400, "request target not origin-form"); return; }
        }

        Connection* u = ep_acquire_upstream(c->upstream_slot);
        if (!u)
        {
            if (!ep_upstream_failed(c, 502, "no upstream (connect failed)"))
                ep_error_respond(c, 502, "no upstream (connect failed)");
            return;
        }

        LB_DEBUG(ReqId{c->req_seq}, " upstream ", *u, " pooled=", u->from_pool,
                 " dest=", upstream_of(u).ip, ":", upstream_of(u).port);
        u->wbuf = std::move(upstream_bytes);
        u->woff = 0;
        // Keep the original bytes when a failover could use them: the rebuilt request
        // above was translated for this venue's dialect, so it cannot be resent to a
        // different one. One copy per in-flight request, and only where it can pay off.
        if (_policy && _upstreams.size() > 1 && c->failover_req.empty())
        {
            const size_t extra = c->rbuf.size() - c->msg.total_len;
            c->failover_req.swap(c->rbuf);
            c->rbuf.assign(c->failover_req, c->msg.total_len, extra);
            c->failover_req.resize(c->msg.total_len);
        }
        else c->rbuf.erase(0, c->msg.total_len);
        c->peer = u;
        u->peer = c;
        c->ever_framed = true;        // past the setup deadline for good
        c->ts_client_activity = now_ns(); // and the idle clock restarts here
        c->ts_req_built = now_ns();   // end of our request-side work
        c->ts_up_activity = c->ts_req_built; // idle-timeout baseline for this request
        if (u->connected) c->ts_wire_ready = c->ts_req_built; // pooled: no handshake
        c->upstream_pooled = u->connected;

        // Optimistic send: if the pooled upstream is already connected (the common
        // case), write immediately and only arm EPOLLOUT if the socket buffer is
        // full. Arming unconditionally costs two epoll_ctl calls + an extra wakeup
        // per request; the client-response leg already writes this way.
        if (u->connected)
        {
#ifdef LLMBRIDGE_HAVE_TLS
            if (u->tls)
            {
                // Pooled TLS conns are always past the handshake (a fresh conn is
                // never `connected` here). Push the plaintext through the session
                // and flush the ciphertext; partial flushes finish on writability.
                tls_push_wbuf(u);
                bool done = false;
                if (!ep_tls_flush(u, &done))
                {
                    if (!ep_retry_upstream(u) && !ep_upstream_failed(c, 502, "upstream write failed")) ep_error_respond(c, 502, "upstream write failed, retry exhausted");
                    return;
                }
                if (done && tls_wbuf_flushed(u)) c->ts_up_sent = now_ns();
                return;
            }
#endif
            bool done = false;
            if (!ep_pump_write(u, &done)) { if (!ep_retry_upstream(u) && !ep_upstream_failed(c, 502, "upstream write failed")) ep_error_respond(c, 502, "upstream write failed, retry exhausted"); return; }
            if (done) c->ts_up_sent = now_ns(); // request fully sent (end of request path)
            else ep_arm_write(u);               // socket full; finish on writability
        }
        else
        {
            ep_arm_write(u); // connect pending; ep_on_upstream_writable sends once it completes
        }
    }

    void Gateway::ep_on_upstream_writable(Connection* u) noexcept
    {
        if (!u->connected)
        {
            int err = net::connect_result(u->fd);
            if (err != 0)
            {
                Connection* client = u->peer;
                u->peer = nullptr;
                ep_close_upstream(u);
                if (client) { client->peer = nullptr; if (!ep_upstream_failed(client, 502, "upstream connect refused")) ep_error_respond(client, 502, "upstream connect refused"); }
                else ++_stats.errors;
                return;
            }
            u->connected = true;
            // t2 for a plaintext upstream: the socket can carry the request now.
            // A TLS upstream is not wire-ready yet; tls_feed() stamps t2 when the
            // handshake completes.
            if (!upstream_is_tls(u) && u->peer && u->peer->ts_wire_ready == 0)
                u->peer->ts_wire_ready = now_ns();
#ifdef LLMBRIDGE_HAVE_TLS
            if (u->tls && !u->tls->handshake_done())
                u->tls->start_handshake(); // ClientHello lands in the write BIO
#endif
        }
#ifdef LLMBRIDGE_HAVE_TLS
        if (u->tls)
        {
            const bool was_flushed = u->peer && tls_wbuf_flushed(u);
            bool tdone = false;
            if (!ep_tls_flush(u, &tdone))
            {
                if (u->peer) { if (!ep_retry_upstream(u) && !ep_upstream_failed(u->peer, 502, "upstream write failed")) ep_error_respond(u->peer, 502, "upstream write failed, retry exhausted"); }
                else ep_close_upstream(u);
                return;
            }
            if (!tdone) return; // EPOLLOUT re-armed by ep_tls_flush
            ep_disarm_write(u); // ciphertext drained; handshake replies arrive via read
            if (u->peer && !was_flushed && tls_wbuf_flushed(u))
                u->peer->ts_up_sent = now_ns();
            return;
        }
#endif
        bool done = false;
        if (!ep_pump_write(u, &done))
        {
            if (u->peer) { if (!ep_retry_upstream(u) && !ep_upstream_failed(u->peer, 502, "upstream write failed")) ep_error_respond(u->peer, 502, "upstream write failed, retry exhausted"); }
            else ep_close_upstream(u);
            return;
        }
        if (!done) { ep_arm_write(u); return; }
        ep_disarm_write(u);
        if (u->peer) u->peer->ts_up_sent = now_ns(); // end of request-path work
    }

    void Gateway::ep_on_upstream_readable(Connection* u) noexcept
    {
        Connection* client = u->peer;
        const bool read_ok =
#ifdef LLMBRIDGE_HAVE_TLS
            u->tls ? ep_tls_drain_read(u) :
#endif
                     ep_drain_read(u);
        if (!read_ok)
        {
#ifdef LLMBRIDGE_HAVE_TLS
            // TLS parity with the io_uring path: a fatal session error (bad record,
            // MAC failure) mid-stream must abort the client, never finalize the
            // stream as clean: a corrupted stream that ends in a well-formed
            // [DONE] would hide the corruption from the client entirely. Only a
            // real transport EOF may end a close-delimited stream normally.
            if (u->tls && u->tls->want() == net::tls::Want::Error && client && client->streaming)
            {
                ep_abort_pair(client);
                return;
            }
#endif
            if (client && client->streaming) { ep_stream_on_upstream_eof(u); return; }
            if (client == nullptr) ep_close_upstream(u); // idle pooled conn dropped (eviction)
            else if (!ep_retry_upstream(u) && !ep_upstream_failed(client, 502, "upstream EOF")) ep_error_respond(client, 502, "upstream EOF, retry exhausted");
            return;
        }
        // Stray bytes on an idle pooled upstream. They are the tail of an exchange
        // that is over, and ep_acquire_upstream does not clear rbuf, so leaving them
        // hands one client's bytes to the next as the head of its response. The
        // io_uring twin already closes here; this side kept the connection.
        if (client == nullptr) { ep_close_upstream(u); return; }
        client->ts_up_activity = now_ns(); // upstream made progress

        // Mid-stream: pump the newly-arrived body bytes and return.
        if (client->streaming) { ep_stream_pump(u); return; }

        // First response bytes: for the Anthropic translate path, peek the head to
        // decide whole-body vs streaming (text/event-stream). Other modes and
        // non-streaming responses fall through to the whole-body path unchanged.
        // Anthropic (translated) or none (byte-forward). Gemini and Cohere are
        // non-streaming here, so a stream from one of those would be forwarded in a
        // dialect the client cannot read; they keep falling through to the whole-body
        // path until their translators exist.
        if (client->effective_dialect == UpstreamDialect::Anthropic ||
            client->effective_dialect == UpstreamDialect::Azure ||
            !client->translate_body)
        {
            net::http::ResponseHead h;
            const auto hs = net::http::parse_response_head(u->rbuf, h);
            if (hs == net::http::FrameStatus::NeedMore) return;
            if (hs == net::http::FrameStatus::Error) { ep_error_respond(client, 502, "upstream response head framing"); return; }
            // Only a 200 carries a real event stream. A provider error (429 rate
            // limit, 529 overloaded, 400 context length, 401 auth) must reach the
            // client with its status (relayed below once the body is framed)
            // never laundered into a 200 stream.
            if (h.event_stream && h.status == 200)
            {
                // t4 for a stream: the provider's response head is now complete.
                // Not the first token, and not the first data chunk. A provider
                // May send 200 + content-type as soon as it accepts the stream,
                // well before its first generated token. Anthropic does not: measured
                // 2026-08-06, its head trails its first token by ~1 ms, so for that
                // provider this tracks TTFT closely. That is a per-provider fact, not
                // a protocol guarantee; see LATENCY.md §3 before assuming it holds.
                // The non-streaming path stamps this after framing, which this branch
                // returns before reaching, so stamp it here or it stays 0 and the
                // TTFB timing header reports garbage.
                client->ts_up_recvd = now_ns();
                note_quota(client, h);
                ep_begin_stream(u, h);
                return;
            }
        }

        // Stamp just before framing so the response HTTP parse is counted in the
        // response-path overhead, without charging inter-packet network wait.
        const int64_t t0 = now_ns();
        // parse_response, not parse: real providers return non-streaming bodies
        // chunked over HTTP/1.1, which parse_request() rejects by design (see http.hpp).
        // `r.body` aliases rbuf (Content-Length) or _resp_scratch (chunked); it is
        // dead before rbuf is erased or the upstream released, below.
        const auto r = net::http::parse_response(u->rbuf, u->rdec);
        if (r.failed()) { ep_error_respond(client, 502, "upstream response framing"); return; }
        if (!r.complete()) return;
        const net::http::ResponseHead& h = r.head;
        const std::string_view body_buf = r.body;
        const size_t total_len = r.total_len;

        client->ts_up_recvd = t0; // end of upstream wait (stamped pre-framing)
        note_quota(client, h);
        note_upstream_error(client, h, body_buf);
        note_served_tier(client, body_buf, /*tail=*/true);

        if (client->translate_body)
        {
            const std::string_view body = body_buf;
            // Relay a provider failure with its own status + message (rate limit,
            // overloaded GPU, context length, auth); translating a non-200 body as
            // if it were a completion would fail and mask it as a generic 502.
            if (h.status != 0 && h.status != 200)
            {
                client->wbuf = build_http_status(
                    h.status, reason_for(h.status),
                    provider::upstream_error_to_openai(body, "upstream_error"));
                client->woff = 0;
                client->peer = nullptr;
                if (h.keep_alive) ep_release_upstream(u); else ep_close_upstream(u);
                ++_stats.errors;
                ep_respond(client);
                return;
            }
            std::string tbody = xlate_resp(client->effective_dialect, body);
            // Scanned unconditionally, not only when --timing-headers is on: the
            // response header is one surface for these numbers and the log is
            // another, and a number that appears in one but not the other is the
            // kind of inconsistency this file has been bitten by before.
            {
                const BodyUsage bu = scan_usage(tbody);
                client->tok_in = bu.in;
                client->tok_out = bu.out;
                client->tok_cached = bu.cached;
                // The cache-creation counts are read from the provider's own body, never the
                // translated one. OpenAI defines no field for them.
                const BodyUsage up = scan_usage(body);
                client->tok_cache_write = up.cache_write;
                client->tok_cw_5m = up.cache_write_5m;
                client->tok_cw_1h = up.cache_write_1h;
            }
            if (tbody.empty())
            {
                client->peer = nullptr;
                ep_release_upstream(u); // framing was valid; the upstream conn is reusable
                ep_error_respond(client, 502, "response translate");
                return;
            }
            std::string timing;
            if (_timing_headers)
            {
                // t5: the response is built here and written immediately after.
                const int64_t ts_resp_built = now_ns();
                const TimingSplit sp = timing_split(
                    client->ts_req_recvd, client->ts_req_built, client->ts_wire_ready,
                    client->ts_up_sent, client->ts_up_recvd, ts_resp_built);
                append_timing_headers(timing, client->ts_req_recvd, sp.compute_ns / 1000,
                                      sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                      sp.upstream_ns / 1000, "x-llmbridge-upstream-us",
                                      client->req_seq, client->client_upload_ns / 1000);
                append_usage_headers(timing, tbody);
            }
            client->wbuf = build_http("HTTP/1.1 200 OK", tbody, timing);
        }
        else
        {
            // Passthrough: forward the upstream's own bytes. A chunked response is
            // re-framed with Content-Length instead of relayed verbatim; we have
            // already decoded it, and handing the client a chunked body we did not
            // re-verify would push our framing problem downstream.
            // The upstream's own status, not a hardcoded 200. Re-framing a chunked
            // response used to relabel every one of them as success, so a provider's
            // 429 reached the caller as a completion whose body happened to be a
            // rate-limit error, with Retry-After dropped. The client cannot back off
            // from a 200. Content-Length responses were relayed verbatim and were
            // never affected, which is why it survived: only chunkedness triggers it.
            if (h.chunked)
                client->wbuf = build_http_status(h.status ? h.status : 200,
                                                 reason_for(h.status ? h.status : 200),
                                                 body_buf);
            else client->wbuf.assign(u->rbuf.data(), total_len);
            // The counts, from the venue's own body. Only the translated branch above
            // scanned, so a byte-forward reported nothing: a sink saw -1 and a tape
            // recorded a request that cost zero tokens at a real price.
            const BodyUsage bu = scan_usage(body_buf);
            client->tok_in = bu.in;
            client->tok_out = bu.out;
            client->tok_cached = bu.cached;
            client->tok_cache_write = bu.cache_write;
            client->tok_cw_5m = bu.cache_write_5m;
            client->tok_cw_1h = bu.cache_write_1h;
        }
        client->woff = 0;

        // Response fully read -> upstream is free. Pool it only if it will stay
        // open (response keep-alive, and for passthrough the client didn't ask to
        // close); otherwise it's about to close, so drop it instead of reuse a
        // stale connection.
        const bool pool_upstream =
            h.keep_alive && (client->translate_body || client->msg.keep_alive);
        client->peer = nullptr;
        // Drop the framed message so a pipelined next response is not mis-read as
        // part of this one; anything left is the start of the next message.
        u->rbuf.erase(0, total_len);
        u->rdec.reset(); // next response on this conn decodes from a clean state
        if (pool_upstream) ep_release_upstream(u);
        else ep_close_upstream(u);
        ep_respond(client);
    }

    void Gateway::ep_respond(Connection* c) noexcept
    {
        bool done = false;
        if (!ep_pump_write(c, &done)) { ep_close_client(c); ++_stats.errors; return; }
        if (!done) { ep_arm_write(c); return; } // socket full; finish on writability
        ep_finish_client(c);
    }

    void Gateway::ep_on_client_writable(Connection* c) noexcept
    {
        if (c->streaming) { ep_stream_flush(c); return; } // pump path has its own drain logic
        bool done = false;
        if (!ep_pump_write(c, &done)) { ep_abort_pair(c); return; }
        if (!done) { ep_arm_write(c); return; }
        ep_finish_client(c);
    }

    /// Answer `Expect: 100-continue` without entering the response path.
    void Gateway::send_interim_continue(Connection* c, bool uring) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            if (!c->tls->handshake_done()) return;
            const auto* p = reinterpret_cast<const uint8_t*>(kContinue.data());
            if (c->tls->write_plaintext({p, kContinue.size()}) != kContinue.size()) return;
#ifdef LLMBRIDGE_HAVE_URING
            if (uring) { ur_tls_flush(c); return; } // completion sees an empty wbuf: nothing finishes
#endif
            (void)uring;
            bool done = false;
            if (!ep_tls_flush(c, &done)) return;
            if (!done) c->client_interim_inflight = true; // the writable event drains it
            return;
        }
#else
        (void)uring;
#endif
        const ssize_t n = ::send(c->fd, kContinue.data(), kContinue.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0 && static_cast<size_t>(n) < kContinue.size())
        {
            // A torn interim line is a protocol error the client cannot recover from.
            c->doomed = true;
        }
    }

    void Gateway::ep_finish_client(Connection* c) noexcept
    {
        if (c->client_interim_inflight)
        {
            c->client_interim_inflight = false; // the 100 Continue left; nothing finished
            return;
        }
        // Error replies (close_after_resp) are counted in _stats.errors, not the
        // latency histograms; their timing stamps are unset and would be garbage.
        if (!c->close_after_resp)
        {
            const int64_t ts_resp_sent = now_ns();
            if (ts_resp_sent - _t_start >= _warmup_ns)
            {
                // req_path is our request-side work only; the wait for connect +
                // TLS is its own histogram so it cannot inflate the added-latency
                // claim. overhead = req_path + resp_path, connect excluded.
                //
                // Derived from the same timing_split() the headers use, so
                // connect(TLS) here and x-llmbridge-connect-us there cannot come to
                // mean different things. t5 does not enter this grouping, so t4 is
                // passed in its place.
                const TimingSplit sp = timing_split(c->ts_req_recvd, c->ts_req_built,
                                                    c->ts_wire_ready, c->ts_up_sent,
                                                    c->ts_up_recvd, c->ts_up_recvd);
                const int64_t conn_ns = sp.connect_ns;
                const int64_t req_ns = sp.req_path_ns;
                const int64_t resp_ns = ts_resp_sent - c->ts_up_recvd;
                if (req_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(req_ns));
                if (conn_ns >= 0) _stats.connect.record(static_cast<uint64_t>(conn_ns));
                if (resp_ns >= 0) _stats.resp_path.record(static_cast<uint64_t>(resp_ns));
                if (req_ns >= 0 && resp_ns >= 0)
                    _stats.overhead.record(static_cast<uint64_t>(req_ns + resp_ns));
                ++_stats.requests;
            }
        }
        ep_disarm_write(c);
        if (_sink) sink_emit(c, status_of(c->wbuf), /*streamed=*/false);
        LB_DEBUG(ReqId{c->req_seq}, " status=", status_of(c->wbuf), " bytes=", c->wbuf.size(),
                 " keep_alive=", c->msg.keep_alive, " on ", *c);
        c->wbuf.clear(); // response fully sent; ep_pump_write no longer clears it for us
        c->woff = 0;
        const bool close_now = c->close_after_resp || !c->msg.keep_alive;
        c->msg = net::http::Message{};
        if (close_now) { ep_close_client(c); return; }
        ep_on_client_readable(c); // service any pipelined next request already in rbuf
    }

    // ── Streaming pump (epoll, Anthropic->OpenAI SSE) ───────────────────────
    // Enter streaming: the upstream response is text/event-stream. Send the
    // client SSE headers (close-delimited) and translate the body as it arrives.
    void Gateway::ep_begin_stream(Connection* u, const net::http::ResponseHead& h) noexcept
    {
        Connection* client = u->peer;
        client->streaming = true;
        client->stream_chunked = h.chunked;
        client->stream_keep_alive = h.keep_alive; // decides poolability at stream end
        stream_warn_if_encoded(client, h);
        // Only a request that needs translating gets a translator.
        if (client->translate_body && client->effective_dialect == UpstreamDialect::Anthropic)
            client->sse_xlate = std::make_unique<provider::AnthropicToOpenAiSse>(-1, client->wants_usage);
        // Chosen once, before either head is built, and remembered.
        client->stream_chunked_out = stream_reusable_out(client);
        if (_timing_headers)
        {
            // t4 = provider's first response byte, stamped by the caller.
            std::string timing;
            // t5 = t4: a stream's response is not built at one instant, so the
            // compute leg is the request side alone instead of an invented figure.
            const TimingSplit sp = timing_split(
                client->ts_req_recvd, client->ts_req_built, client->ts_wire_ready,
                client->ts_up_sent, client->ts_up_recvd, client->ts_up_recvd);
            append_timing_headers(timing, client->ts_req_recvd, sp.compute_ns / 1000,
                                  sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                  sp.upstream_ns / 1000, "x-llmbridge-upstream-ttfb-us",
                                  client->req_seq, client->client_upload_ns / 1000);
            client->wbuf.assign(sse_head_with_timing(
                timing, client->stream_chunked_out ? kSseHeadChunked : kSseHead));
        }
        else
            client->wbuf.assign(client->stream_chunked_out ? kSseHeadChunked : kSseHead);
        client->woff = 0;

        u->rbuf.erase(0, h.header_len); // consume the head; the rest is body
        ep_stream_pump(u);                 // translate any initial body + flush headers
    }

    // Decode + translate the upstream body bytes now sitting in u->rbuf, appending
    // OpenAI SSE to the client's write buffer, then flush.
    void Gateway::ep_stream_pump(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { ep_close_upstream(u); return; } // lost peer mid-stream

        const StreamStep st = stream_step(client, u->rbuf, client->wbuf, /*at_eof=*/false);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            // Truncate honestly: flush what we already translated, then close
            // Without a terminal [DONE] so the client sees an aborted stream
            // instead of a fabricated clean finish.
            stream_truncate(client);
            ep_stream_flush(client);
            return;
        }
        ep_stream_flush(client);
    }

    // Upstream closed the connection: translate whatever remains, emit the terminal
    // [DONE] if we haven't, and finalize.
    void Gateway::ep_stream_on_upstream_eof(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { ep_close_upstream(u); return; }

        const StreamStep st = stream_step(client, u->rbuf, client->wbuf, /*at_eof=*/true);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            stream_truncate(client);
        }
        ep_stream_flush(client);
    }

    // Write buffered SSE to the client. If it doesn't all go, finish on writability
    // and pause upstream reads (backpressure). On full flush, resume upstream, or
    // finalize if the stream has ended.
    void Gateway::ep_stream_flush(Connection* client) noexcept
    {
        bool done = false;
        if (!ep_pump_write(client, &done)) { ep_abort_pair(client); return; } // client gone
        if (!done)
        {
            ep_arm_write(client);
            if (client->peer && !client->peer->doomed) ep_pause_read(client->peer);
            return;
        }
        client->wbuf.clear();
        client->woff = 0;
        ep_disarm_write(client);
        if (client->stream_ended) { ep_finalize_stream(client); return; }
        if (client->peer && !client->peer->doomed) ep_resume_read(client->peer);
    }

    /// Can reuse only if we framed the reply so the body has an end marker, the
    /// stream reached that marker, and the caller wanted the connection kept.
    [[nodiscard]] bool stream_client_reusable(const Connection* c) noexcept
    {
        return c->stream_chunked_out && !c->close_after_resp && c->msg.keep_alive;
    }

    /// Clear the per-stream state so the next request on this connection starts clean.
    void stream_reset_for_next(Connection* c) noexcept
    {
        c->streaming = false;
        c->stream_ended = false;
        c->stream_chunked = false;
        c->stream_chunked_out = false;
        c->stream_keep_alive = false;
        c->wants_usage = false;
        c->sse_xlate.reset();
        c->chunkdec = net::http::ChunkDecoder{};
        c->stream_tail.clear();
        c->sse_scratch.clear();
        // The counts the stream produced.
        c->usage_in = c->usage_out = c->usage_cached = c->usage_cache_write = -1;
        c->usage_cw_5m = c->usage_cw_1h = -1;
        c->ts_first_token = 0;
        c->ts_first_thinking = 0;
        c->ts_last_chunk = 0;
        c->max_chunk_gap_ns = 0;
        c->served_tier_len = 0;
        c->served_tier_tries = 0;
    }

    void Gateway::ep_finalize_stream(Connection* client) noexcept
    {
        if (Connection* u = client->peer)
        {
            const bool reusable = stream_upstream_reusable(client, u);
            client->peer = nullptr;
            u->peer = nullptr;
            // Reuse is the whole point: a streaming request otherwise costs a fresh
            // upstream connect every time, which measured as the dominant term in
            // time-to-first-token. Pool it when the framing says that is safe.
            if (reusable) ep_release_upstream(u);
            else ep_close_upstream(u);
        }
        // A stream never reaches the non-streaming completion log, so without this a
        // streamed request logged its start and then nothing at all: no outcome, no
        // size, no tokens. `close_after_resp` here means the stream was truncated
        // (stream_truncate emits no [DONE] on purpose), which is the one outcome a
        // client cannot distinguish from a clean finish by itself.
        LB_DEBUG(ReqId{client->req_seq}, " stream ended ",
                 client->close_after_resp ? "TRUNCATED" : "clean",
                 " tokens_in=", stream_tokens(client).in,
                 " tokens_out=", stream_tokens(client).out,
                 " on ", *client);
        // Only a stream that terminated cleanly counts as a served request; an
        // aborted one (close_after_resp) was already counted in _stats.errors.
        if (!client->close_after_resp)
        {
            ++_stats.requests;
            stream_record_latency(client);
        }
        if (_sink) sink_emit(client, 200, /*streamed=*/true);

        // Keep the connection when the framing gave the body an end marker.
        if (!stream_client_reusable(client)) { ep_close_client(client); return; }
        stream_reset_for_next(client);
        client->wbuf.clear();
        client->woff = 0;
        client->msg = net::http::Message{};
        ep_on_client_readable(client); // a pipelined next request is already in rbuf
    }

    // Abort requests whose upstream has gone silent. Runs on the loop's existing
    // periodic tick, so an idle gateway costs one cheap scan per tick. A client
    // that hasn't been answered yet gets a real 504; a live stream (headers already
    // sent) is closed without a terminal [DONE], so the client sees a truncated
    // stream instead of a fabricated clean finish.
    void Gateway::print_profile(std::FILE* out, const char* title) const noexcept
    {
        std::ostringstream os;
        os << "\n=== llmbridge " << title << " ===\n";
        if (_stats.overhead.total() > 0) _stats.overhead.print(os, "added-total   ");
        if (_stats.req_path.total() > 0) _stats.req_path.print(os, "req-path      ");
        if (_stats.resp_path.total() > 0) _stats.resp_path.print(os, "resp-path     ");
        if (_stats.connect.total() > 0) _stats.connect.print(os, "connect(TLS)  ");
        if (_stats.accept_tls.total() > 0) _stats.accept_tls.print(os, "accept(TLS)   ");
        if (_stats.first_token.total() > 0) _stats.first_token.print(os, "first-token   ");
        os << "requests=" << _stats.requests << " errors=" << _stats.errors
           << " upstream_conns_opened=" << _stats.upstream_conns_opened
           << " upstream_reused=" << _stats.upstream_reused << "\n";
        os << "client_setup_timeouts=" << _stats.client_setup_timeouts
           << " client_idle_timeouts=" << _stats.client_idle_timeouts
           << " tls_handshake_failures=" << _stats.client_tls_handshake_failures
           << " upstream_timeouts=" << _stats.upstream_timeouts << "\n";
        std::fputs(os.str().c_str(), out);
    }

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
                if (!c->doomed && (c->peer != nullptr || c->streaming)) ++in_flight;
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
                if (c->peer != nullptr || c->streaming) continue; // in flight
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

        // The in-flight abort below is gated on the upstream idle timeout; pool
        // eviction above is not; they are independent settings.
        if (_upstream_idle_ns <= 0) return;

        // Collect first: the teardown below erases from _clients.
        std::vector<Connection*> stale;
        for (auto& [id, c] : _clients)
        {
            if (c->doomed) continue;
            const bool in_flight = c->peer != nullptr || c->streaming;
            if (!in_flight || c->ts_up_activity == 0) continue;
            if (now - c->ts_up_activity > _upstream_idle_ns) stale.push_back(c);
        }
        for (Connection* c : stale)
        {
            ++_stats.upstream_timeouts;
            LB_WARN(ReqId{c->req_seq}, " TIMEOUT upstream silent ", *c,
                    " after_ns=", now - c->ts_up_activity, " limit_ns=", _upstream_idle_ns,
                    " streaming=", c->streaming);
            const bool streaming = c->streaming;
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

    int Gateway::run()
    {
#ifdef LLMBRIDGE_HAVE_URING
        if (_uring_active) return run_uring();
#endif
        return run_epoll();
    }

    int Gateway::run_epoll()
    {
        _t_start = now_ns();
        _active_backend = 1; // also the io_uring fallback path
        epoll_event events[kEpMaxEvents];

        while (!_stop)
        {
            // kPollTickMs timeout so request_stop() is observed within a tick.
            int n = ::epoll_wait(_epfd, events, kEpMaxEvents, kPollTickMs);
            if (n < 0) { if (errno == EINTR) continue; break; }
            for (int i = 0; i < n; ++i)
            {
                Connection* c = static_cast<Connection*>(events[i].data.ptr);
                if (c == _listen_conn) { ep_on_accept(); continue; }
                if (c->doomed) continue; // freed earlier this batch
                // Unlike kqueue, epoll coalesces a fd's readiness into one entry,
                // so a single event can carry both EPOLLIN and EPOLLOUT. Error/
                // hangup conditions fold into the readable path (ep_drain_read then
                // reports the EOF/error). Re-check doomed between the two halves.
                const uint32_t e = events[i].events;
                const bool readable = e & (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP);
                const bool writable = e & EPOLLOUT;
                if (c->is_client)
                {
                    if (readable) ep_on_client_readable(c);
                    if (writable && !c->doomed) ep_on_client_writable(c);
                }
                else
                {
                    if (writable) ep_on_upstream_writable(c);
                    if (readable && !c->doomed) ep_on_upstream_readable(c);
                }
            }
            sweep_idle(/*uring=*/false); // abort requests whose upstream went silent
            for (Connection* d : _doomed) delete d;
            _doomed.clear();
        }
        return 0;
    }

#ifdef LLMBRIDGE_HAVE_URING
    // ════════════════════════════════════════════════════════════════════════
    // io_uring backend (Phase 1): a completion-driven mirror of the epoll loop.
    // Each request advances a small per-connection state machine: every CQE says
    // "this op finished with N bytes," we act, and submit the next op. A conn is
    // freed only when its `inflight` SQEs have all completed (no use-after-free on
    // a completion that lands after we close).
    // ════════════════════════════════════════════════════════════════════════
    namespace
    {
        enum UOp : uint64_t { UAccept = 0, URecv = 1, USend = 2, UConnect = 3, UTimer = 4, UCancel = 5 };
        constexpr uint64_t kUrTagMask = 7;
        inline uint64_t make_ud(Connection* c, UOp op) { return reinterpret_cast<uintptr_t>(c) | op; }
        inline Connection* ud_conn(uint64_t d) { return reinterpret_cast<Connection*>(d & ~kUrTagMask); }
        inline UOp ud_op(uint64_t d) { return static_cast<UOp>(d & kUrTagMask); }

        constexpr unsigned kUrRingDepth = 4096;
        // Provided-buffer pool for multishot recv: plenty so a connection always has
        // a buffer to land in (we recycle each immediately after copying it out).
        constexpr unsigned kUrBufGroup = 1;
        constexpr unsigned kUrBufCount = 4096; // power of two
        constexpr unsigned kUrBufSize = 4096;

        // Cap on buffered SSE output for one stream. SSE is model-rate-limited, so
        // this only trips for a pathologically slow client, instead of buffer
        // without bound we drop that stream.
        //
        // It is the only bound this backend has: the multishot recv stays armed for the
        // upstream's life and there is no uring pause path, so a stalled client throttles
        // nothing. Measured against a 32 MiB flood to a client that never reads: peak RSS
        // 67 MB with the cap, 99 MB without, i.e. the whole flood staged. epoll needs no
        // equivalent because a partial client write pauses its upstream read.
        constexpr size_t kUrStreamBufCap = 8 << 20; // 8 MiB
    } // namespace

    bool Gateway::ur_next_sqe(io_uring_sqe** out) noexcept
    {
        io_uring_sqe* s = _ring.get_sqe();
        if (!s) { _ring.submit(); s = _ring.get_sqe(); } // SQ full: flush, retry once
        *out = s;
        return s != nullptr;
    }

    void Gateway::ur_submit_accept() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return;
        s->opcode = IORING_OP_ACCEPT;
        s->fd = _listen_fd;
        s->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
        s->ioprio = IORING_ACCEPT_MULTISHOT; // one SQE, a completion per accepted fd
        s->user_data = make_ud(_listen_conn, UAccept);
    }

    void Gateway::ur_submit_timer() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return;
        s->opcode = IORING_OP_TIMEOUT;
        s->addr = reinterpret_cast<uint64_t>(&_uring_ts);
        s->len = 1;
        s->user_data = UTimer; // conn = nullptr
    }

    bool Gateway::ur_arm_recv(Connection* c) noexcept
    {
        // Multishot recv drawing from the provided-buffer pool: one submission keeps
        // delivering a completion per data arrival (each naming the buffer it used),
        // so we never re-submit a recv per read. Armed once per connection; only
        // re-armed if the kernel ends the multishot (e.g. pool exhaustion).
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) { ur_close(c); return false; }
        s->opcode = IORING_OP_RECV;
        s->fd = c->fd;
        s->addr = 0;
        s->len = 0;
        s->flags |= IOSQE_BUFFER_SELECT;
        s->buf_group = kUrBufGroup;
        s->ioprio |= IORING_RECV_MULTISHOT;
        s->user_data = make_ud(c, URecv);
        ++c->inflight;
        ++_uring_inflight;
        return true;
    }

    bool Gateway::ur_submit_send(Connection* c) noexcept
    {
        io_uring_sqe* s = nullptr;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_invariant_ok(c)) { ur_close(c); return false; } // never plaintext
#endif
        if (!ur_next_sqe(&s)) { ur_close(c); return false; }
        s->opcode = IORING_OP_SEND;
        s->fd = c->fd;
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            // TLS on either leg: the wire sees ciphertext. wbuf (plaintext) is fed
            // to the Session elsewhere; woff tracks that, not this send.
            s->addr = reinterpret_cast<uint64_t>(c->tls_out.data() + c->tls_out_off);
            s->len = static_cast<unsigned>(c->tls_out.size() - c->tls_out_off);
        }
        else
#endif
        {
            s->addr = reinterpret_cast<uint64_t>(c->wbuf.data() + c->woff);
            s->len = static_cast<unsigned>(c->wbuf.size() - c->woff);
        }
        s->user_data = make_ud(c, USend);
        // The only place send_inflight is set. It means exactly "an SQE referencing
        // this connection's send buffer is outstanding", and an SQE is submitted
        // only here, so this is the only line that can truthfully assert it.
        // Callers must not set it: two of them used to, under two different rules,
        // and one calling into the other silently deadlocked a stream.
        c->send_inflight = true;
        ++c->inflight;
        ++_uring_inflight;
        return true;
    }

    bool Gateway::ur_submit_connect(Connection* u) noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) { ur_abort_pair(u->peer); return false; }
        s->opcode = IORING_OP_CONNECT;
        s->fd = u->fd;
        const sockaddr_in& dst = _upstream_addrs[static_cast<size_t>(
            u->upstream_slot >= 0 ? u->upstream_slot : 0)];
        s->addr = reinterpret_cast<uint64_t>(&dst);
        s->off = sizeof(dst); // connect addrlen rides in `off`
        s->user_data = make_ud(u, UConnect);
        ++u->inflight;
        ++_uring_inflight;
        return true;
    }

    void Gateway::ur_submit_cancel(int fd) noexcept
    {
        // Cancel every in-flight op on `fd` (notably the armed multishot recv, which
        // shutdown() does not terminate). The cancelled ops complete with -ECANCELED,
        // releasing their inflight slots so the conn can be freed / the drain finishes.
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return;
        s->opcode = IORING_OP_ASYNC_CANCEL;
        s->fd = fd;
        s->cancel_flags = IORING_ASYNC_CANCEL_FD;
        s->user_data = UCancel; // sentinel: not inflight-counted, completion ignored
    }

#ifdef LLMBRIDGE_HAVE_TLS
    void Gateway::ur_tls_flush(Connection* u) noexcept
    {
        // Serialized sends, same discipline as the client streaming pump: a send
        // SQE points into tls_out, so tls_out must be immutable while one is in
        // flight; appending could reallocate it under the kernel. Ciphertext
        // produced meanwhile stages inside the Session's write BIO; we pump it
        // out here once the previous send has fully completed.
        if (u->send_inflight) return;
        if (u->tls_out_off >= u->tls_out.size())
        {
            u->tls_out.clear();
            u->tls_out_off = 0;
            tls_pump_out(u);
        }
        if (u->tls_out.empty()) return;
        ur_submit_send(u); // sets send_inflight; see the note there
    }
#endif

    // Send wbuf to a client connection, whatever the transport.
    //
    // The plaintext path submits a send straight out of wbuf. A TLS conn cannot:
    // the SQE points at tls_out, so the plaintext has to go through the Session
    // first or the kernel is handed a zero-length send. That is exactly the bug
    // this helper exists to make unrepeatable. It was found by running curl
    // against both backends, and never by reading the code.
    void Gateway::ur_client_send(Connection* c) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            if (c->tls->handshake_done()) tls_push_wbuf(c);
            ur_tls_flush(c);
            return;
        }
#endif
        ur_submit_send(c);
    }

    bool Gateway::ur_upstream_failed(Connection* client, int status, const char* why) noexcept
    {
        const Retry r = failover_target(client, status, why);
        if (!r.retry) return false;

        LB_WARN(ReqId{client->req_seq}, " venue ", static_cast<int64_t>(client->upstream_slot),
                " failed (", why, "); retrying on ", static_cast<int64_t>(r.upstream_index));
        if (Connection* u = client->peer) { client->peer = nullptr; u->peer = nullptr; ur_close(u); }
        ++client->failover_attempts;
        client->upstream_slot = r.upstream_index;
        client->rbuf.insert(0, client->failover_req); // see the epoll mirror
        ++_stats.upstream_failovers;
        ur_forward(client);
        return true;
    }

    Connection* Gateway::ur_acquire_upstream(int slot) noexcept
    {
        // io_uring connects through the pre-resolved _upstream_addrs, so the record
        // itself is read only to decide TLS: unused in a build without it.
        [[maybe_unused]] const Upstream& up = _upstreams[static_cast<size_t>(slot)];
        auto& pool = _idle_upstreams[static_cast<size_t>(slot)];
        // Only this venue's pool: a connection to one provider cannot serve a request
        // bound for another, and handing one over would send the request, and its
        // credential, to the wrong company.
        if (!pool.empty())
        {
            Connection* u = pool.back();
            pool.pop_back();
            u->from_pool = true; // reused -> a pre-response failure is retry-eligible
            u->retried = false;  // fresh request: one retry available again
            ++_stats.upstream_reused;
            return u;
        }
        const int fd = net::make_client_socket();
        if (fd < 0) return nullptr;
        Connection* u = new Connection();
        u->fd = fd;
        u->is_client = false;
        u->connected = false;
        u->from_pool = false;
        u->upstream_slot = slot; // release() indexes the pool with this
        u->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (up.tls && !tls_attach_upstream(u))
        {
            ::close(fd);
            delete u;
            return nullptr;
        }
#endif
        ++_stats.upstream_conns_opened;
        LB_DEBUG("upstream open ", *u, " pool=", _idle_upstreams.size()); // see ep_ twin
        return u;
    }

    bool Gateway::ur_retry_upstream(Connection* u) noexcept
    {
        // Only safe to resend when the upstream was a pooled reuse, hasn't been
        // retried, and gave us zero response bytes; i.e. the provider closed the
        // idle keep-alive connection without processing the request. (Industry
        // convention: retry an idempotent-or-idle-reused request that failed before
        // any response; don't retry once a partial response has been seen.)
        if (!u->from_pool || u->retried || !u->rbuf.empty()) return false;
        Connection* client = u->peer;
        if (!client) return false;
        [[maybe_unused]] const Upstream& up = upstream_of(u); // see ur_acquire_upstream
        const int fd = net::make_client_socket();
        if (fd < 0) return false;

        Connection* uf = new Connection();
        uf->fd = fd;
        uf->is_client = false;
        uf->connected = false;
        uf->from_pool = false;
        // The same venue: a retry landing elsewhere is a silent reroute of a request
        // already translated for this dialect and carrying its credential.
        uf->upstream_slot = u->upstream_slot;
        uf->retried = true; // this request's one allowed retry is now spent
        // Plaintext invariant pays off here: wbuf was never consumed by the dead
        // session (woff only tracked what was fed, wbuf itself stayed whole), so
        // the retry re-pushes the identical request through a brand-new session.
        uf->wbuf = std::move(u->wbuf);
        uf->woff = 0;
        uf->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (up.tls && !tls_attach_upstream(uf))
        {
            ::close(fd);
            delete uf;
            return false;
        }
#endif
        ++_stats.upstream_conns_opened;

        u->peer = nullptr;
        // WARN, not DEBUG: it is not a cap, it is a recovered failure. A pooled
        // connection was dead when we used it, and the client never learns. That is
        // the point of the retry and also the reason it must be visible: a rising
        // rate here means the provider is dropping keep-alives faster than
        // --pool-idle reaps them, which is a tuning signal nobody can see otherwise.
        LB_WARN("upstream stale, resending on a fresh connection ", *u);
        ur_close(u); // discard the dead pooled connection
        client->peer = uf;
        uf->peer = client;
        ++_stats.upstream_retries;
        ur_submit_connect(uf); // connect fresh, then send on completion
        return true;
    }

    void Gateway::ur_release_upstream(Connection* u) noexcept
    {
        // Checked first: the reset below destroys the very state it reads. See
        // ep_release_upstream for why an unsent request means close, not pool.
        if (!upstream_request_sent(u))
        {
            LB_WARN("closing instead of pooling, request was still going out ", *u);
            ++_stats.upstream_unsent;
            ur_close(u);
            return;
        }
        // send_inflight is not reset here, and must not be: the guard above returns
        // for any connection that still has one, so it is already false. It used to
        // be cleared at the top of this function, inside the TLS ifdef, which papered
        // over the case the guard now refuses outright.
#ifdef LLMBRIDGE_HAVE_TLS
        u->tls_out.clear(); // per-request ciphertext; Session kept for reuse
        u->tls_out_off = 0;
#endif
        // See ep_release_upstream: bounded pool, closed past the cap.
        if (_idle_upstreams.size() >= kMaxIdleUpstreams)
        {
            LB_WARN("CAP pool full, closing instead of pooling ", *u,
                    " limit=", kMaxIdleUpstreams, " (reuse stops here)");
            ur_close(u);
            return;
        }
        u->peer = nullptr;
        u->rbuf.clear();
        u->rdec.reset();
        secure_clear(u->wbuf); // see ep_release_upstream: credential must not idle in the pool
        u->woff = 0;
        u->msg = net::http::Message{};
        u->ts_pooled = now_ns(); // idle-eviction baseline
        _idle_upstreams[static_cast<size_t>(u->upstream_slot)].push_back(u);
    }

    void Gateway::ur_close(Connection* c) noexcept
    {
        if (c->doomed) return;
        if (c->is_client && c->id)
        {
            _clients.erase(c->id);
            LB_DEBUG("close ", *c, " clients=", _clients.size());
        }
        if (!c->is_client && c->upstream_slot >= 0)
        {
            auto& pool = _idle_upstreams[static_cast<size_t>(c->upstream_slot)];
            for (auto it = pool.begin(); it != pool.end(); ++it)
                if (*it == c) { pool.erase(it); break; }
        }
        // This backend closes both kinds in one function where epoll has two, so
        // the upstream branch lives here. After the erase, as in the ep_ twin.
        if (!c->is_client)
            LB_DEBUG("upstream close ", *c, " pool=", pooled_upstream_count());
        // Force any in-flight op on this fd to complete so its inflight count drains
        // (the fd is closed at free time). shutdown alone does not end an armed
        // multishot recv, so also cancel everything on the fd.
        if (c->fd >= 0) { ::shutdown(c->fd, SHUT_RDWR); ur_submit_cancel(c->fd); }
        c->doomed = true;
        _doomed.push_back(c);
        ur_maybe_free(c);
    }

    void Gateway::ur_abort_pair(Connection* client) noexcept
    {
        if (!client) return;
        if (Connection* u = client->peer) { u->peer = nullptr; ur_close(u); }
        ur_close(client);
        ++_stats.errors;
    }

    void Gateway::ur_error_respond(Connection* client, int code, const char* why,
                                   const char* detail) noexcept
    {
        if (!client || client->doomed) return;
        // An authentication refusal names where it came from.
        const std::string peer = (code == 401 || code == 403) ? peer_of(client->fd)
                                                              : std::string{};
        LB_WARN(ReqId{client->req_seq}, " reply ", code, " ", why, " on ", *client,
                peer.empty() ? "" : " peer=", peer);
        if (Connection* u = client->peer) { client->peer = nullptr; u->peer = nullptr; ur_close(u); }
        client->wbuf = build_error(code, detail);
        client->woff = 0;
        client->close_after_resp = true; // ur_finish_client closes once the reply flushes
        ++_stats.errors;
        ur_client_send(client);
    }

    void Gateway::ur_maybe_free(Connection* c) noexcept
    {
        if (!c->doomed || c->inflight > 0) return; // a completion still references it
        if (c->fd >= 0) { ::close(c->fd); c->fd = -1; }
        for (auto it = _doomed.begin(); it != _doomed.end(); ++it)
            if (*it == c) { _doomed.erase(it); break; }
        delete c;
    }

    void Gateway::ur_on_cqe(uint64_t user_data, int res, uint32_t flags) noexcept
    {
        const UOp op = ud_op(user_data);
        if (op == UTimer)
        {
            if (!_draining && !_stop) { sweep_idle(/*uring=*/true); ur_submit_timer(); }
            return;
        }
        if (op == UCancel) return; // control op (cancel-by-fd); not inflight-counted
        if (op == UAccept) { ur_on_accept(res, flags); return; } // not inflight-counted

        Connection* c = ud_conn(user_data);
        // A multishot recv stays armed only while it keeps delivering data (F_MORE
        // set and res > 0). Its terminal completion (EOF/error, res <= 0) can still
        // carry F_MORE, so it must release the inflight slot, or the drain
        // never reaches zero. Only the consuming (terminal) completion decrements.
        const bool armed = (op == URecv) && (flags & IORING_CQE_F_MORE) && res > 0;
        if (!armed) { --c->inflight; --_uring_inflight; }

        if (_draining || c->doomed)
        {
            if (op == URecv && (flags & IORING_CQE_F_BUFFER))
                _bufring.recycle(flags >> IORING_CQE_BUFFER_SHIFT); // return the provided buffer
            if (!armed) ur_maybe_free(c);
            return;
        }
        switch (op)
        {
            case URecv: ur_on_recv(c, res, flags); break;
            case USend: ur_on_send(c, res); break;
            case UConnect: ur_on_connect(c, res); break;
            default: break;
        }
    }

    void Gateway::ur_on_accept(int res, uint32_t flags) noexcept
    {
        if (_stop || _draining)
        {
            if (res >= 0) ::close(res); // shutting down: don't take new work
            return;
        }
        if (!(flags & IORING_CQE_F_MORE)) ur_submit_accept(); // multishot ended -> re-arm
        if (res < 0) return;                                 // transient accept error
        const int fd = res;
        net::set_nodelay(fd);
        Connection* c = new Connection();
        c->fd = fd;
        c->is_client = true;
        c->id = _next_client_id++;
        c->ts_accepted = now_ns();
        c->ts_client_activity = c->ts_accepted;
        c->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (_tls.client_tls && !tls_attach_client(c))
        {
            // Fail closed, exactly as the epoll twin does. No recv is armed yet, so
            // there is nothing in flight and the Connection can be freed directly
            // instead of going on the doomed list.
            ::close(fd);
            delete c;
            ++_stats.errors;
            return;
        }
#endif
        _clients[c->id] = c;
        LB_DEBUG("accept ", *c, " clients=", _clients.size());
        ur_arm_recv(c); // multishot recv stays armed for the connection's life
    }

    void Gateway::ur_on_recv(Connection* c, int res, uint32_t flags) noexcept
    {
        const bool armed = flags & IORING_CQE_F_MORE;

        // -ENOBUFS is not a connection error: the provided-buffer pool was momentarily
        // empty, so the kernel declined to pick a buffer and ended the multishot without
        // consuming one. The cure is to re-arm, not to tear down. Without this branch the
        // fallthrough below would kill the client pair or, mid-stream, report a premature
        // stream end.
        //
        // Measured scope, so nobody mistakes this for a fix to a live bug: on this kernel
        // (7.0) with kUrBufCount=4096, `uring_enobufs` stayed at **0** even at 8192
        // concurrent streams (16384 armed recvs, 4x the pool). The kernel ends the
        // multishot with res>0 and F_MORE clear under pool pressure, which the re-arm at
        // the bottom of this function already handles. So this is defensive hardening for
        // a path that is reachable by contract but was never observed in practice; it is
        // Not the explanation for io_uring's high-concurrency tail latency (that was
        // hypothesised from the kUrBufCount correlation and disproved by this counter).
        // The regression test forces the condition by shrinking the pool.
        if (res == -ENOBUFS)
        {
            ++_stats.uring_enobufs;
            LB_WARN("CAP io_uring provided buffers exhausted; recv re-armed"
                    " total=", _stats.uring_enobufs);
            ur_arm_recv(c);
            return;
        }

        if (res <= 0) // multishot ended: EOF (0) or error (<0)
        {
            if (flags & IORING_CQE_F_BUFFER) _bufring.recycle(flags >> IORING_CQE_BUFFER_SHIFT);
            if (c->is_client) { if (c->peer) ur_abort_pair(c); else ur_close(c); }
            else if (c->peer && c->peer->streaming) ur_stream_on_upstream_eof(c); // stream end, not a failure
            else if (!ur_retry_upstream(c))
            { if (c->peer) { if (!ur_upstream_failed(c->peer, 502, "upstream EOF")) ur_error_respond(c->peer, 502, "upstream EOF, retry exhausted"); } else ur_close(c); }
            return;
        }

        // Copy the bytes out of the kernel-selected buffer, then return it to the
        // pool. For a TLS upstream the bytes are ciphertext: they go through the
        // Session (tls_feed), whose plaintext lands in rbuf, so the parse logic
        // below sees only plaintext either way.
        bool tls_ok = true;
        if (flags & IORING_CQE_F_BUFFER)
        {
            const unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
#ifdef LLMBRIDGE_HAVE_TLS
            if (c->tls)
                tls_ok = tls_feed(c, _bufring.data(bid), static_cast<size_t>(res));
            else
#endif
            {
                if (c->ts_first_byte == 0) c->ts_first_byte = now_ns();
                c->rbuf.append(_bufring.data(bid), static_cast<size_t>(res));
            }
            _bufring.recycle(bid);
        }
        if (!armed) ur_arm_recv(c); // kernel ended the multishot (pool pressure) -> re-arm
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_ok)
        {
            // Fatal TLS failure (bad record, MAC failure, refused certificate).
            // Deliberately not ur_stream_on_upstream_eof for a mid-stream failure: a
            // corrupted stream must not be finalized as if it ended cleanly.
            if (c->peer && c->peer->streaming) { ur_abort_pair(c->peer); return; }
            // An inbound TLS failure is the client's connection dying, not a
            // retryable upstream fault. Retrying it would resend the request to the
            // provider on behalf of a peer that can no longer receive the answer.
            if (c->is_client) { if (c->peer) ur_abort_pair(c); else ur_close(c); return; }
            if (!ur_retry_upstream(c))
            {
                if (c->peer && !ur_upstream_failed(c->peer, 502, "upstream write failed")) ur_error_respond(c->peer, 502, "upstream write failed, retry exhausted");
                else ur_close(c);
            }
            return;
        }
        if (c->tls)
            ur_tls_flush(c); // handshake replies / newly-pushed request bytes
#else
        (void)tls_ok;
#endif

        if (c->is_client)
        {
            ur_try_forward_buffered(c); // forward a framed request iff the client is idle
        }
        else
        {
            // Upstream bytes are the response to the in-flight request. Stray data on
            // an idle pooled upstream (no peer) means it's unusable, so drop it.
            if (!c->peer) { ur_close(c); return; }
            c->peer->ts_up_activity = now_ns(); // upstream made progress

            // Mid-stream: pump the newly-arrived body bytes and return.
            if (c->peer->streaming) { ur_stream_pump(c); return; }

            // First response bytes: peek the head (parse_response_head tolerates
            // chunked, unlike parse_request()); a text/event-stream response enters the
            // streaming pump, everything else the whole-body path below.
            // See the epoll mirror: translated or byte-forward, never Gemini/Cohere.
            if (c->peer->effective_dialect == UpstreamDialect::Anthropic ||
                c->peer->effective_dialect == UpstreamDialect::Azure ||
                !c->peer->translate_body)
            {
                net::http::ResponseHead h;
                const auto hs = net::http::parse_response_head(c->rbuf, h);
                if (hs == net::http::FrameStatus::NeedMore) return; // wait for the full head
                if (hs == net::http::FrameStatus::Error)
                { ur_error_respond(c->peer, 502, "upstream response head framing"); return; }
                // Only a 200 is a real stream; a provider error is relayed with its
                // own status by ur_on_response below (never laundered into a 200).
                if (h.event_stream && h.status == 200)
                {
                    c->peer->ts_up_recvd = now_ns(); // t4: head complete, see the epoll mirror
                    note_quota(c->peer, h);
                    ur_begin_stream(c, h);
                    return;
                }
            }

            // parse_response, not parse: providers return non-streaming bodies
            // chunked over HTTP/1.1 and parse_request() rejects that by design (http.hpp).
            const auto r = net::http::parse_response(c->rbuf, c->rdec);
            if (r.failed()) { ur_error_respond(c->peer, 502, "upstream response framing"); return; }
            if (!r.complete()) return; // armed recv delivers the rest
            c->peer->ts_up_recvd = now_ns();
            note_quota(c->peer, r.head);
            note_upstream_error(c->peer, r.head, r.body);
            note_served_tier(c->peer, r.body, /*tail=*/true);
            ur_on_response(c, r.head, r.body, r.total_len);
        }
    }

    void Gateway::ur_try_forward_buffered(Connection* c) noexcept
    {
        // Forward the next framed request only when the client is idle. No request
        // in flight (peer) and no response still draining to it (wbuf).
        if (c->peer != nullptr || !c->wbuf.empty() || c->rbuf.empty()) return;
        // Same memo as the epoll twin; see the note there.
        if (c->client_frame_want && c->rbuf.size() < c->client_frame_want) return;
        net::http::Message m;
        const auto st = net::http::parse_request(c->rbuf, m);
        if (st == net::http::FrameStatus::NeedMore)
        {
            if (m.total_len && !c->client_frame_want)
            {
                c->client_frame_want = m.total_len;
                c->rbuf.reserve(m.total_len);
                if (c->rbuf.size() <= m.header_len &&
                    expects_continue(std::string_view(c->rbuf.data(), m.header_len)))
                    send_interim_continue(c, /*uring=*/true);
            }
            return; // the armed recv will deliver more
        }
        c->client_frame_want = 0;
        if (st == net::http::FrameStatus::Error) { ur_error_respond(c, 400, "request framing"); return; }
        c->msg = m;
        c->ts_req_recvd = now_ns();
        c->client_upload_ns = span_since(c->ts_first_byte, c->ts_req_recvd);
        c->client_conn_reused = c->ever_framed;
        c->client_conn_setup_ns = c->client_conn_reused ? 0 : span_since(c->ts_accepted, c->ts_req_recvd);
        c->ts_first_byte = c->rbuf.size() > m.total_len ? c->ts_req_recvd : 0; // see epoll mirror
        // See the epoll mirror: assigned here, before forward touches rbuf.
        c->req_seq = g_seq.fetch_add(1, std::memory_order_relaxed);
        // A new request: the previous one's saved copy and failover budget are spent.
        // Cleared here, at the one place a request begins, and not at the many
        // places one can end.
        c->failover_req.clear();
        c->failover_attempts = 0;
        // Fail closed on a body we cannot read. Nothing here inflates, and every
        // reader downstream scans bytes assuming JSON: wants_stream, model_of and
        // the translator.
        if (c->msg.encoded) { ur_error_respond(c, 415, "compressed request body"); return; }
        c->policy_tag = 0;
        if (_sink) sink_capture(c);
        if (_sink || _policy) capture_model(c);
        LB_DEBUG(ReqId{c->req_seq}, " ", request_line(c->rbuf), " on ", *c);
        // The policy seam; see the epoll mirror for why it sits exactly here.
        // Default to the first upstream, so a build with no policy, and a policy that
        // only authenticates, both behave exactly as the single-upstream gateway did.
        c->upstream_slot = 0;
        if (_policy)
        {
            const Decision d = policy_decision(c, m);
            if (!d.allow) { ur_error_respond(c, d.deny_status, d.reason); return; }
            // Out of range is the default, not an error: a policy that does not route
            // leaves this at -1, and one that names a venue that has since left the
            // table must not send the request somewhere arbitrary.
            if (d.upstream_index >= 0 &&
                static_cast<size_t>(d.upstream_index) < _upstreams.size())
                c->upstream_slot = d.upstream_index;
            else if (d.upstream_index >= 0)
                LB_WARN(ReqId{c->req_seq}, " policy chose upstream ",
                        static_cast<int64_t>(d.upstream_index), " of ",
                        static_cast<int64_t>(_upstreams.size()), "; using 0");
            // Valid only through the forward below, which runs in this call stack.
            c->model_override = d.model;
            c->tier_override = d.service_tier;
        }
        ur_forward(c); // resolves the translation for the chosen venue; see ur_forward
    }

    void Gateway::ur_forward(Connection* c) noexcept
    {
        // See the epoll mirror: the venue is stamped on the client before this runs.
        const Upstream& up = upstream_of(c);
        // Resolve the translation for the venue this attempt chose, on every forward; see epoll
        // for why it lives here and not at the decision point (failover changes venue).
        {
            const TranslationPlan plan = resolve_dialect(c, up);
            if (!plan.ok) { ur_error_respond(c, 400, plan.why); return; }
            c->translate_body = plan.translate;
            c->effective_dialect = plan.venue;
        }
        std::string upstream_bytes;
        if (c->translate_body)
        {
            std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
            const std::string_view client_hdrs(c->rbuf.data(), c->msg.header_len);
            const char* why = "";
            // Either failure => 400, and nothing goes upstream.
            // (The credential branch called the EPOLL responder until the ep_/ur_
            // split. It was harmless by three accidents, and the prefixes turned it
            // into a grep; folding both loops onto one builder removes the chance of
            // the next such divergence entirely.)
            if (!build_translated_request(up, c->effective_dialect, body, client_hdrs, _strip_headers,
                                          upstream_bytes, why, c->model_override,
                                          &c->wants_usage))
            {
                if (why[0] == 't')
                    ur_error_respond(c, 400, "translate", translate_failure(body));
                else
                    ur_error_respond(c, 400, "malformed credential",
                                     refuse::kCredential);
                return;
            }
        }
        else
        {
            // No `wants_usage` on this path, deliberately. Its only reader is the
            // Anthropic-to-OpenAI SSE translator, built only when `translate_body`
            // is set.
            // Byte-forward still rebuilds, even with nothing to strip: Host must name
            // the venue, not the client's idea of us.
            // Byte-forward rewrites the model by splicing the value, so every other
            // byte the client sent survives; the derived Content-Length then describes
            // the spliced body without anyone having to remember to update it.
            std::string rewritten;
            if (!c->model_override.empty() || !c->tier_override.empty())
            {
                std::string_view had;
                rewritten = provider::apply_overrides(
                    std::string_view(c->rbuf.data() + c->msg.header_len, c->msg.body_len),
                    c->model_override, c->tier_override, &had);
                if (rewritten.empty())
                { ur_error_respond(c, 400, "cannot apply route overrides"); return; }
                // Copied, not held: `had` points into the request buffer, which is
                // reused before the sink runs. Truncated and not refused, because a
                // tier too long to be one is still worth reporting.
                c->asked_tier_len = static_cast<uint8_t>(
                    had.size() < sizeof c->asked_tier ? had.size() : sizeof c->asked_tier);
                // Guarded, because the common case is a caller that named no tier and
                // an empty string_view's data() is null.
                if (c->asked_tier_len)
                    std::memcpy(c->asked_tier, had.data(), c->asked_tier_len);
            }
            upstream_bytes = request_without(std::string_view(c->rbuf.data(), c->msg.total_len),
                                             c->msg.header_len, _strip_headers, up.host_hdr,
                                             up.base_path, rewritten);
            // Refused, not repaired: a venue with a base path needs an origin-form
            // target to prefix, and nothing has been sent upstream at this point.
            if (upstream_bytes.empty())
            { ur_error_respond(c, 400, "request target not origin-form"); return; }
        }

        Connection* u = ur_acquire_upstream(c->upstream_slot);
        if (!u)
        {
            if (!ur_upstream_failed(c, 502, "no upstream (connect failed)"))
                ur_error_respond(c, 502, "no upstream (connect failed)");
            return;
        }

        LB_DEBUG(ReqId{c->req_seq}, " upstream ", *u, " pooled=", u->from_pool,
                 " dest=", upstream_of(u).ip, ":", upstream_of(u).port);
        u->wbuf = std::move(upstream_bytes);
        u->woff = 0;
        // Keep the original bytes when a failover could use them: the rebuilt request
        // above was translated for this venue's dialect, so it cannot be resent to a
        // different one. One copy per in-flight request, and only where it can pay off.
        if (_policy && _upstreams.size() > 1 && c->failover_req.empty())
        {
            const size_t extra = c->rbuf.size() - c->msg.total_len;
            c->failover_req.swap(c->rbuf);
            c->rbuf.assign(c->failover_req, c->msg.total_len, extra);
            c->failover_req.resize(c->msg.total_len);
        }
        else c->rbuf.erase(0, c->msg.total_len);
        c->peer = u;
        u->peer = c;
        c->ever_framed = true;        // past the setup deadline for good
        c->ts_client_activity = now_ns(); // and the idle clock restarts here
        c->ts_req_built = now_ns();   // end of our request-side work
        c->ts_up_activity = c->ts_req_built; // idle-timeout baseline for this request
        if (u->connected) c->ts_wire_ready = c->ts_req_built; // pooled: no handshake
        c->upstream_pooled = u->connected;

#ifdef LLMBRIDGE_HAVE_TLS
        if (u->connected && u->tls)
        {
            tls_push_wbuf(u); // pooled conns are past the handshake
            ur_tls_flush(u);
            return;
        }
#endif
        if (u->connected) ur_submit_send(u);
        else ur_submit_connect(u); // connect first; on completion we send the request
    }

    void Gateway::ur_on_connect(Connection* u, int res) noexcept
    {
        if (res < 0)
        {
            Connection* cl = u->peer;
            u->peer = nullptr;
            ur_close(u);
            if (cl) { cl->peer = nullptr; if (!ur_upstream_failed(cl, 502, "upstream connect refused")) ur_error_respond(cl, 502, "upstream connect refused"); }
            else ++_stats.errors;
            return;
        }
        u->connected = true;
        // t2 for a plaintext upstream only; see the epoll twin and tls_feed().
        if (!upstream_is_tls(u) && u->peer && u->peer->ts_wire_ready == 0)
            u->peer->ts_wire_ready = now_ns();
        ur_arm_recv(u); // arm the multishot recv for this upstream's life
#ifdef LLMBRIDGE_HAVE_TLS
        if (u->tls)
        {
            // The request waits in wbuf (plaintext) until the handshake completes;
            // what goes out now is the ClientHello.
            u->tls->start_handshake();
            ur_tls_flush(u);
            return;
        }
#endif
        ur_submit_send(u); // then send the request
    }

    void Gateway::ur_on_response(Connection* u, const net::http::ResponseHead& h,
                                std::string_view body_buf, size_t total_len) noexcept
    {
        Connection* client = u->peer;
        if (client->translate_body)
        {
            const std::string_view body = body_buf;
            // Relay a provider failure with its own status + message (see the epoll
            // mirror): a 429/529/400 must not be flattened into a generic 502.
            if (h.status != 0 && h.status != 200)
            {
                client->wbuf = build_http_status(
                    h.status, reason_for(h.status),
                    provider::upstream_error_to_openai(body, "upstream_error"));
                client->woff = 0;
                client->peer = nullptr;
                if (h.keep_alive) ur_release_upstream(u); else ur_close(u);
                ++_stats.errors;
                ur_client_send(client);
                return;
            }
            std::string tbody = xlate_resp(client->effective_dialect, body);
            // Scanned unconditionally, not only when --timing-headers is on: the
            // response header is one surface for these numbers and the log is
            // another, and a number that appears in one but not the other is the
            // kind of inconsistency this file has been bitten by before.
            {
                const BodyUsage bu = scan_usage(tbody);
                client->tok_in = bu.in;
                client->tok_out = bu.out;
                client->tok_cached = bu.cached;
                // The cache-creation counts are read from the provider's own body, never the
                // translated one. OpenAI defines no field for them.
                const BodyUsage up = scan_usage(body);
                client->tok_cache_write = up.cache_write;
                client->tok_cw_5m = up.cache_write_5m;
                client->tok_cw_1h = up.cache_write_1h;
            }
            if (tbody.empty())
            {
                client->peer = nullptr;
                ur_release_upstream(u); // framing was valid; the upstream conn is reusable
                ur_error_respond(client, 502, "response translate");
                return;
            }
            std::string timing;
            if (_timing_headers)
            {
                const int64_t ts_resp_built = now_ns(); // t5, see the epoll twin
                const TimingSplit sp = timing_split(
                    client->ts_req_recvd, client->ts_req_built, client->ts_wire_ready,
                    client->ts_up_sent, client->ts_up_recvd, ts_resp_built);
                append_timing_headers(timing, client->ts_req_recvd, sp.compute_ns / 1000,
                                      sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                      sp.upstream_ns / 1000, "x-llmbridge-upstream-us",
                                      client->req_seq, client->client_upload_ns / 1000);
                append_usage_headers(timing, tbody);
            }
            client->wbuf = build_http("HTTP/1.1 200 OK", tbody, timing);
        }
        else
        {
            // See the epoll mirror: a decoded chunked body is re-framed with
            // Content-Length instead of relayed verbatim.
            // The upstream's own status, not a hardcoded 200. Re-framing a chunked
            // response used to relabel every one of them as success, so a provider's
            // 429 reached the caller as a completion whose body happened to be a
            // rate-limit error, with Retry-After dropped. The client cannot back off
            // from a 200. Content-Length responses were relayed verbatim and were
            // never affected, which is why it survived: only chunkedness triggers it.
            if (h.chunked)
                client->wbuf = build_http_status(h.status ? h.status : 200,
                                                 reason_for(h.status ? h.status : 200),
                                                 body_buf);
            else client->wbuf.assign(u->rbuf.data(), total_len);
            // The counts, from the venue's own body. Only the translated branch above
            // scanned, so a byte-forward reported nothing: a sink saw -1 and a tape
            // recorded a request that cost zero tokens at a real price.
            const BodyUsage bu = scan_usage(body_buf);
            client->tok_in = bu.in;
            client->tok_out = bu.out;
            client->tok_cached = bu.cached;
            client->tok_cache_write = bu.cache_write;
            client->tok_cw_5m = bu.cache_write_5m;
            client->tok_cw_1h = bu.cache_write_1h;
        }
        // Pool the upstream only if it will stay open: the response must say
        // keep-alive and (for passthrough, where the client's Connection header was
        // forwarded verbatim) the client must not have asked to close. Otherwise the
        // upstream is about to close on us. Drop it instead of reusing a corpse.
        const bool pool_upstream =
            h.keep_alive && (client->translate_body || client->msg.keep_alive);
        client->woff = 0;
        client->peer = nullptr;
        // Drop the framed message; anything left is the next pipelined response.
        u->rbuf.erase(0, total_len);
        u->rdec.reset(); // see the epoll mirror
        if (pool_upstream) ur_release_upstream(u);
        else ur_close(u);
        ur_client_send(client);
    }

    void Gateway::ur_on_send(Connection* c, int res) noexcept
    {
        if (res <= 0)
        {
            if (c->is_client) { if (c->peer) ur_abort_pair(c); else ur_close(c); }
            else if (!ur_retry_upstream(c))
            { if (c->peer) { if (!ur_upstream_failed(c->peer, 502, "upstream EOF")) ur_error_respond(c->peer, 502, "upstream EOF, retry exhausted"); } else ur_close(c); }
            return;
        }
        // This send completed, so no SQE references the buffer right now. Cleared
        // Once here, before any branch below runs; a partial send re-arms it by
        // calling ur_submit_send again. When each branch cleared its own, two of
        // the four forgot, and the flag meant different things on different paths.
        c->send_inflight = false;
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            // TLS on either leg: this send moved ciphertext (tls_out); woff/wbuf
            // track plaintext fed to the Session and are not touched here.
            c->tls_out_off += static_cast<size_t>(res);
            if (c->tls_out_off < c->tls_out.size()) { ur_submit_send(c); return; } // partial
            // The Session may hold more: later handshake flights, or plaintext that
            // could not be pushed before the handshake finished.
            if (c->tls->handshake_done() && c->woff < c->wbuf.size()) tls_push_wbuf(c);
            ur_tls_flush(c); // refill from the write BIO; resubmit if non-empty
            if (c->send_inflight) return; // more ciphertext went out; wait for it

            if (!c->is_client)
            {
                if (c->peer && tls_wbuf_flushed(c)) c->peer->ts_up_sent = now_ns();
                return;
            }
            // Inbound leg: the response is fully encrypted and fully on the wire,
            // so this is the same completion point the plaintext path reaches when
            // woff catches up with wbuf. Streams continue, everything else finishes.
            if (!tls_wbuf_flushed(c)) return;
            if (c->streaming)
            {
                c->wbuf.clear();
                c->woff = 0;
                ur_stream_flush(c);
            }
            else if (!c->wbuf.empty())
            {
                if (_sink) sink_emit(c, status_of(c->wbuf), /*streamed=*/false);
                LB_DEBUG(ReqId{c->req_seq}, " status=", status_of(c->wbuf),
                         " bytes=", c->wbuf.size(), " tokens_in=", c->tok_in,
                         " tokens_out=", c->tok_out, " keep_alive=", c->msg.keep_alive,
                         " on ", *c);
                c->wbuf.clear();
                c->woff = 0;
                ur_finish_client(c);
            }
            return;
        }
#endif
        c->woff += static_cast<size_t>(res);
        if (c->woff < c->wbuf.size()) { ur_submit_send(c); return; } // partial: re-arms

        if (!c->is_client)
        {
            // Request fully sent. The response arrives via the already-armed multishot
            // recv. Keep wbuf so we can resend on a stale-connection failure.
            if (c->peer) c->peer->ts_up_sent = now_ns();
        }
        else if (c->streaming)
        {
            // This SSE buffer is fully out. Free the send slot and either send the
            // next pending bytes or finalize if the stream has ended.
            c->wbuf.clear();
            c->woff = 0;
            ur_stream_flush(c);
        }
        else
        {
            if (_sink) sink_emit(c, status_of(c->wbuf), /*streamed=*/false);
            LB_DEBUG(ReqId{c->req_seq}, " status=", status_of(c->wbuf), " bytes=", c->wbuf.size(),
                     " tokens_in=", c->tok_in, " tokens_out=", c->tok_out,
                     " keep_alive=", c->msg.keep_alive, " on ", *c);
            c->wbuf.clear();
            c->woff = 0;
            ur_finish_client(c);
        }
    }

    void Gateway::ur_finish_client(Connection* c) noexcept
    {
        // Error replies (close_after_resp) are counted as errors, not in the latency
        // histograms; their timing stamps are unset.
        if (!c->close_after_resp)
        {
            const int64_t ts_resp_sent = now_ns(); // t6, name matches the epoll twin
            if (ts_resp_sent - _t_start >= _warmup_ns)
            {
                // req_path is our request-side work only; the wait for connect +
                // TLS is its own histogram so it cannot inflate the added-latency
                // claim. overhead = req_path + resp_path, connect excluded.
                //
                // Derived from the same timing_split() the headers use, so
                // connect(TLS) here and x-llmbridge-connect-us there cannot come to
                // mean different things. t5 does not enter this grouping, so t4 is
                // passed in its place.
                const TimingSplit sp = timing_split(c->ts_req_recvd, c->ts_req_built,
                                                    c->ts_wire_ready, c->ts_up_sent,
                                                    c->ts_up_recvd, c->ts_up_recvd);
                const int64_t conn_ns = sp.connect_ns;
                const int64_t req_ns = sp.req_path_ns;
                const int64_t resp_ns = ts_resp_sent - c->ts_up_recvd;
                if (req_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(req_ns));
                if (conn_ns >= 0) _stats.connect.record(static_cast<uint64_t>(conn_ns));
                if (resp_ns >= 0) _stats.resp_path.record(static_cast<uint64_t>(resp_ns));
                if (req_ns >= 0 && resp_ns >= 0)
                    _stats.overhead.record(static_cast<uint64_t>(req_ns + resp_ns));
                ++_stats.requests;
            }
        }
        const bool close_now = c->close_after_resp || !c->msg.keep_alive;
        c->msg = net::http::Message{};
        if (close_now) { ur_close(c); return; }
        // The client's multishot recv is still armed; a pipelined next request may
        // already sit in rbuf. Forward it, else the armed recv delivers more.
        ur_try_forward_buffered(c);
    }

    // ── io_uring streaming pump (Anthropic->OpenAI SSE) ─────────────────────
    // Enter streaming: send the client SSE headers, then translate the body as it
    // arrives. Output accumulates in wpending; ur_stream_flush moves it into wbuf
    // (kept immutable during an in-flight send) one send at a time.
    void Gateway::ur_begin_stream(Connection* u, const net::http::ResponseHead& h) noexcept
    {
        Connection* client = u->peer;
        client->streaming = true;
        client->stream_chunked = h.chunked;
        client->stream_keep_alive = h.keep_alive; // decides poolability at stream end
        stream_warn_if_encoded(client, h);
        // Only a request that needs translating gets a translator.
        if (client->translate_body && client->effective_dialect == UpstreamDialect::Anthropic)
            client->sse_xlate = std::make_unique<provider::AnthropicToOpenAiSse>(-1, client->wants_usage);
        // Chosen once, before either head is built, and remembered.
        client->stream_chunked_out = stream_reusable_out(client);
        if (_timing_headers)
        {
            // t4 = provider's first response byte, stamped by the caller.
            std::string timing;
            // t5 = t4: a stream's response is not built at one instant, so the
            // compute leg is the request side alone instead of an invented figure.
            const TimingSplit sp = timing_split(
                client->ts_req_recvd, client->ts_req_built, client->ts_wire_ready,
                client->ts_up_sent, client->ts_up_recvd, client->ts_up_recvd);
            append_timing_headers(timing, client->ts_req_recvd, sp.compute_ns / 1000,
                                  sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                  sp.upstream_ns / 1000, "x-llmbridge-upstream-ttfb-us",
                                  client->req_seq, client->client_upload_ns / 1000);
            client->wpending.assign(sse_head_with_timing(
                timing, client->stream_chunked_out ? kSseHeadChunked : kSseHead));
        }
        else
            client->wpending.assign(client->stream_chunked_out ? kSseHeadChunked : kSseHead);
        u->rbuf.erase(0, h.header_len); // consume the head; the rest is body
        ur_stream_pump(u);
    }

    void Gateway::ur_stream_pump(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { ur_close(u); return; }

        const StreamStep st = stream_step(client, u->rbuf, client->wpending, /*at_eof=*/false);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            // Truncate honestly: no fabricated [DONE]. Flush what we have, then the
            // finalize path closes the client (an aborted SSE body).
            stream_truncate(client);
            ur_stream_flush(client);
            return;
        }
        if (client->wpending.size() + client->wbuf.size() > kUrStreamBufCap)
        {
            LB_WARN("CAP stream buffer exceeded, dropping the stream ", *client,
                    " buffered=", client->wpending.size() + client->wbuf.size(),
                    " limit=", static_cast<uint64_t>(kUrStreamBufCap),
                    " (the client is slower than the provider)");
            ur_abort_pair(client);
            return;
        }
        ur_stream_flush(client);
    }

    void Gateway::ur_stream_on_upstream_eof(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { ur_close(u); return; }
        const StreamStep st = stream_step(client, u->rbuf, client->wpending, /*at_eof=*/true);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            stream_truncate(client);
        }
        ur_stream_flush(client);
    }

    // Serialize sends: only one send SQE outstanding (concurrent sends on a fd would
    // interleave). wbuf is (re)filled from wpending only when idle, so its bytes stay
    // put while the kernel reads them for an in-flight send: no realloc-under-kernel.
    void Gateway::ur_stream_flush(Connection* client) noexcept
    {
        if (client->send_inflight) return; // a send is already draining wbuf
        if (client->wpending.empty())
        {
            if (client->stream_ended) ur_finalize_stream(client); // nothing left + ended
            return;
        }
        client->wbuf = std::move(client->wpending);
        client->wpending.clear();
        client->woff = 0;
        ur_client_send(client); // sets send_inflight; closes the client on SQE exhaustion
    }

    void Gateway::ur_finalize_stream(Connection* client) noexcept
    {
        if (Connection* u = client->peer)
        {
            const bool reusable = stream_upstream_reusable(client, u);
            client->peer = nullptr;
            u->peer = nullptr;
            if (reusable) ur_release_upstream(u); // see the epoll mirror
            else ur_close(u);
        }
        LB_DEBUG(ReqId{client->req_seq}, " stream ended ",
                 client->close_after_resp ? "TRUNCATED" : "clean",
                 " tokens_in=", stream_tokens(client).in,
                 " tokens_out=", stream_tokens(client).out,
                 " on ", *client);
        // Only a cleanly-terminated stream counts as served (see the epoll mirror).
        if (!client->close_after_resp)
        {
            ++_stats.requests;
            stream_record_latency(client);
        }
        if (_sink) sink_emit(client, 200, /*streamed=*/true);

        // Keep the connection when the framing gave the body an end marker.
        if (!stream_client_reusable(client)) { ur_close(client); return; }
        stream_reset_for_next(client);
        client->wpending.clear();
        client->msg = net::http::Message{};
        // The client's multishot recv stays armed.
        ur_try_forward_buffered(client);
    }

    int Gateway::run_uring()
    {
        _t_start = now_ns();
        _active_backend = 2;

        unsigned flags = 0;
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_DEFER_TASKRUN)
        flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
#endif
        if (!_ring.init(kUrRingDepth, flags) && !_ring.init(kUrRingDepth, 0))
        {
            LB_WARN("io_uring init failed; falling back to epoll");
            return run_epoll();
        }
        const unsigned bufcount = _uring_buf_count ? _uring_buf_count : kUrBufCount;
        if (!_bufring.init(_ring, kUrBufGroup, bufcount, kUrBufSize))
        {
            LB_WARN("io_uring provided-buffer ring unavailable (", _bufring.init_stage(),
                    " errno=", _bufring.init_errno(), " ",
                    std::strerror(_bufring.init_errno()), "); falling back to epoll");
            return run_epoll();
        }
        // Sized once, never resized: ur_submit_connect hands the kernel a pointer into
        // this vector that must stay valid until the connect completes.
        _upstream_addrs.resize(_upstreams.size());
        for (size_t i = 0; i < _upstreams.size(); ++i)
            if (!net::resolve_ipv4(_upstreams[i].ip.c_str(), _upstreams[i].port,
                                   _upstream_addrs[i]))
                return 1;

        _uring_ts.tv_sec = kPollTickMs / 1000;
        _uring_ts.tv_nsec = static_cast<long long>(kPollTickMs % 1000) * 1000000LL;

        ur_submit_accept();
        ur_submit_timer();

        auto reap = [this] {
            _ring.for_each_cqe([this](const io_uring_cqe* cqe) {
                ur_on_cqe(cqe->user_data, cqe->res, cqe->flags);
            });
        };

        while (!_stop)
        {
            const int r = _ring.submit_and_wait(1);
            if (r < 0 && r != -EINTR && r != -ETIME) break;
            reap();
        }

        // Graceful drain: stop taking new work, force every live fd's in-flight ops
        // to complete, and reap until nothing is outstanding, so no kernel op
        // writes into a buffer we're about to free. Acquired upstreams are reachable
        // only via client->peer, so shut those down too.
        _draining = true;
        auto stop_conn = [this](Connection* c) {
            if (c->fd >= 0) { ::shutdown(c->fd, SHUT_RDWR); ur_submit_cancel(c->fd); }
        };
        for (auto& [id, c] : _clients)
        {
            stop_conn(c);
            if (c->peer) stop_conn(c->peer); // acquired upstreams reachable only via peer
        }
        for (auto& pool : _idle_upstreams)
            for (Connection* u : pool) stop_conn(u);
        // doomed conns were already shut down + cancelled by ur_close().

        while (_uring_inflight > 0)
        {
            const int r = _ring.submit_and_wait(1);
            if (r < 0 && r != -EINTR && r != -ETIME) break;
            reap();
        }

        // Free acquired (in-flight) upstreams now drained but tracked only via peer;
        // the rest (_clients, _idle_upstreams, _doomed, listen_conn) are freed by
        // ~Gateway.
        for (auto& [id, c] : _clients)
            if (Connection* u = c->peer) { c->peer = nullptr; if (u->fd >= 0) ::close(u->fd); delete u; }

        return 0;
    }
#endif // LLMBRIDGE_HAVE_URING
} // namespace llmbridge
