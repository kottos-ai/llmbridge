// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Minimal C++ HTTP/1.1 backend for the saturation test.
//
// The Python asyncio mock tops out in the low tens of thousands of RPS — fine
// for measuring llmbridge's *added latency* (where the 200 ms backend dominates),
// but useless for finding where llmbridge itself saturates, because the mock would
// fall over first. This backend is a stripped epoll echo-responder built on the
// same net/ primitives: read a request, immediately write a canned
// chat-completion response, keep-alive. Single thread, zero per-request alloc
// beyond buffer growth — it sustains far more RPS than the proxy in front of
// it, so the proxy is provably the bottleneck in the ramp.
//
//   fastbackend [--port 9002] [--latency-us N] [--anthropic] [--tools]
//                                               (latency default 0 = instant)
//
// --tools serves an Anthropic response containing tool_use blocks. That path is
// NOT covered by the plain body: a tool response makes the translator decode the
// tool_use input object and re-emit it as an OpenAI `arguments` STRING (escaping
// it), which is measurably more work than copying text through. Without this the
// regression sweep would report "no change" for edits that only affect the tool
// path — which is exactly what happened once: a live check showed 19 us for tool
// responses against 15 us for plain, a difference the benchmark could not see.

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
#include <string_view>

#include "net/http.hpp"
#include "net/socket_util.hpp"

namespace
{
    struct Connection
    {
        int fd = -1;
        std::string rbuf;
        std::string wbuf;
        size_t woff = 0;
        bool write_armed = false;
    };

    const std::string kBodyOpenAI =
        "{\"id\":\"chatcmpl-fast\",\"object\":\"chat.completion\",\"model\":\"fast-1\","
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"pong\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":8,\"completion_tokens\":1,"
        "\"total_tokens\":9}}";

    // Anthropic Messages shape — for the translation benchmark (gateway under
    // test translates Anthropic -> OpenAI on the way back).
    const std::string kBodyAnthropic =
        "{\"id\":\"msg_fast\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"fast-1\","
        "\"content\":[{\"type\":\"text\",\"text\":\"pong\"}],\"stop_reason\":\"end_turn\","
        "\"usage\":{\"input_tokens\":8,\"output_tokens\":1}}";

    // Anthropic response carrying TWO tool_use blocks (parallel calls are the
    // common agent shape) plus a text block, so the translator exercises: block
    // iteration, input-object -> arguments-string escaping, and the tool_calls
    // array build. Kept deliberately small — this measures the translation, not
    // the memcpy of a large body.
    const std::string kBodyTools =
        "{\"id\":\"msg_fast\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"fast-1\","
        "\"content\":[{\"type\":\"text\",\"text\":\"checking\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"get_weather\","
        "\"input\":{\"city\":\"Paris\",\"units\":\"c\"}},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"get_weather\","
        "\"input\":{\"city\":\"Rome\",\"units\":\"c\"}}],"
        "\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":24,\"output_tokens\":18}}";

    // Which canned body to serve. An enum rather than a bool: the dialect list
    // already has more than two members in the translator (Gemini, Cohere) and
    // will grow, and "bool anthropic" plus "bool tools" cannot express that —
    // two bools also admit a nonsense state (openai+tools) the enum makes
    // unrepresentable.
    enum class Body { OpenAi, Anthropic, AnthropicTools };

    const std::string& body_for(Body b)
    {
        switch (b)
        {
            case Body::Anthropic:      return kBodyAnthropic;
            case Body::AnthropicTools: return kBodyTools;
            case Body::OpenAi:         break;
        }
        return kBodyOpenAI;
    }

    const char* body_name(Body b)
    {
        switch (b)
        {
            case Body::Anthropic:      return "anthropic";
            case Body::AnthropicTools: return "anthropic+tools";
            case Body::OpenAi:         break;
        }
        return "openai";
    }

    std::string g_resp;
    int g_epfd = -1;

    // epoll is level-triggered; EPOLLIN stays armed for the connection's life and
    // write interest is toggled on top via EPOLL_CTL_MOD.
    void add_read(Connection* c)
    {
        epoll_event ev{}; ev.events = EPOLLIN; ev.data.ptr = c;
        ::epoll_ctl(g_epfd, EPOLL_CTL_ADD, c->fd, &ev);
    }
    void arm_write(Connection* c)
    {
        if (c->write_armed) return;
        epoll_event ev{}; ev.events = EPOLLIN | EPOLLOUT; ev.data.ptr = c;
        ::epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev); c->write_armed = true;
    }
    void disarm_write(Connection* c)
    {
        if (!c->write_armed) return;
        epoll_event ev{}; ev.events = EPOLLIN; ev.data.ptr = c;
        ::epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev); c->write_armed = false;
    }
    void close_conn(Connection* c) { if (c->fd >= 0) ::close(c->fd); delete c; }

    bool pump(Connection* c)
    {
        while (c->woff < c->wbuf.size())
        {
            ssize_t n = ::write(c->fd, c->wbuf.data() + c->woff, c->wbuf.size() - c->woff);
            if (n > 0) { c->woff += (size_t)n; continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { arm_write(c); return true; }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        c->wbuf.clear(); c->woff = 0; disarm_write(c);
        return true;
    }
} // namespace

int main(int argc, char** argv)
{
    uint16_t port = 9002;
    long latency_us = 0;
    Body body = Body::OpenAi;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "--port" && i + 1 < argc) port = (uint16_t)std::atoi(argv[++i]);
        else if (a == "--latency-us" && i + 1 < argc) latency_us = std::atol(argv[++i]);
        else if (a == "--anthropic") body = Body::Anthropic;      // Anthropic-shape body
        else if (a == "--tools") body = Body::AnthropicTools;     // + tool_use blocks
    }

    std::signal(SIGPIPE, SIG_IGN);

    // --tools selects the Anthropic shape too: tool_use is an Anthropic construct,
    // so "openai + tools" is not a thing the enum can express.
    const std::string& kBody = body_for(body);
    g_resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
             std::to_string(kBody.size()) + "\r\nConnection: keep-alive\r\n\r\n" + kBody;

    g_epfd = ::epoll_create1(0);
    int lfd = llmbridge::net::make_listener(port);
    if (lfd < 0) { std::fprintf(stderr, "bind :%u failed\n", port); return 1; }
    Connection* lc = new Connection(); lc->fd = lfd;
    add_read(lc);
    std::fprintf(stderr, "fastbackend: :%u latency=%ldus body=%s\n", port, latency_us,
                 body_name(body));

    epoll_event events[2048];
    for (;;)
    {
        int n = ::epoll_wait(g_epfd, events, 2048, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; ++i)
        {
            Connection* c = static_cast<Connection*>(events[i].data.ptr);
            if (c == lc)
            {
                for (;;)
                {
                    int fd = ::accept(lfd, nullptr, nullptr);
                    if (fd < 0) break;
                    llmbridge::net::set_nonblocking(fd);
                    llmbridge::net::set_nodelay(fd);
                    llmbridge::net::set_nosigpipe(fd);
                    Connection* nc = new Connection(); nc->fd = fd; nc->rbuf.reserve(2048);
                    add_read(nc);
                }
                continue;
            }

            const uint32_t e = events[i].events;
            // epoll coalesces a fd's readiness, so handle EPOLLOUT then EPOLLIN in
            // the same iteration. If the write half closes the conn, skip the rest.
            if (e & EPOLLOUT) { if (!pump(c)) { close_conn(c); continue; } }
            if (!(e & (EPOLLIN | EPOLLHUP | EPOLLERR))) continue;

            // readable
            char tmp[16384];
            bool dead = false;
            for (;;)
            {
                ssize_t r = ::read(c->fd, tmp, sizeof(tmp));
                if (r > 0) { c->rbuf.append(tmp, (size_t)r); if ((size_t)r < sizeof(tmp)) break; continue; }
                if (r == 0) { dead = true; break; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                dead = true; break;
            }
            if (dead) { close_conn(c); continue; }

            // Drain all complete requests in the buffer, respond to each.
            for (;;)
            {
                llmbridge::net::http::Message m;
                auto st = llmbridge::net::http::parse_request(c->rbuf, m);
                if (st != llmbridge::net::http::FrameStatus::Complete) break;
                c->rbuf.erase(0, m.total_len);
                if (latency_us > 0) { struct timespec t{latency_us / 1000000, (latency_us % 1000000) * 1000}; nanosleep(&t, nullptr); }
                c->wbuf.append(g_resp);
            }
            if (!c->wbuf.empty() && !pump(c)) { close_conn(c); }
        }
    }
    return 0;
}
