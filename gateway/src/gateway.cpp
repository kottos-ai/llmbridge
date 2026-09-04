// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "gateway/gateway.hpp"

#include "loop.hpp"
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

    namespace detail
    {
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
    } // namespace detail

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

#endif // LLMBRIDGE_HAVE_TLS

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
} // namespace llmbridge
