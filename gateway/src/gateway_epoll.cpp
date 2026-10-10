// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The epoll half of Gateway: every ep_ method and run_epoll(). Its twin is
// gateway_uring.cpp; the shared methods are in gateway.cpp. DESIGN.md "Naming
// conventions" says why the two are kept apart and never merged.

#include "gateway/gateway.hpp"

#include "core/limits.hpp"
#include "core/registry.hpp"
#include "net/secure.hpp"
#include "net/socket_util.hpp"
#include "request.hpp"
#include "response.hpp"
#include "scan.hpp"
#include "stream.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string_view>
#include <sys/epoll.h>

namespace llmbridge
{
    using namespace detail;

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

    void Gateway::ep_pause_accept() noexcept
    {
        epoll_event ev{};
        ev.data.ptr = _listen_conn;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, _listen_fd, &ev);
        _accept_resume_ns = now_ns() + kAcceptBackoffNs;
        ++_stats.accept_backoffs;
        LB_WARN("CAP out of file descriptors; accepting paused for 100 ms, total=",
                _stats.accept_backoffs);
    }

    void Gateway::ep_resume_accept() noexcept
    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = _listen_conn;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, _listen_fd, &ev);
        _accept_resume_ns = 0;
    }

    // The one writer of a connection's interest mask, from both flags: arming a write
    // used to set EPOLLIN too, silently undoing a backpressure pause.
    void Gateway::ep_sync_interest(Connection* c) noexcept
    {
        epoll_event ev{};
        ev.events = (c->read_paused ? 0u : static_cast<uint32_t>(EPOLLIN)) |
                    (c->write_armed ? static_cast<uint32_t>(EPOLLOUT) : 0u);
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }

    void Gateway::ep_arm_write(Connection* c) noexcept
    {
        if (c->write_armed) return;
        c->write_armed = true;
        ep_sync_interest(c);
    }

    void Gateway::ep_disarm_write(Connection* c) noexcept
    {
        if (!c->write_armed) return;
        c->write_armed = false;
        ep_sync_interest(c);
    }

    // Backpressure: pause/resume reading a connection. Used to stop pulling
    // upstream SSE bytes while the client's write buffer is draining, so a slow
    // client can't make us buffer an unbounded stream. (EPOLLHUP/EPOLLERR still
    // fire while paused, so an upstream close is never missed.)
    void Gateway::ep_pause_read(Connection* c) noexcept
    {
        if (c->read_paused) return;
        c->read_paused = true;
        ep_sync_interest(c);
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
        c->read_paused = false;
        ep_sync_interest(c);
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
            if (c->tls->handshake_done()) tls_push_out(c);
            bool flushed = false;
            if (!ep_tls_flush(c, &flushed)) return false;
            *done = flushed && tls_out_flushed(c);
            return true;
        }
#endif
        while (!c->out.wire().empty())
        {
            const std::string_view w = c->out.wire();
            const ssize_t n = ::write(c->fd, w.data(), w.size());
            if (n > 0)
            {
                c->out.sent(static_cast<size_t>(n));
                c->sent_bytes += static_cast<uint64_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        // The bytes stay in `out` once sent: a request stays available for a
        // stale-connection resend until the response is read.
        *done = true;
        return true;
    }

#ifdef LLMBRIDGE_HAVE_TLS

    bool Gateway::ep_tls_flush(Connection* u, bool* done) noexcept
    {
        *done = false;
        tls_pump_out(u);
        while (!u->tls_out.wire().empty())
        {
            const std::string_view w = u->tls_out.wire();
            const ssize_t n = ::write(u->fd, w.data(), w.size());
            if (n > 0)
            {
                u->tls_out.sent(static_cast<size_t>(n));
                u->sent_bytes += static_cast<uint64_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                ep_arm_write(u);
                return true;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        u->tls_out.clear();
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
        const bool was_flushed = u->peer && tls_out_flushed(u);
        bool done = false;
        if (!ep_tls_flush(u, &done)) return false;
        if (u->peer && !was_flushed && tls_out_flushed(u))
            u->peer->req.f.ts_up_sent = now_ns();
        return true;
    }

#endif // LLMBRIDGE_HAVE_TLS

    bool Gateway::ep_upstream_failed(Connection* client, int status, const char* why) noexcept
    {
        const Retry r = failover_target(client, status, why);
        if (!r.retry) return false;

        LB_WARN(ReqId{client->req.f.req_seq}, " venue ", static_cast<int64_t>(client->upstream_slot),
                " failed (", why, "); retrying on ", static_cast<int64_t>(r.upstream_index));
        if (Connection* u = unpair(client)) ep_close_upstream(u);
        ++client->req.f.failover_attempts;
        client->upstream_slot = r.upstream_index;
        // Put the original request back at the front of rbuf. ep_forward rebuilds it for
        // the new venue's dialect and erases exactly these bytes again, so a pipelined
        // request already queued behind it keeps its place.
        client->rbuf.insert(0, client->req.saved);
        ++_stats.upstream_failovers;
        ep_forward(client);
        return true;
    }

    Connection* Gateway::ep_acquire_upstream(int slot) noexcept
    {
        const Upstream& up = _upstreams[static_cast<size_t>(slot)];
        // Only this venue's pool: a connection to one provider cannot serve a request
        // bound for another, and handing one over would send the request, and its
        // credential, to the wrong company.
        if (Connection* u = _pool->acquire(slot))
        {
            u->from_pool = true; // reused -> a pre-response failure is retry-eligible
            u->ts_pool_taken = now_ns();
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
        u->ts_accepted = now_ns(); // the connect deadline counts from here
        // make sure the upstream ip did not rotate
        (void)net::resolve_ipv4(up.ip.c_str(), up.port, u->up_addr);
        u->rbuf.reserve(kInitialBuf);
        adopt_warm(u);
#ifdef LLMBRIDGE_HAVE_TLS
        if (up.tls && !tls_attach_upstream(u))
        {
            ::close(fd);
            delete u;
            return nullptr;
        }
#endif
        _upconns->add(u);
        ep_add_read(u);
        ep_arm_write(u); // learn when the non-blocking connect completes
        ++_stats.upstream_conns_opened;
        // Pairs with "upstream reuse": without it an upstream first appears in
        // the log at its second request, never at its birth.
        LB_DEBUG("upstream open ", *u, " to ", up.ip, " pool=", pooled_upstream_count());
        return u;
    }

    bool Gateway::ep_retry_upstream(Connection* u) noexcept
    {
        // Stale pooled connection: reused from the keep-alive pool, not yet retried,
        // and failed before sending any response. The provider almost certainly
        // dropped it idle without processing. Resend the request once on a fresh
        // connection instead of failing the client. (Same rule as the io_uring path.)
        if (!u->from_pool || u->retried || !u->rbuf.empty()) return false;
        if (now_ns() - u->ts_pool_taken > kStaleRetryWindowNs) return false; // may have run
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
        uf->ts_accepted = now_ns(); // a retried connect gets the same deadline
        // make sure the upstream ip did not rotate
        (void)net::resolve_ipv4(up.ip.c_str(), up.port, uf->up_addr);
        client->req.f.upstream_ip = uf->up_addr.sin_addr.s_addr;
        uf->upstream_slot = u->upstream_slot;
        uf->retried = true; // this request's one allowed retry is now spent
        uf->out.stage() = u->out.take(); // plaintext, re-pushed through the new session
        uf->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (up.tls && !tls_attach_upstream(uf))
        {
            ::close(fd);
            delete uf;
            return false;
        }
#endif
        _upconns->add(uf);
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

        ep_close_upstream(u); // discard the dead connection; unpairs it
        pair(client, uf);
        return true;
    }

    void Gateway::ep_release_upstream(Connection* u, bool keep_alive, bool body_ended) noexcept
    {
        if (!pool_upstream(u, keep_alive, body_ended)) { ep_close_upstream(u); return; }
        ep_disarm_write(u);
        // A pooled connection must be readable. A slow client pauses its upstream's
        // reads, and ep_stream_flush's stream_ended branch returns before the resume
        // below it, so an upstream can arrive here with its interest mask at 0, and
        // the next client's response would never be read. Resumed here because this
        // is the only door into the pool; no test provokes that interleaving.
        ep_resume_read(u);
    }

    // close_* defer the free to the end of the epoll batch: an earlier event in
    // the same batch can close a conn that a later event still references via
    // data.ptr (e.g. a peer aborted while its own fd is also ready), so freeing
    // inline would dangle that pointer. Events for doomed conns are skipped in
    // run().
    void Gateway::ep_close_client(Connection* c) noexcept
    {
        if (c->doomed) return;
        // Every close unpairs; an upstream mid-response is of no use to anyone else.
        if (Connection* u = unpair(c)) ep_close_upstream(u);
        _clients->remove(c);
        LB_DEBUG("close ", *c, " clients=", _clients->size());
        if (c->fd >= 0) { ::close(c->fd); c->fd = -1; }
        c->doomed = true;
        _doomed->add(c);
    }

    void Gateway::ep_close_upstream(Connection* u) noexcept
    {
        if (u->doomed) return;
        unpair(u);
        _pool->remove(*u);
        _upconns->remove(u);
        // After the erase, so `pool=` excludes this connection.
        LB_DEBUG("upstream close ", *u, " pool=", pooled_upstream_count());
        if (u->fd >= 0) { ::close(u->fd); u->fd = -1; }
        u->doomed = true;
        _doomed->add(u);
    }

    void Gateway::ep_abort_pair(Connection* client) noexcept
    {
        ep_close_client(client); // and its upstream
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
        LB_WARN(ReqId{client->req.f.req_seq}, " reply ", code, " ", why, " on ", *client,
                peer.empty() ? "" : " peer=", peer);
        // We're replying to the client ourselves, so drop any in-flight upstream.
        if (Connection* u = unpair(client)) ep_close_upstream(u);
        append_error(client->out.stage(), code, detail);
        client->close_after_resp = true; // ep_finish_client closes once it flushes
        ++_stats.errors;
        ep_respond(client);
    }

    void Gateway::ep_on_accept() noexcept
    {
        for (;;)
        {
            int fd = ::accept4(_listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                // The pending connection stays queued, so a level-triggered listener
                // would report it again at once: back off instead of spinning.
                if (errno == EMFILE || errno == ENFILE) ep_pause_accept();
                return;
            }
            net::set_nodelay(fd);
            net::set_nosigpipe(fd);
            if (_client_sndbuf > 0)
                ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &_client_sndbuf, sizeof(_client_sndbuf));
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
            _clients->add(c);
            LB_DEBUG("accept ", *c, " clients=", _clients->size());
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
            const bool in_flight = c->peer != nullptr || !c->out.bytes().empty();
            if (in_flight) ep_abort_pair(c);
            else ep_close_client(c);
            return;
        }
        // Reading continues while a request is in flight, so this is the only bound on
        // what a client can pipeline behind it.
        if (c->rbuf.size() > kMaxClientBuffered)
        {
            LB_WARN("CAP client buffered ", c->rbuf.size(), " bytes on ", *c, "; closing");
            ep_abort_pair(c);
            return;
        }
        // One request in flight at a time per client.
        if (c->peer != nullptr || !c->out.bytes().empty()) return;
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
                    expects_continue(std::string_view(c->rbuf.data(), m.header_len)) &&
                    !send_interim_continue(c, /*uring=*/false))
                    ep_close_client(c);
            }
            return;
        }
        c->client_frame_want = 0;
        // The one per-request reset, before any reply: a framing error's 400 carries
        // its own sequence number and none of the previous request's state. The
        // sequencer is assigned here and not later because every log line below
        // quotes it, and ep_forward erases the request out of rbuf.
        c->req.begin(g_seq.fetch_add(1, std::memory_order_relaxed));
        c->upstream_slot = -1;
        c->req.f.ts_req_recvd = t0;
        if (st == net::http::FrameStatus::Error) { ep_error_respond(c, 400, "request framing"); return; }
        if (!net::http::request_line_ok(c->rbuf)) { ep_error_respond(c, 400, "request line"); return; }

        c->msg = m;
        c->req.f.client_upload_ns = span_since(c->ts_first_byte, t0);
        c->req.f.client_conn_reused = c->ever_framed;
        c->req.f.client_conn_setup_ns = c->req.f.client_conn_reused ? 0 : span_since(c->ts_accepted, t0);
        // A pipelining client's next request is already here, so its arrival begins
        // now; otherwise the next read stamps it.
        c->ts_first_byte = c->rbuf.size() > m.total_len ? t0 : 0;
        // Fail closed on a body we cannot read. Nothing here inflates, and every
        // reader downstream scans bytes assuming JSON: wants_stream, model_of and
        // the translator.
        if (c->msg.encoded) { ep_error_respond(c, 415, "compressed request body"); return; }
        if (_sink) sink_capture(c);
        if (_sink || _policy)
            if (const char* why = capture_model(c))
            {
                ep_error_respond(c, 400, why, kAmbiguousKeysMessage);
                return;
            }
        LB_DEBUG(ReqId{c->req.f.req_seq}, " ", request_line(c->rbuf), " on ", *c);
        // The policy seam, one call site per backend. Here because framing has
        // succeeded but nothing is translated, no credential mapped and no upstream
        // acquired, so a refusal reaches no provider.
        // Default to the first upstream, so a build with no policy, and a policy that
        // only authenticates, both behave exactly as the single-upstream gateway did.
        c->upstream_slot = 0;
        if (_policy)
        {
            const Decision d = policy_decision(c, m);
            if (!d.allow) { ep_error_respond(c, d.deny_status, d.reason, d.message); return; }
            // Out of range is the default, not an error: a policy that does not route
            // leaves this at -1, and one that names a venue that has since left the
            // table must not send the request somewhere arbitrary.
            if (d.upstream_index >= 0 &&
                static_cast<size_t>(d.upstream_index) < _upstreams.size())
                c->upstream_slot = d.upstream_index;
            else if (d.upstream_index >= 0)
                LB_WARN(ReqId{c->req.f.req_seq}, " policy chose upstream ",
                        static_cast<int64_t>(d.upstream_index), " of ",
                        static_cast<int64_t>(_upstreams.size()), "; using 0");
            // Copied: the policy's views die when this returns, and a failover runs
            // from a later event. Refused rather than cut, which would route to
            // whatever model the prefix names.
            if (!c->req.f.model_override.set(d.model) || !c->req.f.tier_override.set(d.service_tier))
            {
                ep_error_respond(c, 500, "policy override longer than the gateway holds");
                return;
            }
        }
        ep_forward(c); // resolves the translation for the chosen venue; see ep_forward
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
            c->req.f.translate_body = plan.translate;
            c->req.f.effective_dialect = plan.venue;
        }
        // Build the bytes to send upstream (translate first, before acquiring an
        // upstream, so a bad body can't leak a pooled connection).
        note_build(c->msg.total_len);
        if (c->req.f.translate_body)
        {
            std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
            // Remember whether the client asked for a final usage chunk. The
            // request bytes are consumed below, but the stream needs it later.
            // `wants_usage` is an output of the translation below, not a separate
            // question: the translator parses this body anyway.
            const std::string_view client_hdrs(c->rbuf.data(), c->msg.header_len);
            const char* why = "";
            // Either failure => 400, and nothing goes upstream.
            if (!build_translated_request(up, c->req.f.effective_dialect, body, client_hdrs, _strip_headers,
                                          _rebuild, _xlate, why, c->req.f.model_override.view(),
                                          &c->req.f.wants_usage))
            {
                if (why[0] == 't')
                    ep_error_respond(c, 400, "translate", translate_failure(body));
                // The caller's own header, so the caller is told. The value is
                // never echoed: it is a credential, and this string lands in a JSON
                // body with no escaping.
                else if (why[0] == 'b')
                    ep_error_respond(c, 400, "malformed credential",
                                     refuse::kBedrockCredential);
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
            std::string_view rewritten;
            if (!c->req.f.model_override.empty() || !c->req.f.tier_override.empty())
            {
                std::string_view had;
                if (!provider::apply_overrides(
                    std::string_view(c->rbuf.data() + c->msg.header_len, c->msg.body_len),
                    c->req.f.model_override.view(), c->req.f.tier_override.view(), &had, _xlate))
                { ep_error_respond(c, 400, "cannot apply route overrides"); return; }
                rewritten = _xlate;
                // Copied, not held: `had` points into the request buffer, which is
                // reused before the sink runs. Truncated and not refused, because a
                // tier too long to be one is still worth reporting.
                c->req.f.asked_tier.set(had);
            }
            // Refused, not repaired: a venue with a base path needs an origin-form
            // target to prefix, and nothing has been sent upstream at this point.
            if (!request_without(std::string_view(c->rbuf.data(), c->msg.total_len),
                                 c->msg.header_len, _strip_headers, up.host_hdr,
                                 up.base_path, rewritten, _rebuild))
            { ep_error_respond(c, 400, "request target not origin-form"); return; }
        }

        Connection* u = ep_acquire_upstream(c->upstream_slot);
        if (!u)
        {
            secure_clear(_rebuild); // a credential must not wait in the scratch for the next request
            // A failover attempt runs on a copy put back in rbuf; failover_req keeps the
            // original, so drop the copy or the next failover inserts a second one.
            if (!c->req.saved.empty()) c->rbuf.erase(0, c->msg.total_len);
            if (!ep_upstream_failed(c, 502, "no upstream (connect failed)"))
                ep_error_respond(c, 502, "no upstream (connect failed)");
            return;
        }

        LB_DEBUG(ReqId{c->req.f.req_seq}, " upstream ", *u, " pooled=", u->from_pool,
                 " dest=", upstream_of(u).ip, ":", upstream_of(u).port);
        // The rebuilt request is already in `_rebuild`; the swap hands it over and
        // takes the upstream's scrubbed, still-allocated buffer back as the next scratch.
        u->out.stage().swap(_rebuild);
        // Keep the original bytes when a failover could use them: the rebuilt request
        // above was translated for this venue's dialect, so it cannot be resent to a
        // different one. One copy per in-flight request, and only where it can pay off.
        if (_policy && _upstreams.size() > 1 && c->req.saved.empty())
        {
            const size_t extra = c->rbuf.size() - c->msg.total_len;
            c->req.saved.swap(c->rbuf);
            c->rbuf.assign(c->req.saved, c->msg.total_len, extra);
            c->req.saved.resize(c->msg.total_len);
        }
        else c->rbuf.erase(0, c->msg.total_len);
        pair(c, u);
        c->ever_framed = true;        // past the setup deadline for good
        c->ts_client_activity = now_ns(); // and the idle clock restarts here
        c->req.f.ts_req_built = now_ns();   // end of our request-side work
        c->req.f.ts_progress = c->req.f.ts_req_built; // idle-timeout baseline for this request
        // Per attempt: a failover's venue is not the last one's. Pooled means no handshake.
        c->req.f.ts_wire_ready = u->connected ? c->req.f.ts_req_built : 0;
        c->req.f.ts_up_sent = 0;
        c->req.f.upstream_pooled = u->connected;
        c->req.f.upstream_ip = u->up_addr.sin_addr.s_addr;

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
                tls_push_out(u);
                bool done = false;
                if (!ep_tls_flush(u, &done))
                {
                    if (!ep_retry_upstream(u) && !ep_upstream_failed(c, 502, "upstream write failed")) ep_error_respond(c, 502, "upstream write failed, retry exhausted");
                    return;
                }
                if (done && tls_out_flushed(u)) c->req.f.ts_up_sent = now_ns();
                return;
            }
#endif
            bool done = false;
            if (!ep_pump_write(u, &done)) { if (!ep_retry_upstream(u) && !ep_upstream_failed(c, 502, "upstream write failed")) ep_error_respond(c, 502, "upstream write failed, retry exhausted"); return; }
            if (done) c->req.f.ts_up_sent = now_ns(); // request fully sent (end of request path)
            else ep_arm_write(u);               // socket full; finish on writability
        }
        else
        {
            ep_arm_write(u); // connect pending; ep_on_upstream_writable sends once it completes
        }
    }

    void Gateway::ep_on_upstream_writable(Connection* u) noexcept
    {
        const uint64_t sent = u->sent_bytes;
        if (!u->connected)
        {
            int err = net::connect_result(u->fd);
            if (err != 0)
            {
                note_connect_failure(u->upstream_slot, std::strerror(err));
                Connection* client = unpair(u);
                ep_close_upstream(u);
                if (client) { if (!ep_upstream_failed(client, 502, "upstream connect refused")) ep_error_respond(client, 502, "upstream connect refused"); }
                else ++_stats.errors;
                return;
            }
            u->connected = true;
            // t2 for a plaintext upstream: the socket can carry the request now.
            // A TLS upstream is not wire-ready yet; tls_feed() stamps t2 when the
            // handshake completes.
            if (!upstream_is_tls(u))
            {
                u->wire_ready = true;
                if (u->peer) u->peer->req.f.ts_wire_ready = now_ns();
            }
#ifdef LLMBRIDGE_HAVE_TLS
            if (u->tls && !u->tls->handshake_done())
                u->tls->start_handshake(); // ClientHello lands in the write BIO
#endif
        }
#ifdef LLMBRIDGE_HAVE_TLS
        if (u->tls)
        {
            const bool was_flushed = u->peer && tls_out_flushed(u);
            bool tdone = false;
            if (!ep_tls_flush(u, &tdone))
            {
                if (u->peer) { if (!ep_retry_upstream(u) && !ep_upstream_failed(u->peer, 502, "upstream write failed")) ep_error_respond(u->peer, 502, "upstream write failed, retry exhausted"); }
                else ep_close_upstream(u);
                return;
            }
            if (u->peer && u->sent_bytes != sent) u->peer->req.f.ts_progress = now_ns();
            if (!tdone) return; // EPOLLOUT re-armed by ep_tls_flush
            ep_disarm_write(u); // ciphertext drained; handshake replies arrive via read
            if (u->peer && !was_flushed && tls_out_flushed(u))
                u->peer->req.f.ts_up_sent = now_ns();
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
        if (u->peer && u->sent_bytes != sent) u->peer->req.f.ts_progress = now_ns();
        if (!done) { ep_arm_write(u); return; }
        ep_disarm_write(u);
        if (u->peer) u->peer->req.f.ts_up_sent = now_ns(); // end of request-path work
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
            u->peer_eof = true; // never pooled now, whatever finishes the exchange
#ifdef LLMBRIDGE_HAVE_TLS
            // TLS parity with the io_uring path: a fatal session error (bad record,
            // MAC failure) mid-stream must abort the client, never finalize the
            // stream as clean: a corrupted stream that ends in a well-formed
            // [DONE] would hide the corruption from the client entirely. Only a
            // real transport EOF may end a close-delimited stream normally.
            if (u->tls && u->tls->want() == net::tls::Want::Error && client && client->req.f.streaming)
            {
                ep_abort_pair(client);
                return;
            }
#endif
            if (client && client->req.f.streaming) { ep_stream_on_upstream_eof(u); return; }
            if (client == nullptr) ep_close_upstream(u); // idle pooled conn dropped (eviction)
            else if (!ep_retry_upstream(u) && !ep_upstream_failed(client, 502, "upstream EOF")) ep_error_respond(client, 502, "upstream EOF, retry exhausted");
            return;
        }
        // Stray bytes on an idle pooled upstream. They are the tail of an exchange
        // that is over, and ep_acquire_upstream does not clear rbuf, so leaving them
        // hands one client's bytes to the next as the head of its response. The
        // io_uring twin already closes here; this side kept the connection.
        if (client == nullptr) { ep_close_upstream(u); return; }
        client->req.f.ts_progress = now_ns(); // upstream made progress

        // Mid-stream: pump the newly-arrived body bytes and return.
        if (client->req.f.streaming) { ep_stream_pump(u); return; }

        // First response bytes: for the Anthropic translate path, peek the head to
        // decide whole-body vs streaming (text/event-stream). Other modes and
        // non-streaming responses fall through to the whole-body path unchanged.
        // Anthropic (translated) or none (byte-forward). Gemini and Cohere are
        // non-streaming here, so a stream from one of those would be forwarded in a
        // dialect the client cannot read; they keep falling through to the whole-body
        // path until their translators exist.
        if (client->req.f.effective_dialect == UpstreamDialect::Anthropic ||
            client->req.f.effective_dialect == UpstreamDialect::Azure ||
            !client->req.f.translate_body)
        {
            net::http::ResponseHead h;
            const auto hs = net::http::parse_response_head(u->rbuf, h, sent_head(*client));
            if (hs == net::http::FrameStatus::NeedMore) return;
            if (hs == net::http::FrameStatus::Error) { ep_error_respond(client, 502, "upstream response head framing"); return; }
            // Only a 200 carries a real event stream. A provider error (429 rate
            // limit, 529 overloaded, 400 context length, 401 auth) must reach the
            // client with its status (relayed below once the body is framed)
            // never laundered into a 200 stream.
            if (h.event_stream && h.status == 200 && h.body != net::http::Body::None)
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
                client->req.f.ts_up_recvd = now_ns();
                note_quota(client, h);
                note_venue_req_id(client, std::string_view(u->rbuf.data(), h.header_len));
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
        const auto r = net::http::parse_response(u->rbuf, u->rdec, sent_head(*client));
        if (r.failed()) { ep_error_respond(client, 502, "upstream response framing"); return; }
        if (!r.complete()) return;
        const net::http::ResponseHead& h = r.head;
        const std::string_view body_buf = r.body;
        const size_t total_len = r.total_len;

        client->req.f.ts_up_recvd = t0; // end of upstream wait (stamped pre-framing)
        note_quota(client, h);
        note_venue_req_id(client, std::string_view(u->rbuf.data(), h.header_len));
        note_upstream_error(client, h, body_buf);
        note_served_tier(client, body_buf, /*tail=*/true);
        note_served_model(client, body_buf, /*streamed=*/false);

        if (client->req.f.translate_body)
        {
            const std::string_view body = body_buf;
            // Relay a provider failure with its own status + message (rate limit,
            // overloaded GPU, context length, auth); translating a non-200 body as
            // if it were a completion would fail and mask it as a generic 502.
            if (h.status != 0 && h.status != 200)
            {
                append_http_status(client->out.stage(), h.status, reason_for(h.status),
                                   provider::upstream_error_to_openai(body, "upstream_error"));
                unpair(u);
                u->rbuf.erase(0, total_len);
                ep_release_upstream(u, h.keep_alive, true);
                ++_stats.errors;
                ep_respond(client);
                return;
            }
            if (!xlate_resp(client->req.f.effective_dialect, body, client->xlate_scratch, client->req.f.tok))
            {
                unpair(u);
                u->rbuf.erase(0, total_len); // framing was valid; the upstream conn is reusable
                ep_release_upstream(u, h.keep_alive, true);
                ep_error_respond(client, 502, "response translate");
                return;
            }
            std::string timing;
            if (_timing_headers)
            {
                // t5: the response is built here and written immediately after.
                const int64_t ts_resp_built = now_ns();
                const TimingSplit sp = timing_split(
                    client->req.f.ts_req_recvd, client->req.f.ts_req_built, client->req.f.ts_wire_ready,
                    client->req.f.ts_up_sent, client->req.f.ts_up_recvd, ts_resp_built);
                append_timing_headers(timing, client->req.f.ts_req_recvd, sp.compute_ns / 1000,
                                      sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                      sp.upstream_ns / 1000, "x-llmbridge-upstream-us",
                                      client->req.f.req_seq, client->req.f.client_upload_ns / 1000);
                append_usage_headers(timing, client->req.f.tok);
            }
            append_http(client->out.stage(), "HTTP/1.1 200 OK", client->xlate_scratch, timing);
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
            if (h.body == net::http::Body::Chunked)
                append_http_status(client->out.stage(), h.status ? h.status : 200,
                                   reason_for(h.status ? h.status : 200), body_buf);
            else client->out.stage().append(u->rbuf.data(), total_len);
            // The counts, from the venue's own body. Only the translated branch above
            // scanned, so a byte-forward reported nothing: a sink saw -1 and a tape
            // recorded a request that cost zero tokens at a real price.
            client->req.f.tok = scan_usage(body_buf);
        }

        // Response fully read -> upstream is free. Pool it only if it will stay
        // open (response keep-alive, and for passthrough the client didn't ask to
        // close); otherwise it's about to close, so drop it instead of reuse a
        // stale connection.
        const bool keep_alive =
            h.keep_alive && (client->req.f.translate_body || client->msg.keep_alive);
        unpair(u);
        // Drop the framed message; anything left is a second response and keeps the
        // connection out of the pool.
        u->rbuf.erase(0, total_len);
        ep_release_upstream(u, keep_alive, true);
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
        if (c->req.f.streaming) { ep_stream_flush(c); return; } // pump path has its own drain logic
        bool done = false;
        if (!ep_pump_write(c, &done)) { ep_abort_pair(c); return; }
        if (!done) { ep_arm_write(c); return; }
        ep_finish_client(c);
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
                const TimingSplit sp = timing_split(c->req.f.ts_req_recvd, c->req.f.ts_req_built,
                                                    c->req.f.ts_wire_ready, c->req.f.ts_up_sent,
                                                    c->req.f.ts_up_recvd, c->req.f.ts_up_recvd);
                const int64_t conn_ns = sp.connect_ns;
                const int64_t req_ns = sp.req_path_ns;
                const int64_t resp_ns = ts_resp_sent - c->req.f.ts_up_recvd;
                if (req_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(req_ns));
                if (conn_ns >= 0) _stats.connect.record(static_cast<uint64_t>(conn_ns));
                if (resp_ns >= 0) _stats.resp_path.record(static_cast<uint64_t>(resp_ns));
                if (req_ns >= 0 && resp_ns >= 0)
                    _stats.overhead.record(static_cast<uint64_t>(req_ns + resp_ns));
                ++_stats.requests;
            }
        }
        ep_disarm_write(c);
        if (_sink) sink_emit(c, status_of(c->out.bytes()), /*streamed=*/false);
        LB_DEBUG(ReqId{c->req.f.req_seq}, " status=", status_of(c->out.bytes()),
                 " bytes=", c->out.bytes().size(), " keep_alive=", c->msg.keep_alive, " on ", *c);
        c->out.clear();
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
        client->req.f.streaming = true;
        client->req.f.stream_chunked = h.chunked;
        client->req.f.stream_keep_alive = h.keep_alive; // decides poolability at stream end
        stream_warn_if_encoded(client, h);
        // Only a request that needs translating gets a translator.
        client->req.f.sse_translating =
            client->req.f.translate_body && client->req.f.effective_dialect == UpstreamDialect::Anthropic;
        if (client->req.f.sse_translating) client->sse_xlate.reset(-1, client->req.f.wants_usage);
        // Chosen once, before either head is built, and remembered.
        client->req.f.stream_chunked_out = stream_reusable_out(client);
        if (_timing_headers)
        {
            // t4 = provider's first response byte, stamped by the caller.
            std::string timing;
            // t5 = t4: a stream's response is not built at one instant, so the
            // compute leg is the request side alone instead of an invented figure.
            const TimingSplit sp = timing_split(
                client->req.f.ts_req_recvd, client->req.f.ts_req_built, client->req.f.ts_wire_ready,
                client->req.f.ts_up_sent, client->req.f.ts_up_recvd, client->req.f.ts_up_recvd);
            append_timing_headers(timing, client->req.f.ts_req_recvd, sp.compute_ns / 1000,
                                  sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                  sp.upstream_ns / 1000, "x-llmbridge-upstream-ttfb-us",
                                  client->req.f.req_seq, client->req.f.client_upload_ns / 1000);
            append_sse_head(client->out.stage(), timing,
                            client->req.f.stream_chunked_out ? kSseHeadChunked : kSseHead);
        }
        else
            client->out.stage().append(client->req.f.stream_chunked_out ? kSseHeadChunked : kSseHead);

        u->rbuf.erase(0, h.header_len); // consume the head; the rest is body
        ep_stream_pump(u);                 // translate any initial body + flush headers
    }

    // Decode + translate the upstream body bytes now sitting in u->rbuf, appending
    // OpenAI SSE to the client's write buffer, then flush.
    void Gateway::ep_stream_pump(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { ep_close_upstream(u); return; } // lost peer mid-stream

        const StreamStep st = stream_step(client, u->rbuf, client->out.stage(), /*at_eof=*/false);
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

        const StreamStep st = stream_step(client, u->rbuf, client->out.stage(), /*at_eof=*/true);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            stream_truncate(client);
        }
        // Either way the stream has ended, so the upstream is done: close it now. Kept
        // while reads were paused, its dead fd raised EPOLLHUP on every wait.
        ep_close_upstream(u);
        ep_stream_flush(client);
    }

    // Write buffered SSE to the client. If it doesn't all go, finish on writability
    // and pause upstream reads (backpressure). On full flush, resume upstream, or
    // finalize if the stream has ended.
    void Gateway::ep_stream_flush(Connection* client) noexcept
    {
        const uint64_t sent = client->sent_bytes;
        bool done = false;
        if (!ep_pump_write(client, &done)) { ep_abort_pair(client); return; } // client gone
        // A client draining a paused stream is progress, though the upstream is silent.
        if (client->sent_bytes != sent) client->req.f.ts_progress = now_ns();
        if (!done)
        {
            ep_arm_write(client);
            if (client->peer && !client->peer->doomed) ep_pause_read(client->peer);
            return;
        }
        client->out.clear();
        ep_disarm_write(client);
        if (client->req.f.stream_ended) { ep_finalize_stream(client); return; }
        if (client->peer && !client->peer->doomed) ep_resume_read(client->peer);
    }

    void Gateway::ep_finalize_stream(Connection* client) noexcept
    {
        // Reuse is the whole point: a streaming request otherwise costs a fresh
        // upstream connect every time, which measured as the dominant term in
        // time-to-first-token. Pool it when the framing says that is safe.
        if (Connection* u = unpair(client))
            ep_release_upstream(u, stream_keeps_upstream(client), stream_body_ended(client));
        // A stream never reaches the non-streaming completion log, so without this a
        // streamed request logged its start and then nothing at all: no outcome, no
        // size, no tokens. `close_after_resp` here means the stream was truncated
        // (stream_truncate emits no [DONE] on purpose), which is the one outcome a
        // client cannot distinguish from a clean finish by itself.
        LB_DEBUG(ReqId{client->req.f.req_seq}, " stream ended ",
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
        client->req.f.streaming = false; // over; begin() resets the rest at the next framing
        client->out.clear();
        client->msg = net::http::Message{};
        ep_on_client_readable(client); // a pipelined next request is already in rbuf
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
            sweep_idle(); // heartbeat, pool reaping and every timeout
            if (_accept_resume_ns && now_ns() >= _accept_resume_ns) ep_resume_accept();
            _doomed->clear([this](Connection* d) {
                retire_wbuf(d);
                delete d;
            });
        }
        return 0;
    }
} // namespace llmbridge
