// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// nullrelay — the ZERO-WORK control for the streaming benchmark: the dumbest
// possible proxy. Accept a client, connect
// upstream, forward bytes both ways. NO HTTP parsing, NO chunked decode, NO SSE
// translation, no per-chunk state. Whatever latency THIS adds is the irreducible
// cost of inserting one extra process/hop into the path — i.e. the floor that any
// gateway pays before doing any work at all.
//
// This control exists because it answers the question "is the gateway's added
// latency its own work, or the price of the hop?" — and the answer is not what
// intuition suggests. Measured at 64 concurrent streams, p50 added latency:
//
//                                        C-states default   C-states capped at C1
//     direct (1 hop)                          53 us                13 us
//     nullrelay (2 hops, no work)             106 us                27 us
//     llmbridge (2 hops, full translate)      108 us                32 us
//     ------------------------------------------------------------------------
//     => cost of the HOP itself                53 us                14 us
//     => cost of ALL of llmbridge's work        2 us  (noise)         5 us
//
// Two conclusions, both of which needed this control to reach:
//
// 1. llmbridge's HTTP framing + chunked decode + SSE translation + re-serialisation
//    costs ~5 us/token. Everything else is the price of inserting a process into the
//    path. Every gateway sits at ~5% of one core, so none of this is compute-bound.
//
// 2. The hop is mostly a HOST TUNING artefact, not transit. Capping idle states at C1
//    (2 us exit, vs 70-890 us for C3-C10) cuts the hop 3.9x. See bench/LATENCY-TUNING.md.
//    Of the 14 us that remains, busy-polling the relay recovers only ~3 us — so the
//    residual is the loopback data path (copies, TCP, softirq, waking the RECEIVER),
//    not this process's own wakeup.
//
// It also explains why io_uring and epoll measure the same here, which is not obvious:
// io_uring really does cut syscalls (2.31 fewer per delivered token, measured with
// strace). But a syscall costs ~537 ns on this box, so the entire theoretical saving is
// ~1.24 us/token — under 4% of the 32 us total, and below run-to-run variance. It stays
// invisible even after the C-state fix, and io_uring's own bookkeeping offsets it.
// io_uring pays off where work-per-wakeup is high (the non-streaming ~90k RPS path),
// not in per-token latency at a few thousand tokens/s.
//
//   nullrelay --listen 8991 --upstream 9001
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
struct C { int fd = -1; C* peer = nullptr; bool is_client = false; bool doomed = false; };
int ep = -1;
sockaddr_in up{};
std::vector<C*> doomed_list; // freed at end of batch, never mid-batch

void add(C* c, uint32_t ev) { epoll_event e{}; e.events = ev; e.data.ptr = c; ::epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &e); }

// One epoll_wait batch can carry events for BOTH ends of a pair, so a conn must not
// be freed while that batch is still being walked — the next entry would dereference
// freed memory. Same deferred-free discipline as the gateway's event loop.
//
// Status of this hazard: LATENT here, not observed. A busy-polling variant of this
// relay (which reaps far more events per epoll_wait, so both ends of a pair share a
// batch much more often) did corrupt its heap without this guard. 250k pair teardowns
// under ASan did NOT reproduce it in this blocking version — the guard is hardening,
// not a fix for a demonstrated failure. Kept because a control whose own memory
// safety is in question cannot be used to attribute latency.
void retire(C* c) {
    if (c->doomed) return;
    c->doomed = true;
    ::epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, nullptr);
    ::close(c->fd);
    c->fd = -1;
    doomed_list.push_back(c);
}

void kill_pair(C* c) {
    if (C* p = c->peer) { p->peer = nullptr; c->peer = nullptr; retire(p); }
    retire(c);
}
} // namespace

int main(int argc, char** argv) {
    uint16_t lport = 8500, uport = 9000;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--listen") && i + 1 < argc) lport = (uint16_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--upstream") && i + 1 < argc) uport = (uint16_t)std::atoi(argv[++i]);
    }
    std::signal(SIGPIPE, SIG_IGN);
    up.sin_family = AF_INET; up.sin_port = htons(uport);
    ::inet_pton(AF_INET, "127.0.0.1", &up.sin_addr);

    int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1; ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(lport);
    if (::bind(lfd, (sockaddr*)&a, sizeof a) < 0) { std::perror("bind"); return 1; }
    ::listen(lfd, 4096);
    ep = ::epoll_create1(0);
    C listener; listener.fd = lfd; add(&listener, EPOLLIN);
    std::printf("nullrelay :%u -> 127.0.0.1:%u\n", lport, uport); std::fflush(stdout);

    epoll_event evs[4096];
    char buf[65536];
    for (;;) {
        int n = ::epoll_wait(ep, evs, 4096, -1);
        for (int i = 0; i < n; ++i) {
            C* c = (C*)evs[i].data.ptr;
            if (c != &listener && c->doomed) continue; // retired earlier in this batch
            if (c == &listener) {
                for (;;) {
                    int cfd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK);
                    if (cfd < 0) break;
                    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    int ufd = ::socket(AF_INET, SOCK_STREAM, 0); // blocking connect: setup only
                    ::setsockopt(ufd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    if (::connect(ufd, (sockaddr*)&up, sizeof up) < 0) { ::close(cfd); ::close(ufd); continue; }
                    int fl = 1; ::ioctl(ufd, FIONBIO, &fl);
                    C* cl = new C{cfd, nullptr, true};
                    C* us = new C{ufd, cl, false};
                    cl->peer = us;
                    add(cl, EPOLLIN); add(us, EPOLLIN);
                }
                continue;
            }
            if (!c->peer) { kill_pair(c); continue; }
            bool dead = false;
            for (;;) {
                ssize_t r = ::read(c->fd, buf, sizeof buf);
                if (r > 0) {
                    ssize_t off = 0;
                    while (off < r) {  // blocking-ish drain; fine for a control
                        ssize_t w = ::write(c->peer->fd, buf + off, r - off);
                        if (w > 0) { off += w; continue; }
                        if (w < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                        dead = true; break;
                    }
                    if (dead) break;
                    continue;
                }
                if (r == 0) { dead = true; break; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                dead = true; break;
            }
            if (dead) kill_pair(c);
        }
        for (C* d : doomed_list) delete d; // safe: batch fully walked
        doomed_list.clear();
    }
}
