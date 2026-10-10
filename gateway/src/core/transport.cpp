// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Bytes between a socket and a connection's plaintext buffers: the TLS Session on
// either leg, the sent-request test and the interim 100 Continue.

#include "gateway/gateway.hpp"

#include "core/limits.hpp"
#include "request.hpp"

#include <sys/socket.h>

#include <cstdint>
#include <memory>

namespace llmbridge
{
    using namespace detail;

    // Shared by both backends, hence no ep_/ur_ prefix: is this upstream carrying
    // TLS? Compiles to `false` in a build without TLS support.
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

        // Must be encrypted and has no Session: refuse the write, the caller tears it
        // down. Unreachable today and kept on purpose: GATEWAY-INTERNALS.md 10d.
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
        if (u->tls_out.capacity() < need) u->tls_out.reserve(need + need / 8);
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
            // Headroom, as for the plaintext scratch: an exact reserve on a context
            // that grows every turn is a fresh allocation, and fresh pages, per request.
            const size_t need = u->tls_out.size() + todo + todo / 512 + 256;
            if (u->tls_out.capacity() < need)
            {
                ++_stats.tls_out_grows;
                u->tls_out.reserve(need + need / 8);
            }
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
            // The error is an OpenSSL reason plus a file path, no key material. A
            // client handshake failure is almost always a scanner, so it is DEBUG and
            // counted; anything else is actionable and WARN. GATEWAY-INTERNALS.md 10d.
            const bool scanner_noise = u->is_client && !hs_was_done;
            if (scanner_noise)
            {
                ++_stats.client_tls_handshake_failures;
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
            u->wire_ready = true;
            if (u->peer) u->peer->req.f.ts_wire_ready = now_ns();
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

    /// Answer `Expect: 100-continue` without entering the response path.
    bool Gateway::send_interim_continue(Connection* c, bool uring) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            if (!c->tls->handshake_done()) return true;
            const auto* p = reinterpret_cast<const uint8_t*>(kContinue.data());
            if (c->tls->write_plaintext({p, kContinue.size()}) != kContinue.size()) return true;
#ifdef LLMBRIDGE_HAVE_URING
            if (uring) { ur_tls_flush(c); return true; } // completion sees an empty wbuf: nothing finishes
#endif
            (void)uring;
            bool done = false;
            if (!ep_tls_flush(c, &done)) return true;
            if (!done) c->client_interim_inflight = true; // the writable event drains it
            return true;
        }
#else
        (void)uring;
#endif
        const ssize_t n = ::send(c->fd, kContinue.data(), kContinue.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        // A torn interim line is a protocol error the client cannot recover from. The
        // caller closes through its backend: setting `doomed` here left epoll spinning on
        // a registered fd and let io_uring free a connection still in _clients.
        return n <= 0 || static_cast<size_t>(n) == kContinue.size();
    }

} // namespace llmbridge
