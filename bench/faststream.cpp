// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// faststream: a C++ streaming (SSE) provider for the Phase-B saturation test.
// The streaming counterpart to fastbackend.cpp, and it exists for exactly the same
// reason: the Python asyncio mock tops out around 45-50k chunks/s, which is below
// what llmbridge sustains, so a sweep against it measures the mock's ceiling and
// tells you nothing about the gateway's. This backend emits the identical Anthropic
// event stream from a single-threaded epoll loop with no per-token allocation, so
// the proxy in front of it is provably the bottleneck.
//
// Wire-compatible with mock_provider.py --format anthropic: same events, same
// chunked transfer-encoding, and the same "t=<CLOCK_MONOTONIC ns> " emission stamp
// inside each token, which is what streamgen subtracts to get added latency.
//
//   faststream [--port 9002] [--tokens 60] [--token-interval-us 20000]
//              [--prefill-us 20000]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace
{
    int64_t now_ns()
    {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }

    struct Conn
    {
        int fd = -1;
        bool streaming = false;
        bool write_armed = false;
        int tokens_sent = 0;
        int64_t next_emit_ns = 0; // when the next token is due
        std::string rbuf;
        std::string wbuf;
        size_t woff = 0;
    };

    int g_epfd = -1;
    uint64_t g_stream_seq = 0; // for phase staggering (see begin_stream)
    int g_tokens = 60;
    int64_t g_interval_ns = 20'000'000; // 20 ms
    int64_t g_prefill_ns = 20'000'000;

    void arm_write(Conn* c)
    {
        if (c->write_armed) return;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.ptr = c;
        ::epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->write_armed = true;
    }
    void disarm_write(Conn* c)
    {
        if (!c->write_armed) return;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        ::epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->write_armed = false;
    }

    // Append one HTTP/1.1 chunk wrapping one SSE event.
    void append_chunk(std::string& out, const char* payload, size_t len)
    {
        char hdr[32];
        const int n = std::snprintf(hdr, sizeof hdr, "%zx\r\n", len);
        out.append(hdr, static_cast<size_t>(n));
        out.append(payload, len);
        out.append("\r\n", 2);
    }

    // Flush as much of wbuf as the socket accepts. false = peer gone.
    bool pump(Conn* c)
    {
        while (c->woff < c->wbuf.size())
        {
            const ssize_t w = ::write(c->fd, c->wbuf.data() + c->woff, c->wbuf.size() - c->woff);
            if (w > 0) { c->woff += static_cast<size_t>(w); continue; }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { arm_write(c); return true; }
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        c->wbuf.clear();
        c->woff = 0;
        disarm_write(c);
        return true;
    }

    void begin_stream(Conn* c)
    {
        c->streaming = true;
        c->tokens_sent = 0;
        // Phase stagger. Streams that start together would otherwise stay aligned
        // and fire in one synchronized burst every interval, which is both
        // unrealistic (real providers are not in lockstep) and inflates measured
        // latency for every path: the whole burst queues behind one wakeup.
        // Spreading start phases deterministically across the interval makes the
        // emission rate smooth, so what we measure is the gateway, not our own
        // thundering herd.
        const int64_t phase = static_cast<int64_t>((g_stream_seq++ * 2654435761u) % 
                                                   static_cast<uint64_t>(g_interval_ns));
        c->next_emit_ns = now_ns() + g_prefill_ns + phase;
        c->wbuf.append(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: keep-alive\r\n\r\n");
        static const char kStart[] =
            "event: message_start\n"
            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_llmbridge_mock\","
            "\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-mock-1\","
            "\"content\":[],\"usage\":{\"input_tokens\":8,\"output_tokens\":1}}}\n\n";
        static const char kBlock[] =
            "event: content_block_start\n"
            "data: {\"type\":\"content_block_start\",\"index\":0,"
            "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
        append_chunk(c->wbuf, kStart, sizeof(kStart) - 1);
        append_chunk(c->wbuf, kBlock, sizeof(kBlock) - 1);
    }

    // Emit one token, stamped with the current monotonic clock (the stamp is taken
    // at emit time, so scheduler jitter never corrupts the latency measurement
    // it only shifts when the token was sent, which is what we claim it is).
    void emit_token(Conn* c)
    {
        char ev[256];
        const int n = std::snprintf(ev, sizeof ev,
            "event: content_block_delta\n"
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
            "{\"type\":\"text_delta\",\"text\":\"t=%lld \"}}\n\n",
            (long long)now_ns());
        append_chunk(c->wbuf, ev, static_cast<size_t>(n));
        ++c->tokens_sent;
    }

    void end_stream(Conn* c)
    {
        static const char kStop[] =
            "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n";
        static const char kMsgStop[] = "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
        char md[256];
        const int n = std::snprintf(md, sizeof md,
            "event: message_delta\n"
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\","
            "\"stop_sequence\":null},\"usage\":{\"output_tokens\":%d}}\n\n", g_tokens);
        append_chunk(c->wbuf, kStop, sizeof(kStop) - 1);
        append_chunk(c->wbuf, md, static_cast<size_t>(n));
        append_chunk(c->wbuf, kMsgStop, sizeof(kMsgStop) - 1);
        c->wbuf.append("0\r\n\r\n", 5); // terminating chunk
        c->streaming = false;
        c->rbuf.clear(); // keep-alive: ready for the next request
    }
} // namespace

int main(int argc, char** argv)
{
    uint16_t port = 9002;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--port") { if (const char* v = next()) port = static_cast<uint16_t>(std::atoi(v)); }
        else if (a == "--tokens") { if (const char* v = next()) g_tokens = std::atoi(v); }
        else if (a == "--token-interval-us") { if (const char* v = next()) g_interval_ns = std::atoll(v) * 1000; }
        else if (a == "--prefill-us") { if (const char* v = next()) g_prefill_ns = std::atoll(v) * 1000; }
        else if (a == "--help" || a == "-h")
        {
            std::printf("usage: %s [--port N] [--tokens N] [--token-interval-us N] [--prefill-us N]\n", argv[0]);
            return 0;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);

    const int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { std::perror("bind"); return 1; }
    ::listen(lfd, 4096); // deep backlog: a bursty client must never be refused
    g_epfd = ::epoll_create1(0);

    Conn listener;
    listener.fd = lfd;
    epoll_event lev{};
    lev.events = EPOLLIN;
    lev.data.ptr = &listener;
    ::epoll_ctl(g_epfd, EPOLL_CTL_ADD, lfd, &lev);

    std::vector<Conn*> active; // connections with a stream in progress
    std::printf("faststream on 127.0.0.1:%u  tokens=%d interval=%lldus prefill=%lldus\n",
                port, g_tokens, (long long)(g_interval_ns / 1000), (long long)(g_prefill_ns / 1000));
    std::fflush(stdout);

    std::vector<epoll_event> events(4096);
    for (;;)
    {
        // Wake early enough to hit the next token deadline. Sub-millisecond timeout
        // granularity keeps the emission cadence tight even at high concurrency.
        int timeout_ms = 1;
        const int n = ::epoll_wait(g_epfd, events.data(), static_cast<int>(events.size()), timeout_ms);
        for (int i = 0; i < n; ++i)
        {
            Conn* c = static_cast<Conn*>(events[i].data.ptr);
            if (c == &listener)
            {
                for (;;)
                {
                    const int fd = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK);
                    if (fd < 0) break;
                    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    Conn* nc = new Conn();
                    nc->fd = fd;
                    epoll_event ev{};
                    ev.events = EPOLLIN;
                    ev.data.ptr = nc;
                    ::epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev);
                }
                continue;
            }

            const uint32_t e = events[i].events;
            if (e & (EPOLLIN | EPOLLHUP | EPOLLERR))
            {
                char tmp[8192];
                bool gone = false;
                for (;;)
                {
                    const ssize_t r = ::read(c->fd, tmp, sizeof(tmp));
                    if (r > 0) { c->rbuf.append(tmp, static_cast<size_t>(r)); continue; }
                    if (r == 0) { gone = true; break; }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    gone = true;
                    break;
                }
                if (gone)
                {
                    for (size_t k = 0; k < active.size(); ++k)
                        if (active[k] == c) { active[k] = active.back(); active.pop_back(); break; }
                    ::epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, nullptr);
                    ::close(c->fd);
                    delete c;
                    continue;
                }
                // A complete request header is enough to start (the body is small and
                // arrives with it); we don't need to interpret it: every request to
                // this backend is a streaming request by construction.
                if (!c->streaming && c->rbuf.find("\r\n\r\n") != std::string::npos)
                {
                    begin_stream(c);
                    active.push_back(c);
                    if (!pump(c)) { /* peer gone; cleaned up on next event */ }
                }
            }
            if ((e & EPOLLOUT) && c->fd >= 0) pump(c);
        }

        // Emit every token that has come due. O(active) per tick with trivial
        // integer comparisons; the write() syscalls dominate, not this scan.
        const int64_t t = now_ns();
        for (size_t k = 0; k < active.size();)
        {
            Conn* c = active[k];
            bool finished = false;
            while (c->streaming && c->tokens_sent < g_tokens && c->next_emit_ns <= t)
            {
                emit_token(c);
                c->next_emit_ns += g_interval_ns; // absolute schedule: no cumulative drift
            }
            if (c->streaming && c->tokens_sent >= g_tokens) { end_stream(c); finished = true; }
            if (!c->wbuf.empty() && !pump(c))
            {
                ::epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, nullptr);
                ::close(c->fd);
                delete c;
                active[k] = active.back();
                active.pop_back();
                continue;
            }
            if (finished) { active[k] = active.back(); active.pop_back(); continue; }
            ++k;
        }
    }
}
