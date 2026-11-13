// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The io_uring backend. Every CQE says one operation finished; the shared lifecycle
// acts and submits the next. A connection is freed only when its `inflight` SQEs have
// all completed, so no completion lands on freed memory.

#include "io/uring_io.hpp"

#ifdef LLMBRIDGE_HAVE_URING

#include "core/conn.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"
#include "engine.hpp"
#include "net/socket_util.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace llmbridge::detail
{
    namespace
    {
        // user_data: the connection, the op in the low three bits and the kind in the
        // fourth (ConnBase is aligned to 16), so a completion reaches a typed handler.
        enum UOp : uint64_t { UAccept = 0, URecv = 1, USend = 2, UConnect = 3, UTimer = 4, UCancel = 5 };
        constexpr uint64_t kOpMask = 7;
        constexpr uint64_t kUpstreamBit = 8;
        static_assert(alignof(ConnBase) >= 16);
        inline uint64_t make_ud(const ConnBase* c, UOp op)
        {
            return reinterpret_cast<uintptr_t>(c) | op | (c->kind == ConnKind::Upstream ? kUpstreamBit : 0);
        }
        inline ConnBase* ud_conn(uint64_t d) { return reinterpret_cast<ConnBase*>(d & ~(kOpMask | kUpstreamBit)); }
        inline UOp ud_op(uint64_t d) { return static_cast<UOp>(d & kOpMask); }

        constexpr unsigned kRingDepth = 4096;
        // Provided buffers for the multishot recvs, each recycled as soon as it is copied out.
        constexpr unsigned kBufGroup = 1;
        constexpr unsigned kBufCount = 4096; // power of two
        constexpr unsigned kBufSize = 4096;
    } // namespace

    UringIo::UringIo(Engine& e) noexcept : _e(e)
    {
        unsigned flags = 0;
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_DEFER_TASKRUN)
        flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
#endif
        // A failure here is reported, and Engine::run decides what follows.
        if (!_ring.init(kRingDepth, flags) && !_ring.init(kRingDepth, 0))
        {
            LB_WARN("io_uring ring setup failed");
            return;
        }
        const unsigned count = _e._uring_buf_count ? _e._uring_buf_count : kBufCount;
        if (!_bufring.init(_ring, kBufGroup, count, kBufSize))
        {
            LB_WARN("io_uring provided-buffer ring unavailable (", _bufring.init_stage(),
                    " errno=", _bufring.init_errno(), " ", std::strerror(_bufring.init_errno()), ")");
            return;
        }
        _ts.tv_sec = kPollTickMs / 1000;
        _ts.tv_nsec = static_cast<long long>(kPollTickMs % 1000) * 1000000LL;
        _ready = true;
    }

    bool UringIo::next_sqe(io_uring_sqe** out) noexcept
    {
        io_uring_sqe* s = _ring.get_sqe();
        if (!s)
        {
            _ring.submit(); // SQ full: flush, retry once
            s = _ring.get_sqe();
        }
        *out = s;
        return s != nullptr;
    }

    bool UringIo::submit_accept() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!next_sqe(&s)) return false;
        s->opcode = IORING_OP_ACCEPT;
        s->fd = _e._listen_fd;
        s->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
        s->ioprio = IORING_ACCEPT_MULTISHOT; // one SQE, a completion per accepted fd
        s->user_data = UAccept;
        return true;
    }

    // The timer tick re-arms the accept: an EMFILE ends the multishot, and an immediate
    // re-arm fails again at once.
    void UringIo::pause_accept(const char* why) noexcept
    {
        _e._accept_resume_ns = now_ns() + kAcceptBackoffNs;
        ++_e._stats.accept_backoffs;
        LB_WARN("CAP ", why, "; accepting paused for 100 ms, total=", _e._stats.accept_backoffs);
    }

    bool UringIo::submit_timer() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!next_sqe(&s)) return false;
        s->opcode = IORING_OP_TIMEOUT;
        s->addr = reinterpret_cast<uint64_t>(&_ts);
        s->len = 1;
        s->user_data = UTimer;
        return true;
    }

    // Full even after a flush: this pair cannot progress, so both legs end together.
    void UringIo::drop_sq_full(ConnBase* c) noexcept
    {
        LB_WARN("CAP io_uring submission queue full; closing ", *c);
        if (ClientConn* client = visit(*c, [](auto& k) { return client_of(k); })) _e.abort_pair(client);
        else close(*c);
    }

    // One multishot recv per connection, drawing from the provided buffers; re-armed
    // only when the kernel ends it.
    bool UringIo::arm_recv(ConnBase* c) noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!next_sqe(&s))
        {
            drop_sq_full(c);
            return false;
        }
        s->opcode = IORING_OP_RECV;
        s->fd = c->fd;
        s->addr = 0;
        s->len = 0;
        s->flags |= IOSQE_BUFFER_SELECT;
        s->buf_group = kBufGroup;
        s->ioprio |= IORING_RECV_MULTISHOT;
        s->user_data = make_ud(c, URecv);
        ++c->inflight;
        ++_inflight;
        c->recv_armed = true;
        return true;
    }

    // Its last completion, -ECANCELED, re-arms if reads resumed meanwhile. Bytes the
    // kernel already placed still arrive: bounded by the shared buffer ring.
    bool UringIo::cancel_recv(ConnBase* c) noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!next_sqe(&s)) return false;
        s->opcode = IORING_OP_ASYNC_CANCEL;
        s->addr = make_ud(c, URecv);
        s->user_data = UCancel;
        return true;
    }

    bool UringIo::submit_send(ConnBase* c) noexcept
    {
        io_uring_sqe* s = nullptr;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!_e.tls_invariant_ok(c))
        {
            drop_sq_full(c); // never plaintext on a TLS conn
            return false;
        }
#endif
        if (!next_sqe(&s))
        {
            drop_sq_full(c);
            return false;
        }
        s->opcode = IORING_OP_SEND;
        s->fd = c->fd;
        OutBuf& w = c->wire(); // ciphertext on TLS
        s->addr = reinterpret_cast<uint64_t>(w.wire().data());
        s->len = static_cast<unsigned>(w.wire().size());
        s->user_data = make_ud(c, USend);
        // The only pin: the kernel reads the front until on_send. GATEWAY-INTERNALS.md 5b.
        w.pin();
        ++c->inflight;
        ++_inflight;
        return true;
    }

    // Shutdown does not end an armed multishot recv, so cancel everything on the fd; the
    // cancelled ops complete with -ECANCELED and release their inflight slots.
    void UringIo::submit_cancel(int fd) noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!next_sqe(&s)) return;
        s->opcode = IORING_OP_ASYNC_CANCEL;
        s->fd = fd;
        s->cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL;
        s->user_data = UCancel; // not inflight-counted, completion ignored
    }

#ifdef LLMBRIDGE_HAVE_TLS
    void UringIo::tls_flush(ConnBase* u) noexcept
    {
        if (u->tls_out.pinned()) return; // the completion flushes what staged behind
        _e.tls_pump_out(u);
        if (u->tls_out.idle()) return;
        submit_send(u);
    }
#endif

    // A TLS conn's SQE points at tls_out, so plaintext must pass through the Session
    // first, or the kernel is handed a zero-length send (found by curl, not reading).
    void UringIo::client_send(ConnBase* c) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            if (c->tls->handshake_done()) _e.tls_push_out(c);
            tls_flush(c);
            return;
        }
#endif
        submit_send(c);
    }

    void UringIo::close(ConnBase& c) noexcept
    {
        if (c.doomed) return;
        ConnBase* other = _e.forget(c);
        // Every op on the fd completes, so the inflight count drains; the fd closes at the free.
        // A reset sends no FIN ahead of its RST.
        if (c.fd >= 0)
        {
            ::shutdown(c.fd, c.reset_on_close ? SHUT_RD : SHUT_RDWR);
            submit_cancel(c.fd);
        }
        if (other) close(*other); // an upstream mid-response is of no use to anyone else
        maybe_free(&c);
    }

    void UringIo::maybe_free(ConnBase* c) noexcept
    {
        if (!c->doomed || c->inflight > 0) return; // a completion still references it
        if (c->fd >= 0)
        {
            ::close(c->fd);
            c->fd = -1;
        }
        _e.free_conn(c);
    }

    Sent UringIo::send(ConnBase& c) noexcept
    {
        if (c.send_inflight()) return Sent::Queued; // its completion sends the rest
#ifdef LLMBRIDGE_HAVE_TLS
        if (c.tls ? _e.tls_out_flushed(&c) : c.out.idle()) return Sent::Done;
#else
        if (c.out.idle()) return Sent::Done;
#endif
        client_send(&c); // on SQ exhaustion it has already torn `c` down
        return Sent::Queued;
    }

    int UringIo::connect(UpstreamConn& u) noexcept
    {
        const int fd = net::make_client_socket();
        if (fd < 0) return errno;
        if (_e._upstream_sndbuf > 0)
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &_e._upstream_sndbuf, sizeof(_e._upstream_sndbuf));
        u.fd = fd;
        io_uring_sqe* s = nullptr;
        if (!next_sqe(&s))
        {
            ::close(fd);
            u.fd = -1;
            return EAGAIN; // the submission queue is full: this host's, not the venue's
        }
        s->opcode = IORING_OP_CONNECT;
        s->fd = fd;
        s->addr = reinterpret_cast<uint64_t>(&u.up_addr);
        s->off = sizeof(u.up_addr); // the addrlen rides in `off`
        s->user_data = make_ud(&u, UConnect);
        ++u.inflight;
        ++_inflight;
        return 0;
    }

    void UringIo::set_read(ConnBase& c, bool on) noexcept
    {
        if (c.read_paused == !on) return;
        if (on)
        {
            c.read_paused = false;
            if (!c.recv_armed) arm_recv(&c); // else its cancel's completion does
        }
        else if (!c.recv_armed || cancel_recv(&c))
            c.read_paused = true;
    }

    void UringIo::on_cqe(uint64_t user_data, int res, uint32_t flags) noexcept
    {
        const UOp op = ud_op(user_data);
        if (op == UTimer)
        {
            if (!_draining && !_e._stop)
            {
                _e.sweep_idle();
                if (_e._accept_resume_ns && now_ns() >= _e._accept_resume_ns)
                {
                    _e._accept_resume_ns = 0;
                    if (!submit_accept()) pause_accept("accept re-arm found the SQ full");
                }
                if (!submit_timer()) _timer_lost = true;
            }
            return;
        }
        if (op == UCancel) return;
        if (op == UAccept) return on_accept(res, flags);

        ConnBase* c = ud_conn(user_data);
        // A multishot recv stays armed only while F_MORE is set and it delivers data; its
        // terminal completion may still carry F_MORE, and must release the slot.
        const bool armed = (op == URecv) && (flags & IORING_CQE_F_MORE) && res > 0;
        if (!armed)
        {
            --c->inflight;
            --_inflight;
        }
        if (op == URecv && !armed) c->recv_armed = false;
        if (_draining || c->doomed)
        {
            if (op == USend) c->wire().sent(0);
            if (op == URecv && (flags & IORING_CQE_F_BUFFER))
                _bufring.recycle(flags >> IORING_CQE_BUFFER_SHIFT);
            if (!armed) maybe_free(c);
            return;
        }
        if (user_data & kUpstreamBit)
        {
            auto* u = static_cast<UpstreamConn*>(c);
            if (op == URecv) on_recv(u, res, flags);
            else if (op == USend) on_send(u, res);
            else if (op == UConnect) on_connect(u, res);
        }
        else if (op == URecv) on_recv(static_cast<ClientConn*>(c), res, flags);
        else if (op == USend) on_send(static_cast<ClientConn*>(c), res);
    }

    void UringIo::on_accept(int res, uint32_t flags) noexcept
    {
        if (_e._stop || _draining)
        {
            if (res >= 0) ::close(res); // shutting down: no new work
            return;
        }
        if (!(flags & IORING_CQE_F_MORE)) // the multishot ended: re-arm, unless that would spin
        {
            if (res == -EMFILE || res == -ENFILE) pause_accept("out of file descriptors");
            else if (!submit_accept()) pause_accept("accept re-arm found the SQ full");
        }
        if (res < 0) return;
        const int fd = res;
        net::set_nodelay(fd);
        if (_e._client_sndbuf > 0)
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &_e._client_sndbuf, sizeof(_e._client_sndbuf));
        auto* c = new ClientConn();
        c->fd = fd;
        c->id = _e._next_client_id++;
        c->ts_accepted = now_ns();
        c->ts_client_activity = c->ts_accepted;
        c->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (_e._tls.client_tls && !_e.tls_attach_client(c))
        {
            // Fail closed. Nothing is in flight yet, so it is freed directly.
            ::close(fd);
            delete c;
            ++_e._stats.errors;
            return;
        }
#endif
        _e._clients->add(c);
        LB_DEBUG("accept ", *c, " clients=", _e._clients->size());
        arm_recv(c);
    }

    template <class C> void UringIo::on_recv(C* c, int res, uint32_t flags) noexcept
    {
        const bool armed = flags & IORING_CQE_F_MORE;
        // The provided buffers ran out for a moment: the kernel ended the multishot
        // without consuming one, so re-arm. Never observed at the shipped size (0 at
        // 8192 streams); the regression test forces it by shrinking the pool.
        if (res == -ENOBUFS)
        {
            ++_e._stats.uring_enobufs;
            LB_WARN("CAP io_uring provided buffers exhausted; recv re-armed total=", _e._stats.uring_enobufs);
            if (!c->read_paused) arm_recv(c);
            return;
        }
        if (res == -ECANCELED) // a pause's cancel, or a resume that raced it
        {
            if (!c->read_paused) arm_recv(c);
            return;
        }
        if (res <= 0) // EOF or an error
        {
            if (flags & IORING_CQE_F_BUFFER) _bufring.recycle(flags >> IORING_CQE_BUFFER_SHIFT);
            return lost(c, false);
        }
        // Copied out of the kernel's buffer, which goes straight back. TLS bytes are
        // ciphertext: the Session's plaintext lands in rbuf.
        bool tls_ok = true;
        if (flags & IORING_CQE_F_BUFFER)
        {
            const unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
#ifdef LLMBRIDGE_HAVE_TLS
            if (c->tls)
                tls_ok = _e.tls_feed(c, _bufring.data(bid), static_cast<size_t>(res));
            else
#endif
            {
                stamp_first_byte(*c);
                c->rbuf.append(_bufring.data(bid), static_cast<size_t>(res));
            }
            _bufring.recycle(bid);
            if constexpr (std::is_same_v<C, UpstreamConn>) note_response_bytes(c);
        }
        // The kernel ended the multishot (pool pressure): re-arm. A failed arm may free c.
        if (!armed && !c->read_paused && !arm_recv(c)) return;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_ok) return lost(c, true);
        if (c->tls) tls_flush(c); // handshake replies, or request bytes just pushed
#else
        (void)tls_ok;
#endif
        arrived(c);
    }

    // Fatal on a client is its connection dying, never a retryable upstream fault.
    void UringIo::lost(ClientConn* c, bool) noexcept { _e.client_gone(c); }

    void UringIo::lost(UpstreamConn* u, bool tls_failed) noexcept { _e.on_upstream_eof(u, tls_failed); }

    void UringIo::arrived(ClientConn* c) noexcept { _e.on_client_bytes(c); }

    void UringIo::arrived(UpstreamConn* u) noexcept { _e.on_upstream_bytes(u); }

    void UringIo::on_connect(UpstreamConn* u, int res) noexcept
    {
        if (res < 0) return _e.on_connect_error(u, -res);
        u->connected = true;
        // t2 for a plaintext upstream; tls_feed() stamps it when a handshake completes.
        if (!_e.upstream_is_tls(u))
        {
            u->wire_ready = true;
            if (u->client) u->client->req.f.ts_wire_ready = now_ns();
        }
        if (!arm_recv(u)) return; // for the upstream's life; may free u
#ifdef LLMBRIDGE_HAVE_TLS
        if (u->tls)
        {
            // The request waits in `out` until the handshake completes; the ClientHello goes now.
            u->tls->start_handshake();
            tls_flush(u);
            return;
        }
#endif
        submit_send(u);
    }

    template <class C> void UringIo::on_send(C* c, int res) noexcept
    {
        c->wire().sent(res > 0 ? static_cast<size_t>(res) : 0); // unpins; what staged folds in
        if (res <= 0) return send_failed(c);
        c->sent_bytes += static_cast<uint64_t>(res);
        progressed(c);
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            if (!c->tls_out.idle())
            {
                submit_send(c); // partial, or staged behind
                return;
            }
            // The Session may hold more: later flights, or plaintext held for the handshake.
            if (c->tls->handshake_done() && !c->out.idle()) _e.tls_push_out(c);
            tls_flush(c);
            if (c->send_inflight() || !_e.tls_out_flushed(c)) return;
            return drained(c);
        }
#endif
        if (!c->out.idle())
        {
            submit_send(c); // partial, or staged behind
            return;
        }
        drained(c);
    }

    void UringIo::send_failed(ClientConn* c) noexcept { _e.client_gone(c); }

    void UringIo::send_failed(UpstreamConn* u) noexcept { _e.upstream_lost(u, Engine::UpFail::Write); }

    // A client draining a stream the provider finished is progress, not idleness.
    void UringIo::progressed(ClientConn* c) noexcept
    {
        if (c->req.f.streaming) c->req.f.ts_progress = now_ns();
    }

    // Before the rest is resubmitted, so `out` says whether everything staged is in.
    void UringIo::progressed(UpstreamConn* u) noexcept
    {
        if (u->client) _e.note_sent(u, now_ns()); // the idle clock, t3, or a body's last byte
    }

    void UringIo::drained(ClientConn* c) noexcept
    {
        if (c->req.f.streaming) _e.stream_flush(c); // finalizes if the stream ended
        else _e.finish(c);
    }

    // An upstream's request stays in `out` for a resend; its response comes by recv, and
    // progressed() took its stamps.
    void UringIo::drained(UpstreamConn*) noexcept {}

    int UringIo::run()
    {
        _e._t_start = now_ns();
        _e._active_backend = 2;
        _e.attach(this, IoCaps{.stream_backpressure = true,
                               .stream_pause_above = kStreamPauseAbove,
                               .stream_cap = kStreamBufCap});
        if (!submit_accept()) pause_accept("accept arm found the SQ full");
        if (!submit_timer()) _timer_lost = true;
        const auto reap = [this] {
            _ring.for_each_cqe([this](const io_uring_cqe* cqe) { on_cqe(cqe->user_data, cqe->res, cqe->flags); });
        };
        while (!_e._stop)
        {
            const int r = _ring.submit_and_wait(1);
            // EBUSY (a CQ overflow backlog) and EAGAIN are transient: reaping clears them.
            if (r < 0 && r != -EINTR && r != -ETIME && r != -EBUSY && r != -EAGAIN)
            {
                LB_ERROR("io_uring_enter failed (", -r, "); this worker stops accepting work");
                break;
            }
            reap();
            // A lost timer disables every timeout and the accept backoff.
            if (_timer_lost && submit_timer()) _timer_lost = false;
        }
        // Drain: no new work, every live fd's operations forced to complete, and reaped
        // until none is outstanding, so no kernel write lands in freed memory. Closed
        // connections were cancelled by close().
        _draining = true;
        const auto stop_conn = [this](ConnBase* c) {
            if (c->fd < 0) return;
            ::shutdown(c->fd, SHUT_RDWR);
            submit_cancel(c->fd);
        };
        for (ClientConn* c : *_e._clients) stop_conn(c);
        for (UpstreamConn* u : *_e._upconns) stop_conn(u); // pooled and in flight alike
        while (_inflight > 0)
        {
            const int r = _ring.submit_and_wait(1);
            if (r < 0 && r != -EINTR && r != -ETIME) break;
            reap();
        }
        _e.attach(nullptr, IoCaps{});
        return 0; // the Engine frees what the registries still hold
    }
} // namespace llmbridge::detail

#endif // LLMBRIDGE_HAVE_URING
