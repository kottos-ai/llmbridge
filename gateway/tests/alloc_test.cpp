// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Heap allocations per request on the event-loop thread, held to ceilings.
// Global operator new is replaced and counts only on a thread that opted in, which
// is the thread that calls Gateway::run(). C mallocs are not seen (getaddrinfo,
// OpenSSL), and nothing else in the process is: the mock upstream and the client
// run on other threads. Counts are deterministic, so the ceilings are exact
// numbers: a change that adds one allocation per request fails here, and a change
// that removes one lowers the ceiling in the same commit.

#include "gateway/gateway.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>

// TSan's runtime defines operator new and delete itself, and a second definition does
// not link, so under TSan nothing is replaced and the ceilings are skipped. ASan's
// runtime links beside these and the test runs there.
#if defined(__SANITIZE_THREAD__)
    #define LLMBRIDGE_ALLOC_UNDER_TSAN 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define LLMBRIDGE_ALLOC_UNDER_TSAN 1
    #endif
#endif
#ifndef LLMBRIDGE_ALLOC_UNDER_TSAN
    #define LLMBRIDGE_ALLOC_UNDER_TSAN 0
#endif

namespace
{
    thread_local bool t_counted = false;
    std::atomic<uint64_t> g_allocs{0};
} // namespace

#if !LLMBRIDGE_ALLOC_UNDER_TSAN
namespace
{
    void* counted_alloc(std::size_t n)
    {
        if (t_counted) g_allocs.fetch_add(1, std::memory_order_relaxed);
        if (void* p = std::malloc(n ? n : 1)) return p;
        throw std::bad_alloc();
    }

    void* counted_aligned(std::size_t n, std::align_val_t a)
    {
        if (t_counted) g_allocs.fetch_add(1, std::memory_order_relaxed);
        const auto al = static_cast<std::size_t>(a);
        if (void* p = std::aligned_alloc(al, (n + al - 1) / al * al)) return p;
        throw std::bad_alloc();
    }
} // namespace

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void* operator new(std::size_t n, std::align_val_t a) { return counted_aligned(n, a); }
void* operator new[](std::size_t n, std::align_val_t a) { return counted_aligned(n, a); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    try { return counted_alloc(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    try { return counted_alloc(n); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif

namespace
{
    using llmbridge::Gateway;
    using llmbridge::IoBackend;
    using llmbridge::UpstreamDialect;

    int listen_any(uint16_t& port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), len) != 0 || ::listen(fd, 64) != 0) return -1;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        return fd;
    }

    bool write_all(int fd, std::string_view s)
    {
        while (!s.empty())
        {
            const ssize_t n = ::send(fd, s.data(), s.size(), MSG_NOSIGNAL);
            if (n <= 0) return false;
            s.remove_prefix(static_cast<size_t>(n));
        }
        return true;
    }

    size_t content_length(std::string_view head)
    {
        const size_t at = head.find("Content-Length: ");
        return at == std::string_view::npos ? 0 : std::strtoul(head.data() + at + 16, nullptr, 10);
    }

    // Keep-alive upstream: answers every request on a connection with one fixed reply.
    class Upstream
    {
    public:
        explicit Upstream(std::string reply) : _reply(std::move(reply))
        {
            _fd = listen_any(_port);
            _th = std::thread([this] { accept_loop(); });
        }
        ~Upstream()
        {
            ::shutdown(_fd, SHUT_RDWR);
            ::close(_fd);
            _th.join();
            for (auto& t : _conns) t.join();
        }
        uint16_t port() const { return _port; }

    private:
        void accept_loop()
        {
            for (;;)
            {
                const int c = ::accept(_fd, nullptr, nullptr);
                if (c < 0) return;
                _conns.emplace_back([this, c] { serve(c); });
            }
        }
        void serve(int c)
        {
            std::string buf;
            char tmp[16384];
            for (;;)
            {
                size_t end;
                while ((end = buf.find("\r\n\r\n")) == std::string::npos ||
                       buf.size() < end + 4 + content_length(buf.substr(0, end)))
                {
                    const ssize_t n = ::read(c, tmp, sizeof(tmp));
                    if (n <= 0) { ::close(c); return; }
                    buf.append(tmp, static_cast<size_t>(n));
                }
                buf.erase(0, end + 4 + content_length(buf.substr(0, end)));
                if (!write_all(c, _reply)) { ::close(c); return; }
            }
        }

        std::string _reply;
        int _fd = -1;
        uint16_t _port = 0;
        std::thread _th;
        std::vector<std::thread> _conns;
    };

    // One keep-alive client connection; reads a Content-Length or a chunked reply.
    class Client
    {
    public:
        explicit Client(uint16_t port)
        {
            _fd = ::socket(AF_INET, SOCK_STREAM, 0);
            const int one = 1;
            ::setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            _ok = ::connect(_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
        }
        ~Client() { ::close(_fd); }

        // The status code of one full reply, or 0 when the connection broke.
        int roundtrip(std::string_view request)
        {
            if (!_ok || !write_all(_fd, request)) return 0;
            std::string buf;
            char tmp[16384];
            for (;;)
            {
                const size_t end = buf.find("\r\n\r\n");
                if (end != std::string::npos)
                {
                    const std::string_view head(buf.data(), end);
                    const bool chunked = head.find("Transfer-Encoding: chunked") != std::string_view::npos;
                    const bool done = chunked ? buf.find("\r\n0\r\n\r\n", end) != std::string::npos
                                              : buf.size() >= end + 4 + content_length(head);
                    if (done) return std::atoi(buf.c_str() + 9);
                }
                const ssize_t n = ::read(_fd, tmp, sizeof(tmp));
                if (n <= 0) return 0;
                buf.append(tmp, static_cast<size_t>(n));
            }
        }

    private:
        int _fd = -1;
        bool _ok = false;
    };

    std::string post(std::string_view body)
    {
        return "POST /v1/chat/completions HTTP/1.1\r\nHost: gw\r\nAuthorization: Bearer sk-test\r\n"
               "Content-Type: application/json\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
    }

    std::string reply(std::string_view content_type, std::string_view body)
    {
        return "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(content_type) +
               "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
    }

    std::string anthropic_stream(int deltas)
    {
        std::string ev =
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
            "\"model\":\"claude-x\",\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}}\n\n"
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
            "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
        for (int i = 0; i < deltas; ++i)
            ev += "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
                  "\"delta\":{\"type\":\"text_delta\",\"text\":\"word \"}}\n\n";
        ev += "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
              "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
              "\"end_turn\"},\"usage\":{\"output_tokens\":12}}\n\n"
              "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
        char size[32];
        std::snprintf(size, sizeof(size), "%zx\r\n", ev.size());
        return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n" +
               std::string(size) + ev + "\r\n0\r\n\r\n";
    }

    constexpr std::string_view kChat = R"({"model":"m","messages":[{"role":"user","content":"hi"}]})";
    constexpr std::string_view kChatStream =
        R"({"model":"m","stream":true,"messages":[{"role":"user","content":"hi"}]})";
    constexpr std::string_view kOpenAiReply =
        R"({"id":"c1","object":"chat.completion","created":1,"model":"m","choices":[{"index":0,)"
        R"("message":{"role":"assistant","content":"hello"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":5,"completion_tokens":2,"total_tokens":7}})";
    constexpr std::string_view kAnthropicReply =
        R"({"id":"msg_1","type":"message","role":"assistant","model":"claude-x","content":)"
        R"([{"type":"text","text":"hello"}],"stop_reason":"end_turn",)"
        R"("usage":{"input_tokens":5,"output_tokens":2}})";

    // Allocations per request on the loop thread, averaged after a warm-up that
    // lets every reused buffer reach its steady-state capacity.
    double allocs_per_request(IoBackend io, UpstreamDialect dialect, std::string upstream_reply,
                              const std::string& request)
    {
        constexpr int kWarm = 20, kMeasured = 50;
        Upstream up(std::move(upstream_reply));
        Gateway gw(0, "127.0.0.1", up.port(), 0, dialect, io);
        std::thread loop([&] { t_counted = true; gw.run(); });
        double per = -1;
        {
            Client c(gw.bound_port());
            for (int i = 0; i < kWarm; ++i) EXPECT_EQ(c.roundtrip(request), 200);
            const uint64_t before = g_allocs.load();
            for (int i = 0; i < kMeasured; ++i) EXPECT_EQ(c.roundtrip(request), 200);
            per = static_cast<double>(g_allocs.load() - before) / kMeasured;
        }
        gw.request_stop();
        loop.join();
        return per;
    }

    struct Ceiling
    {
        double passthrough, translated, stream_20, per_delta;
    };

    // Measured 2026-10-10 at v0.74.0 (GCC 13 Release and ASan Debug agree), after the
    // response translators began writing into kept buffers and the stream translator
    // was held by value: 12 -> 3, 7 -> 3, every one of them on the request side.
    // v0.78.0 writes the Anthropic request front to back, without a prefix string:
    // 3 -> 2 on both translated rows.
    // Lower these when a change removes allocations; never raise them without a
    // reason in the commit message. io_uring's per-delta figure can read 0.02, not 0:
    // its 4 KiB receive buffers split the stream at different points.
    constexpr Ceiling kEpoll{3, 2, 2, 0.03};
    constexpr Ceiling kUring{3, 2, 2, 0.03};

    class AllocCeiling : public ::testing::TestWithParam<IoBackend>
    {
    protected:
        void SetUp() override
        {
            if (LLMBRIDGE_ALLOC_UNDER_TSAN) GTEST_SKIP() << "TSan owns operator new";
        }
        const Ceiling& ceiling() const { return GetParam() == IoBackend::Epoll ? kEpoll : kUring; }
    };

    TEST_P(AllocCeiling, PassthroughRequest)
    {
        const double n = allocs_per_request(GetParam(), UpstreamDialect::OpenAI,
                                            reply("application/json", kOpenAiReply), post(kChat));
        std::printf("passthrough: %.2f allocations per request\n", n);
        EXPECT_LE(n, ceiling().passthrough);
    }

    TEST_P(AllocCeiling, TranslatedRequest)
    {
        const double n = allocs_per_request(GetParam(), UpstreamDialect::Anthropic,
                                            reply("application/json", kAnthropicReply), post(kChat));
        std::printf("translated: %.2f allocations per request\n", n);
        EXPECT_LE(n, ceiling().translated);
    }

    TEST_P(AllocCeiling, TranslatedStream)
    {
        const double n20 = allocs_per_request(GetParam(), UpstreamDialect::Anthropic,
                                              anthropic_stream(20), post(kChatStream));
        const double n120 = allocs_per_request(GetParam(), UpstreamDialect::Anthropic,
                                               anthropic_stream(120), post(kChatStream));
        const double per_delta = (n120 - n20) / 100;
        std::printf("translated stream: %.2f allocations per 20-delta request, %.2f per delta\n",
                    n20, per_delta);
        EXPECT_LE(n20, ceiling().stream_20);
        EXPECT_LE(per_delta, ceiling().per_delta);
    }

    INSTANTIATE_TEST_SUITE_P(Backends, AllocCeiling, ::testing::Values(IoBackend::Epoll, IoBackend::Uring),
                             [](const testing::TestParamInfo<IoBackend>& i)
                             { return i.param == IoBackend::Epoll ? "epoll" : "uring"; });
} // namespace
