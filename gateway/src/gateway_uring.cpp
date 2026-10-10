// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The io_uring half of Gateway: every ur_ method and run_uring(). Its twin is
// gateway_epoll.cpp; the shared methods are in gateway.cpp. Empty without
// LLMBRIDGE_HAVE_URING, the same guard net/uring.hpp carries.

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
#include "net/uring.hpp"

#ifdef LLMBRIDGE_HAVE_URING
namespace llmbridge
{
    using namespace detail;

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

    bool Gateway::ur_submit_accept() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return false;
        s->opcode = IORING_OP_ACCEPT;
        s->fd = _listen_fd;
        s->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
        s->ioprio = IORING_ACCEPT_MULTISHOT; // one SQE, a completion per accepted fd
        s->user_data = make_ud(_listen_conn, UAccept);
        return true;
    }

    // The timer tick re-arms the accept. Without it, an EMFILE ends the multishot and an
    // immediate re-arm fails again at once, and a failed re-arm left the listener dead.
    void Gateway::ur_pause_accept(const char* why) noexcept
    {
        _accept_resume_ns = now_ns() + kAcceptBackoffNs;
        ++_stats.accept_backoffs;
        LB_WARN("CAP ", why, "; accepting paused for 100 ms, total=", _stats.accept_backoffs);
    }

    bool Gateway::ur_submit_timer() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return false;
        s->opcode = IORING_OP_TIMEOUT;
        s->addr = reinterpret_cast<uint64_t>(&_uring_ts);
        s->len = 1;
        s->user_data = UTimer; // conn = nullptr
        return true;
    }

    // The SQ is full even after a flush, so this connection cannot make progress: end it
    // and its peer together. Closing only `c` left the peer pointing at a freed object.
    void Gateway::ur_drop_sq_full(Connection* c) noexcept
    {
        LB_WARN("CAP io_uring submission queue full; closing ", *c);
        Connection* client = c->is_client ? c : c->peer;
        if (client) ur_abort_pair(client);
        else ur_close(c);
    }

    bool Gateway::ur_arm_recv(Connection* c) noexcept
    {
        // Multishot recv drawing from the provided-buffer pool: one submission keeps
        // delivering a completion per data arrival (each naming the buffer it used),
        // so we never re-submit a recv per read. Armed once per connection; only
        // re-armed if the kernel ends the multishot (e.g. pool exhaustion).
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) { ur_drop_sq_full(c); return false; }
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
        if (!tls_invariant_ok(c)) { ur_drop_sq_full(c); return false; } // never plaintext
#endif
        if (!ur_next_sqe(&s)) { ur_drop_sq_full(c); return false; }
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
        s->addr = reinterpret_cast<uint64_t>(&u->up_addr);
        s->off = sizeof(u->up_addr); // connect addrlen rides in `off`
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
        s->cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL; // a recv and a send
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

#endif // LLMBRIDGE_HAVE_TLS

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

        LB_WARN(ReqId{client->req.f.req_seq}, " venue ", static_cast<int64_t>(client->upstream_slot),
                " failed (", why, "); retrying on ", static_cast<int64_t>(r.upstream_index));
        if (Connection* u = unpair(client)) ur_close(u);
        ++client->req.f.failover_attempts;
        client->upstream_slot = r.upstream_index;
        client->rbuf.insert(0, client->req.saved); // see the epoll mirror
        ++_stats.upstream_failovers;
        ur_forward(client);
        return true;
    }

    Connection* Gateway::ur_acquire_upstream(int slot) noexcept
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
        const int fd = net::make_client_socket();
        if (fd < 0) return nullptr;
        Connection* u = new Connection();
        u->fd = fd;
        u->is_client = false;
        u->connected = false;
        u->from_pool = false;
        u->upstream_slot = slot; // release() indexes the pool with this
        u->ts_accepted = now_ns(); // the connect deadline counts from here
        // make sure the upstream ip did not rotate
        if (!net::resolve_ipv4(up.ip.c_str(), up.port, u->up_addr))
        {
            ::close(fd);
            delete u;
            return nullptr;
        }
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
        ++_stats.upstream_conns_opened;
        LB_DEBUG("upstream open ", *u, " to ", up.ip, " pool=", pooled_upstream_count()); // see ep_ twin
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
        if (now_ns() - u->ts_pool_taken > kStaleRetryWindowNs) return false; // see the epoll mirror
        Connection* client = u->peer;
        if (!client) return false;
        const Upstream& up = upstream_of(u);
        const int fd = net::make_client_socket();
        if (fd < 0) return false;

        Connection* uf = new Connection();
        uf->fd = fd;
        uf->is_client = false;
        uf->connected = false;
        uf->from_pool = false;
        uf->ts_accepted = now_ns();
        // make sure the upstream ip did not rotate
        if (!net::resolve_ipv4(up.ip.c_str(), up.port, uf->up_addr))
        {
            ::close(fd);
            delete uf;
            return false;
        }
        client->req.f.upstream_ip = uf->up_addr.sin_addr.s_addr;
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
        _upconns->add(uf);
        ++_stats.upstream_conns_opened;

        // WARN, not DEBUG: it is not a cap, it is a recovered failure. A pooled
        // connection was dead when we used it, and the client never learns. That is
        // the point of the retry and also the reason it must be visible: a rising
        // rate here means the provider is dropping keep-alives faster than
        // --pool-idle reaps them, which is a tuning signal nobody can see otherwise.
        LB_WARN("upstream stale, resending on a fresh connection ", *u);
        ur_close(u); // discard the dead pooled connection; unpairs it
        pair(client, uf);
        ++_stats.upstream_retries;
        ur_submit_connect(uf); // connect fresh, then send on completion
        return true;
    }

    void Gateway::ur_release_upstream(Connection* u, bool keep_alive, bool body_ended) noexcept
    {
        if (!pool_upstream(u, keep_alive, body_ended)) ur_close(u); // see the epoll mirror
    }

    void Gateway::ur_close(Connection* c) noexcept
    {
        if (c->doomed) return;
        // Every close unpairs: the other leg never keeps a pointer to a freed object.
        Connection* other = unpair(c);
        if (c->is_client)
        {
            _clients->remove(c);
            LB_DEBUG("close ", *c, " clients=", _clients->size());
        }
        else
        {
            // This backend closes both kinds in one function where epoll has two, so
            // the upstream branch lives here. After the erase, as in the ep_ twin.
            _pool->remove(*c);
            _upconns->remove(c);
            LB_DEBUG("upstream close ", *c, " pool=", pooled_upstream_count());
        }
        // Force any in-flight op on this fd to complete so its inflight count drains
        // (the fd is closed at free time). shutdown alone does not end an armed
        // multishot recv, so also cancel everything on the fd.
        if (c->fd >= 0) { ::shutdown(c->fd, SHUT_RDWR); ur_submit_cancel(c->fd); }
        c->doomed = true;
        _doomed->add(c);
        // An upstream mid-response is of no use to anyone else.
        if (other && c->is_client) ur_close(other);
        ur_maybe_free(c);
    }

    void Gateway::ur_abort_pair(Connection* client) noexcept
    {
        if (!client) return;
        ur_close(client); // and its upstream
        ++_stats.errors;
    }

    void Gateway::ur_error_respond(Connection* client, int code, const char* why,
                                   const char* detail) noexcept
    {
        if (!client || client->doomed) return;
        // An authentication refusal names where it came from.
        const std::string peer = (code == 401 || code == 403) ? peer_of(client->fd)
                                                              : std::string{};
        LB_WARN(ReqId{client->req.f.req_seq}, " reply ", code, " ", why, " on ", *client,
                peer.empty() ? "" : " peer=", peer);
        // A send in flight is reading wbuf: replacing it would hand the kernel freed
        // memory. Whatever the client was sent, it now gets a close.
        if (client->send_inflight) { ur_abort_pair(client); return; }
        if (Connection* u = unpair(client)) ur_close(u);
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
        _doomed->remove(c);
        retire_wbuf(c);
        delete c;
    }

    void Gateway::ur_on_cqe(uint64_t user_data, int res, uint32_t flags) noexcept
    {
        const UOp op = ud_op(user_data);
        if (op == UTimer)
        {
            if (!_draining && !_stop)
            {
                sweep_idle();
                if (_accept_resume_ns && now_ns() >= _accept_resume_ns)
                {
                    _accept_resume_ns = 0;
                    if (!ur_submit_accept()) ur_pause_accept("accept re-arm found the SQ full");
                }
                if (!ur_submit_timer()) _uring_timer_lost = true;
            }
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
        if (!(flags & IORING_CQE_F_MORE)) // multishot ended -> re-arm, unless that would spin
        {
            if (res == -EMFILE || res == -ENFILE) ur_pause_accept("out of file descriptors");
            else if (!ur_submit_accept()) ur_pause_accept("accept re-arm found the SQ full");
        }
        if (res < 0) return;                                 // transient accept error
        const int fd = res;
        net::set_nodelay(fd);
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
            // Fail closed, exactly as the epoll twin does. No recv is armed yet, so
            // there is nothing in flight and the Connection can be freed directly
            // instead of going on the doomed list.
            ::close(fd);
            delete c;
            ++_stats.errors;
            return;
        }
#endif
        _clients->add(c);
        LB_DEBUG("accept ", *c, " clients=", _clients->size());
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
            if (!c->is_client) c->peer_eof = true; // its recv is over: never pooled now
            if (c->is_client) { if (c->peer) ur_abort_pair(c); else ur_close(c); }
            else if (c->peer && c->peer->req.f.streaming) ur_stream_on_upstream_eof(c); // stream end, not a failure
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
        // Kernel ended the multishot (pool pressure): re-arm. A failed arm may free c.
        if (!armed && !ur_arm_recv(c)) return;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_ok)
        {
            // Fatal TLS failure (bad record, MAC failure, refused certificate).
            // Deliberately not ur_stream_on_upstream_eof for a mid-stream failure: a
            // corrupted stream must not be finalized as if it ended cleanly.
            if (c->peer && c->peer->req.f.streaming) { ur_abort_pair(c->peer); return; }
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
            if (c->rbuf.size() > kMaxClientBuffered) // see the epoll mirror
            {
                LB_WARN("CAP client buffered ", c->rbuf.size(), " bytes on ", *c, "; closing");
                ur_abort_pair(c);
                return;
            }
            ur_try_forward_buffered(c); // forward a framed request iff the client is idle
        }
        else
        {
            // Upstream bytes are the response to the in-flight request. Stray data on
            // an idle pooled upstream (no peer) means it's unusable, so drop it.
            if (!c->peer) { ur_close(c); return; }
            c->peer->req.f.ts_up_activity = now_ns(); // upstream made progress

            // Mid-stream: pump the newly-arrived body bytes and return.
            if (c->peer->req.f.streaming) { ur_stream_pump(c); return; }

            // First response bytes: peek the head (parse_response_head tolerates
            // chunked, unlike parse_request()); a text/event-stream response enters the
            // streaming pump, everything else the whole-body path below.
            // See the epoll mirror: translated or byte-forward, never Gemini/Cohere.
            if (c->peer->req.f.effective_dialect == UpstreamDialect::Anthropic ||
                c->peer->req.f.effective_dialect == UpstreamDialect::Azure ||
                !c->peer->req.f.translate_body)
            {
                net::http::ResponseHead h;
                const auto hs = net::http::parse_response_head(c->rbuf, h, sent_head(*c->peer));
                if (hs == net::http::FrameStatus::NeedMore) return; // wait for the full head
                if (hs == net::http::FrameStatus::Error)
                { ur_error_respond(c->peer, 502, "upstream response head framing"); return; }
                // Only a 200 is a real stream; a provider error is relayed with its
                // own status by ur_on_response below (never laundered into a 200).
                if (h.event_stream && h.status == 200 && h.body != net::http::Body::None)
                {
                    c->peer->req.f.ts_up_recvd = now_ns(); // t4: head complete, see the epoll mirror
                    note_quota(c->peer, h);
                    note_venue_req_id(c->peer, std::string_view(c->rbuf.data(), h.header_len));
                    ur_begin_stream(c, h);
                    return;
                }
            }

            // parse_response, not parse: providers return non-streaming bodies
            // chunked over HTTP/1.1 and parse_request() rejects that by design (http.hpp).
            const auto r = net::http::parse_response(c->rbuf, c->rdec, sent_head(*c->peer));
            if (r.failed()) { ur_error_respond(c->peer, 502, "upstream response framing"); return; }
            if (!r.complete()) return; // armed recv delivers the rest
            c->peer->req.f.ts_up_recvd = now_ns();
            note_quota(c->peer, r.head);
            note_venue_req_id(c->peer, std::string_view(c->rbuf.data(), r.head.header_len));
            note_upstream_error(c->peer, r.head, r.body);
            note_served_tier(c->peer, r.body, /*tail=*/true);
            note_served_model(c->peer, r.body, /*streamed=*/false);
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
                    expects_continue(std::string_view(c->rbuf.data(), m.header_len)) &&
                    !send_interim_continue(c, /*uring=*/true))
                    ur_close(c);
            }
            return; // the armed recv will deliver more
        }
        c->client_frame_want = 0;
        // The one per-request reset, before any reply; see the epoll mirror.
        c->req.begin(g_seq.fetch_add(1, std::memory_order_relaxed));
        c->upstream_slot = -1;
        c->req.f.ts_req_recvd = now_ns();
        if (st == net::http::FrameStatus::Error) { ur_error_respond(c, 400, "request framing"); return; }
        if (!net::http::request_line_ok(c->rbuf)) { ur_error_respond(c, 400, "request line"); return; }
        c->msg = m;
        c->req.f.client_upload_ns = span_since(c->ts_first_byte, c->req.f.ts_req_recvd);
        c->req.f.client_conn_reused = c->ever_framed;
        c->req.f.client_conn_setup_ns = c->req.f.client_conn_reused ? 0 : span_since(c->ts_accepted, c->req.f.ts_req_recvd);
        c->ts_first_byte = c->rbuf.size() > m.total_len ? c->req.f.ts_req_recvd : 0; // see epoll mirror
        // Fail closed on a body we cannot read. Nothing here inflates, and every
        // reader downstream scans bytes assuming JSON: wants_stream, model_of and
        // the translator.
        if (c->msg.encoded) { ur_error_respond(c, 415, "compressed request body"); return; }
        if (_sink) sink_capture(c);
        if (_sink || _policy)
            if (const char* why = capture_model(c))
            {
                ur_error_respond(c, 400, why, kAmbiguousKeysMessage);
                return;
            }
        LB_DEBUG(ReqId{c->req.f.req_seq}, " ", request_line(c->rbuf), " on ", *c);
        // The policy seam; see the epoll mirror for why it sits exactly here.
        // Default to the first upstream, so a build with no policy, and a policy that
        // only authenticates, both behave exactly as the single-upstream gateway did.
        c->upstream_slot = 0;
        if (_policy)
        {
            const Decision d = policy_decision(c, m);
            if (!d.allow) { ur_error_respond(c, d.deny_status, d.reason, d.message); return; }
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
            // Copied: see the epoll mirror.
            if (!c->req.f.model_override.set(d.model) || !c->req.f.tier_override.set(d.service_tier))
            {
                ur_error_respond(c, 500, "policy override longer than the gateway holds");
                return;
            }
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
            c->req.f.translate_body = plan.translate;
            c->req.f.effective_dialect = plan.venue;
        }
        note_build(c->msg.total_len);
        if (c->req.f.translate_body)
        {
            std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
            const std::string_view client_hdrs(c->rbuf.data(), c->msg.header_len);
            const char* why = "";
            // Either failure => 400, and nothing goes upstream.
            // (The credential branch called the EPOLL responder until the ep_/ur_
            // split. It was harmless by three accidents, and the prefixes turned it
            // into a grep; folding both loops onto one builder removes the chance of
            // the next such divergence entirely.)
            if (!build_translated_request(up, c->req.f.effective_dialect, body, client_hdrs, _strip_headers,
                                          _rebuild, _xlate, why, c->req.f.model_override.view(),
                                          &c->req.f.wants_usage))
            {
                if (why[0] == 't')
                    ur_error_respond(c, 400, "translate", translate_failure(body));
                else if (why[0] == 'b')
                    ur_error_respond(c, 400, "malformed credential",
                                     refuse::kBedrockCredential);
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
            std::string_view rewritten;
            if (!c->req.f.model_override.empty() || !c->req.f.tier_override.empty())
            {
                std::string_view had;
                if (!provider::apply_overrides(
                    std::string_view(c->rbuf.data() + c->msg.header_len, c->msg.body_len),
                    c->req.f.model_override.view(), c->req.f.tier_override.view(), &had, _xlate))
                { ur_error_respond(c, 400, "cannot apply route overrides"); return; }
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
            { ur_error_respond(c, 400, "request target not origin-form"); return; }
        }

        Connection* u = ur_acquire_upstream(c->upstream_slot);
        if (!u)
        {
            secure_clear(_rebuild); // a credential must not wait in the scratch for the next request
            if (!c->req.saved.empty()) c->rbuf.erase(0, c->msg.total_len); // see the epoll mirror
            if (!ur_upstream_failed(c, 502, "no upstream (connect failed)"))
                ur_error_respond(c, 502, "no upstream (connect failed)");
            return;
        }

        LB_DEBUG(ReqId{c->req.f.req_seq}, " upstream ", *u, " pooled=", u->from_pool,
                 " dest=", upstream_of(u).ip, ":", upstream_of(u).port);
        // The rebuilt request is already in `_rebuild`; the swap hands it over and
        // takes the upstream's scrubbed, still-allocated buffer back as the next scratch.
        u->wbuf.swap(_rebuild);
        u->woff = 0;
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
        c->req.f.ts_up_activity = c->req.f.ts_req_built; // idle-timeout baseline for this request
        // Per attempt: a failover's venue is not the last one's. Pooled means no handshake.
        c->req.f.ts_wire_ready = u->connected ? c->req.f.ts_req_built : 0;
        c->req.f.ts_up_sent = 0;
        c->req.f.upstream_pooled = u->connected;
        c->req.f.upstream_ip = u->up_addr.sin_addr.s_addr;

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
            note_connect_failure(u->upstream_slot, std::strerror(-res));
            Connection* cl = unpair(u);
            ur_close(u);
            if (cl) { if (!ur_upstream_failed(cl, 502, "upstream connect refused")) ur_error_respond(cl, 502, "upstream connect refused"); }
            else ++_stats.errors;
            return;
        }
        u->connected = true;
        // t2 for a plaintext upstream only; see the epoll twin and tls_feed().
        if (!upstream_is_tls(u))
        {
            u->wire_ready = true;
            if (u->peer) u->peer->req.f.ts_wire_ready = now_ns();
        }
        if (!ur_arm_recv(u)) return; // arm the multishot recv for this upstream's life; may free u
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
        if (client->req.f.translate_body)
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
                unpair(u);
                u->rbuf.erase(0, total_len);
                ur_release_upstream(u, h.keep_alive, true);
                ++_stats.errors;
                ur_client_send(client);
                return;
            }
            if (!xlate_resp(client->req.f.effective_dialect, body, client->xlate_scratch, client->req.f.tok))
            {
                unpair(u);
                u->rbuf.erase(0, total_len); // framing was valid; the upstream conn is reusable
                ur_release_upstream(u, h.keep_alive, true);
                ur_error_respond(client, 502, "response translate");
                return;
            }
            std::string timing;
            if (_timing_headers)
            {
                const int64_t ts_resp_built = now_ns(); // t5, see the epoll twin
                const TimingSplit sp = timing_split(
                    client->req.f.ts_req_recvd, client->req.f.ts_req_built, client->req.f.ts_wire_ready,
                    client->req.f.ts_up_sent, client->req.f.ts_up_recvd, ts_resp_built);
                append_timing_headers(timing, client->req.f.ts_req_recvd, sp.compute_ns / 1000,
                                      sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                      sp.upstream_ns / 1000, "x-llmbridge-upstream-us",
                                      client->req.f.req_seq, client->req.f.client_upload_ns / 1000);
                append_usage_headers(timing, client->req.f.tok);
            }
            build_http(client->wbuf, "HTTP/1.1 200 OK", client->xlate_scratch, timing);
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
            if (h.body == net::http::Body::Chunked)
                client->wbuf = build_http_status(h.status ? h.status : 200,
                                                 reason_for(h.status ? h.status : 200),
                                                 body_buf);
            else client->wbuf.assign(u->rbuf.data(), total_len);
            // The counts, from the venue's own body. Only the translated branch above
            // scanned, so a byte-forward reported nothing: a sink saw -1 and a tape
            // recorded a request that cost zero tokens at a real price.
            client->req.f.tok = scan_usage(body_buf);
        }
        // Pool the upstream only if it will stay open: the response must say
        // keep-alive and (for passthrough, where the client's Connection header was
        // forwarded verbatim) the client must not have asked to close. Otherwise the
        // upstream is about to close on us. Drop it instead of reusing a corpse.
        const bool keep_alive =
            h.keep_alive && (client->req.f.translate_body || client->msg.keep_alive);
        client->woff = 0;
        unpair(u);
        u->rbuf.erase(0, total_len); // see the epoll mirror
        ur_release_upstream(u, keep_alive, true);
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
                if (c->peer && tls_wbuf_flushed(c)) c->peer->req.f.ts_up_sent = now_ns();
                return;
            }
            // Inbound leg: the response is fully encrypted and fully on the wire,
            // so this is the same completion point the plaintext path reaches when
            // woff catches up with wbuf. Streams continue, everything else finishes.
            if (!tls_wbuf_flushed(c)) return;
            if (c->req.f.streaming)
            {
                c->wbuf.clear();
                c->woff = 0;
                ur_stream_flush(c);
            }
            else if (!c->wbuf.empty())
            {
                if (_sink) sink_emit(c, status_of(c->wbuf), /*streamed=*/false);
                LB_DEBUG(ReqId{c->req.f.req_seq}, " status=", status_of(c->wbuf),
                         " bytes=", c->wbuf.size(), " tokens_in=", c->req.f.tok.in,
                         " tokens_out=", c->req.f.tok.out, " keep_alive=", c->msg.keep_alive,
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
            if (c->peer) c->peer->req.f.ts_up_sent = now_ns();
        }
        else if (c->req.f.streaming)
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
            LB_DEBUG(ReqId{c->req.f.req_seq}, " status=", status_of(c->wbuf), " bytes=", c->wbuf.size(),
                     " tokens_in=", c->req.f.tok.in, " tokens_out=", c->req.f.tok.out,
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
            client->wpending.assign(sse_head_with_timing(
                timing, client->req.f.stream_chunked_out ? kSseHeadChunked : kSseHead));
        }
        else
            client->wpending.assign(client->req.f.stream_chunked_out ? kSseHeadChunked : kSseHead);
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
            if (client->req.f.stream_ended) ur_finalize_stream(client); // nothing left + ended
            return;
        }
        client->wbuf = std::move(client->wpending);
        client->wpending.clear();
        client->woff = 0;
        ur_client_send(client); // sets send_inflight; closes the client on SQE exhaustion
    }

    void Gateway::ur_finalize_stream(Connection* client) noexcept
    {
        if (Connection* u = unpair(client)) // see the epoll mirror
            ur_release_upstream(u, stream_keeps_upstream(client), stream_body_ended(client));
        LB_DEBUG(ReqId{client->req.f.req_seq}, " stream ended ",
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
        client->req.f.streaming = false; // over; begin() resets the rest at the next framing
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
        _uring_ts.tv_sec = kPollTickMs / 1000;
        _uring_ts.tv_nsec = static_cast<long long>(kPollTickMs % 1000) * 1000000LL;

        if (!ur_submit_accept()) ur_pause_accept("accept arm found the SQ full");
        if (!ur_submit_timer()) _uring_timer_lost = true;

        auto reap = [this] {
            _ring.for_each_cqe([this](const io_uring_cqe* cqe) {
                ur_on_cqe(cqe->user_data, cqe->res, cqe->flags);
            });
        };

        while (!_stop)
        {
            const int r = _ring.submit_and_wait(1);
            // EBUSY (CQ overflow backlog) and EAGAIN are transient: reaping clears them.
            if (r < 0 && r != -EINTR && r != -ETIME && r != -EBUSY && r != -EAGAIN)
            {
                LB_ERROR("io_uring_enter failed (", -r, "); this worker stops accepting work");
                break;
            }
            reap();
            // A lost timer disables every timeout and the accept backoff: re-arm it now.
            if (_uring_timer_lost && ur_submit_timer()) _uring_timer_lost = false;
        }

        // Graceful drain: stop taking new work, force every live fd's in-flight ops
        // to complete, and reap until nothing is outstanding, so no kernel op
        // writes into a buffer we're about to free.
        _draining = true;
        auto stop_conn = [this](Connection* c) {
            if (c->fd >= 0) { ::shutdown(c->fd, SHUT_RDWR); ur_submit_cancel(c->fd); }
        };
        for (Connection* c : *_clients) stop_conn(c);
        for (Connection* u : *_upconns) stop_conn(u); // pooled and in flight alike
        // doomed conns were already shut down + cancelled by ur_close().

        while (_uring_inflight > 0)
        {
            const int r = _ring.submit_and_wait(1);
            if (r < 0 && r != -EINTR && r != -ETIME) break;
            reap();
        }

        return 0; // ~Gateway frees what the registries still hold

    }
} // namespace llmbridge
#endif // LLMBRIDGE_HAVE_URING
