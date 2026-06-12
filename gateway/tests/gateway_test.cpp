// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Integration tests for the Gateway. A real in-process backend (blocking
// thread) + the Gateway event loop running on its own thread, driven by a
// loopback client. Covers: round-trip correctness, response-body integrity,
// keep-alive, multi-client + upstream-pool reuse, warm-up gating,
// upstream-refused error, and clean shutdown.

#include "gateway/gateway.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "net/http.hpp"

using llmbridge::Gateway;
using llmbridge::TranslateMode;

namespace
{
    const std::string kRespBody = "pong";
    std::string canned_response()
    {
        return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
               std::to_string(kRespBody.size()) + "\r\nConnection: keep-alive\r\n\r\n" + kRespBody;
    }

    std::string make_request(const std::string& body = "hello", bool keep_alive = true)
    {
        return "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n" +
               std::string(keep_alive ? "" : "Connection: close\r\n") +
               "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }

    // Wrap a JSON body as a 200 response (keep-alive). For translate-mode tests the
    // backend must speak the upstream provider's dialect.
    std::string http_ok(const std::string& body)
    {
        return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    // Same, but the upstream declares Connection: close — the gateway must not pool it.
    std::string http_close(const std::string& body)
    {
        return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    }

    // A minimal valid OpenAI chat-completion request carrying `content`.
    std::string openai_request(const std::string& content, bool keep_alive = true)
    {
        return make_request(
            "{\"model\":\"gpt-4o\",\"max_tokens\":64,\"messages\":[{\"role\":\"user\",\"content\":\"" +
                content + "\"}]}",
            keep_alive);
    }

    // Provider-dialect response BODIES whose assistant text is `text`.
    std::string anthropic_resp_body(const std::string& text)
    {
        return "{\"id\":\"msg_x\",\"model\":\"claude-3-5-sonnet\",\"stop_reason\":\"end_turn\","
               "\"content\":[{\"type\":\"text\",\"text\":\"" + text + "\"}],"
               "\"usage\":{\"input_tokens\":3,\"output_tokens\":5}}";
    }
    std::string gemini_resp_body(const std::string& text)
    {
        return "{\"modelVersion\":\"gemini-2.0\",\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"" +
               text + "\"}]},\"finishReason\":\"STOP\"}],"
               "\"usageMetadata\":{\"promptTokenCount\":3,\"candidatesTokenCount\":5,\"totalTokenCount\":8}}";
    }
    std::string cohere_resp_body(const std::string& text)
    {
        return "{\"id\":\"c_x\",\"model\":\"command-r\",\"finish_reason\":\"COMPLETE\","
               "\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"" + text + "\"}]},"
               "\"usage\":{\"tokens\":{\"input_tokens\":3,\"output_tokens\":5}}}";
    }

    uint16_t free_port()
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        socklen_t len = sizeof(a);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        uint16_t p = ntohs(a.sin_port);
        ::close(fd);
        return p;
    }

    // Blocking backend that answers each framed request with the canned response.
    class TestBackend
    {
    public:
        void start()
        {
            _fd = ::socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = 0;
            ::bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
            socklen_t len = sizeof(a);
            ::getsockname(_fd, reinterpret_cast<sockaddr*>(&a), &len);
            _port = ntohs(a.sin_port);
            ::listen(_fd, 128);
            _acc = std::thread([this] { accept_loop(); });
        }

        void stop()
        {
            _stop = true;
            if (_fd >= 0) ::shutdown(_fd, SHUT_RDWR); // unblock the accept loop
            if (_acc.joinable()) _acc.join();          // accept_loop has stopped touching _fd
            if (_fd >= 0) { ::close(_fd); _fd = -1; }  // now safe to close/null it
            // The proxy keeps upstream connections open (keep-alive pool), so the
            // handler threads are parked in a blocking read() that _stop alone
            // won't interrupt — shut the accepted fds down to unblock them.
            {
                std::lock_guard<std::mutex> lk(_mu);
                for (int fd : _client_fds) ::shutdown(fd, SHUT_RDWR);
            }
            for (auto& t : _conns) if (t.joinable()) t.join();
        }

        uint16_t port() const { return _port; }

        // Test knobs (set before start(), or before the request that uses them).
        void set_response(std::string r) { _resp_override = std::move(r); }
        void set_trickle(int chunk) { _trickle_chunk = chunk; }        // write reply in chunks
        void set_close_mid_response(bool b) { _close_mid = b; }         // simulate upstream abort
        // Respond once (keep-alive), then close the connection — simulates a
        // provider dropping an idle pooled keep-alive connection.
        void set_close_after_first(bool b) { _close_after_first = b; }
        int requests_seen() const { return _requests_seen.load(); }
        std::string last_request()
        {
            std::lock_guard<std::mutex> lk(_mu);
            return _last_request;
        }

    private:
        void accept_loop()
        {
            for (;;)
            {
                int c = ::accept(_fd, nullptr, nullptr);
                if (c < 0) return;
                { std::lock_guard<std::mutex> lk(_mu); _client_fds.push_back(c); }
                _conns.emplace_back([this, c] { handle(c); });
            }
        }
        void handle(int c)
        {
            std::string buf;
            char tmp[16384];
            while (!_stop)
            {
                llmbridge::http::Message m;
                while (llmbridge::http::parse(buf, m) != llmbridge::http::ParseStatus::Complete)
                {
                    ssize_t n = ::read(c, tmp, sizeof(tmp));
                    if (n <= 0) { ::close(c); return; }
                    buf.append(tmp, static_cast<size_t>(n));
                }
                { std::lock_guard<std::mutex> lk(_mu); _last_request.assign(buf.data(), m.total_len); }
                _requests_seen.fetch_add(1, std::memory_order_relaxed);
                buf.erase(0, m.total_len);

                const std::string resp = _resp_override.empty() ? canned_response() : _resp_override;
                if (_close_mid)
                {
                    // Send a partial response then drop — exercises the gateway's
                    // upstream-abort path.
                    (void)!::write(c, resp.data(), resp.size() / 2 + 1);
                    ::close(c);
                    return;
                }
                bool ok = true;
                if (_trickle_chunk > 0)
                {
                    const size_t chunk = static_cast<size_t>(_trickle_chunk);
                    for (size_t off = 0; off < resp.size() && ok; off += chunk)
                    {
                        const size_t len = std::min(chunk, resp.size() - off);
                        if (::write(c, resp.data() + off, len) < 0) ok = false;
                        timespec ts{0, 200000}; // 0.2 ms: force the gateway to see partial reads
                        nanosleep(&ts, nullptr);
                    }
                }
                else
                {
                    ok = ::write(c, resp.data(), resp.size()) >= 0;
                }
                if (!ok) { ::close(c); return; }
                if (_close_after_first) { ::close(c); return; } // drop the "idle" keep-alive
                if (!m.keep_alive) { ::close(c); return; }
            }
            ::close(c);
        }

        int _fd = -1;
        uint16_t _port = 0;
        std::string _resp_override;
        int _trickle_chunk = 0;
        bool _close_mid = false;
        bool _close_after_first = false;
        std::atomic<int> _requests_seen{0};
        std::string _last_request;
        std::atomic<bool> _stop{false};
        std::thread _acc;
        std::vector<std::thread> _conns;
        std::mutex _mu;
        std::vector<int> _client_fds;
    };

    // Blocking loopback client with response framing.
    class Client
    {
    public:
        bool connect(uint16_t port)
        {
            _fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(port);
            return ::connect(_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
        }
        bool send(const std::string& s)
        {
            size_t off = 0;
            while (off < s.size())
            {
                ssize_t n = ::write(_fd, s.data() + off, s.size() - off);
                if (n <= 0) return false;
                off += static_cast<size_t>(n);
            }
            return true;
        }
        // Dribble the bytes out in tiny chunks to force the gateway's framer through
        // the NeedMore path (partial reads mid-request).
        bool send_trickle(const std::string& s, size_t chunk = 1)
        {
            for (size_t off = 0; off < s.size(); off += chunk)
            {
                const size_t len = std::min(chunk, s.size() - off);
                if (::write(_fd, s.data() + off, len) <= 0) return false;
                timespec ts{0, 150000};
                nanosleep(&ts, nullptr);
            }
            return true;
        }
        // Read one framed response; "" on closed/timeout.
        std::string recv_response(int timeout_ms = 2000)
        {
            char tmp[8192];
            for (;;)
            {
                llmbridge::http::Message m;
                if (llmbridge::http::parse(_buf, m) == llmbridge::http::ParseStatus::Complete)
                {
                    std::string out = _buf.substr(0, m.total_len);
                    _buf.erase(0, m.total_len);
                    return out;
                }
                pollfd p{_fd, POLLIN, 0};
                if (::poll(&p, 1, timeout_ms) <= 0) return "";
                ssize_t n = ::read(_fd, tmp, sizeof(tmp));
                if (n <= 0) return "";
                _buf.append(tmp, static_cast<size_t>(n));
            }
        }
        // Returns true if the peer closed within the timeout (read -> 0).
        bool wait_closed(int timeout_ms = 2000)
        {
            char tmp[256];
            pollfd p{_fd, POLLIN, 0};
            if (::poll(&p, 1, timeout_ms) <= 0) return false;
            return ::read(_fd, tmp, sizeof(tmp)) == 0;
        }
        void close() { if (_fd >= 0) { ::close(_fd); _fd = -1; } }
        ~Client() { close(); }

    private:
        int _fd = -1;
        std::string _buf;
    };

    // Fixture: backend + Gateway-in-a-thread.
    class ProxyIT : public ::testing::Test
    {
    protected:
        void start(int64_t warmup_ns = 0, bool with_backend = true,
                   TranslateMode translate = TranslateMode::None,
                   llmbridge::IoBackend backend = llmbridge::IoBackend::Epoll)
        {
            uint16_t up_port;
            if (with_backend) { _backend.start(); up_port = _backend.port(); }
            else up_port = free_port(); // nothing listening -> connection refused

            _gw = std::make_unique<Gateway>(0, "127.0.0.1", up_port, warmup_ns, translate, backend);
            _proxy_port = _gw->bound_port();
            _gt = std::thread([this] { _gw->run(); });
        }
        void shutdown()
        {
            if (_shut) return;
            _shut = true;
            if (_gw) _gw->request_stop();
            if (_gt.joinable()) _gt.join();
            _backend.stop();
        }
        void TearDown() override { shutdown(); }

        TestBackend _backend;
        std::unique_ptr<Gateway> _gw;
        std::thread _gt;
        uint16_t _proxy_port = 0;
        bool _shut = false;
    };
} // namespace

TEST_F(ProxyIT, SingleRequestRoundTrip)
{
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    std::string resp = c.recv_response();
    EXPECT_FALSE(resp.empty());
    EXPECT_NE(resp.find("200 OK"), std::string::npos);
    EXPECT_NE(resp.find(kRespBody), std::string::npos);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 1u);
    EXPECT_EQ(_gw->stats().errors, 0u);
}

TEST_F(ProxyIT, ResponseBodyIntegrity)
{
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    std::string resp = c.recv_response();
    llmbridge::http::Message m;
    ASSERT_EQ(llmbridge::http::parse(resp, m), llmbridge::http::ParseStatus::Complete);
    EXPECT_EQ(resp.substr(m.header_len), kRespBody);
}

class ProxyKeepAlive : public ProxyIT, public ::testing::WithParamInterface<int> {};
TEST_P(ProxyKeepAlive, MultipleRequestsOnOneConnection)
{
    const int n = GetParam();
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    for (int i = 0; i < n; ++i)
    {
        ASSERT_TRUE(c.send(make_request("req" + std::to_string(i))));
        std::string resp = c.recv_response();
        ASSERT_NE(resp.find("200 OK"), std::string::npos) << "request " << i;
    }
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, static_cast<uint64_t>(n));
}
INSTANTIATE_TEST_SUITE_P(Counts, ProxyKeepAlive, ::testing::Values(1, 2, 3, 5, 10, 25),
                         [](const testing::TestParamInfo<int>& i) {
                             return "n" + std::to_string(i.param);
                         });

class ProxyMultiClient : public ProxyIT, public ::testing::WithParamInterface<int> {};
TEST_P(ProxyMultiClient, EachClientGetsResponse)
{
    const int n = GetParam();
    start();
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < n; ++i)
    {
        auto c = std::make_unique<Client>();
        ASSERT_TRUE(c->connect(_proxy_port));
        ASSERT_TRUE(c->send(make_request()));
        clients.push_back(std::move(c));
    }
    for (int i = 0; i < n; ++i)
        EXPECT_NE(clients[i]->recv_response().find("200 OK"), std::string::npos) << "client " << i;
    for (auto& c : clients) c->close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, static_cast<uint64_t>(n));
    EXPECT_GE(_gw->stats().upstream_conns_opened, 1u);
    EXPECT_LE(_gw->stats().upstream_conns_opened, static_cast<uint64_t>(n)); // pool reuse
}
INSTANTIATE_TEST_SUITE_P(Counts, ProxyMultiClient, ::testing::Values(2, 5, 10, 20),
                         [](const testing::TestParamInfo<int>& i) {
                             return "n" + std::to_string(i.param);
                         });

TEST_F(ProxyIT, PoolReuseAcrossSequentialClients)
{
    start();
    for (int i = 0; i < 8; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(make_request()));
        ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos);
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 8u);
    // Sequential clients should mostly reuse one or a few upstream connections.
    EXPECT_LE(_gw->stats().upstream_conns_opened, 4u);
}

TEST_F(ProxyIT, WarmupGatingExcludesEarlyRequests)
{
    start(/*warmup_ns=*/5'000'000'000LL); // 5 s warm-up; test sends well within it
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(c.send(make_request()));
        ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos);
    }
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 0u); // all within warm-up -> not counted
}

TEST_F(ProxyIT, UpstreamRefusedClosesClientAndCountsError)
{
    start(/*warmup_ns=*/0, /*with_backend=*/false); // upstream port has no listener
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_TRUE(c.wait_closed()); // proxy aborts the client when upstream connect fails
    c.close();
    shutdown();
    EXPECT_GE(_gw->stats().errors, 1u);
    EXPECT_EQ(_gw->stats().requests, 0u);
}

TEST_F(ProxyIT, ShutdownWithoutTrafficTerminatesGraph)
{
    start();
    // No requests sent; shutdown must still join the graph thread promptly.
    shutdown();
    SUCCEED();
}

TEST_F(ProxyIT, ConnectionCloseHeaderClosesClientAfterResponse)
{
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request("bye", /*keep_alive=*/false)));
    EXPECT_NE(c.recv_response().find("200 OK"), std::string::npos);
    EXPECT_TRUE(c.wait_closed()); // proxy honors Connection: close
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 1u);
}

// ════════════════════════════════════════════════════════════════════════════
// Extended correctness suite — the backend-agnostic gate that BOTH the epoll loop
// and (once it lands) the io_uring loop must pass identically. Drives the public
// Gateway over real loopback sockets: translation round-trips, large bodies,
// pipelining, partial/trickled I/O framing, and the error/abort paths.
// ════════════════════════════════════════════════════════════════════════════

namespace
{
    std::string provider_resp_body(TranslateMode m, const std::string& text)
    {
        switch (m)
        {
            case TranslateMode::Anthropic: return anthropic_resp_body(text);
            case TranslateMode::Gemini: return gemini_resp_body(text);
            case TranslateMode::Cohere: return cohere_resp_body(text);
            default: return {};
        }
    }
    const char* mode_name(TranslateMode m)
    {
        switch (m)
        {
            case TranslateMode::Anthropic: return "Anthropic";
            case TranslateMode::Gemini: return "Gemini";
            case TranslateMode::Cohere: return "Cohere";
            default: return "None";
        }
    }

    std::string body_of(const std::string& resp)
    {
        llmbridge::http::Message m;
        if (llmbridge::http::parse(resp, m) != llmbridge::http::ParseStatus::Complete) return {};
        return resp.substr(m.header_len, m.body_len);
    }
} // namespace

// ── Translation round-trips: OpenAI in, provider out, OpenAI back ──────────────
class ProxyTranslateMode : public ProxyIT, public ::testing::WithParamInterface<TranslateMode> {};

TEST_P(ProxyTranslateMode, RoundTripsToOpenAIShape)
{
    const TranslateMode mode = GetParam();
    _backend.set_response(http_ok(provider_resp_body(mode, "pong")));
    start(0, true, mode);

    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("ping")));
    const std::string body = body_of(c.recv_response());

    EXPECT_NE(body.find("\"object\":\"chat.completion\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"content\":\"pong\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"finish_reason\":\"stop\""), std::string::npos) << body;
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 1u);
    EXPECT_EQ(_gw->stats().errors, 0u);
}

TEST_P(ProxyTranslateMode, ForwardsTranslatedRequestUpstream)
{
    const TranslateMode mode = GetParam();
    _backend.set_response(http_ok(provider_resp_body(mode, "ok")));
    start(0, true, mode);

    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("unique-marker-content")));
    (void)c.recv_response();
    const std::string upstream = _backend.last_request();

    // Upstream saw the provider-shaped, content-preserving request.
    EXPECT_NE(upstream.find("unique-marker-content"), std::string::npos) << upstream;
    if (mode == TranslateMode::Anthropic) EXPECT_NE(upstream.find("/v1/messages"), std::string::npos);
    if (mode == TranslateMode::Gemini) EXPECT_NE(upstream.find("generateContent"), std::string::npos);
    if (mode == TranslateMode::Cohere) EXPECT_NE(upstream.find("/v2/chat"), std::string::npos);
    c.close();
    shutdown();
}

INSTANTIATE_TEST_SUITE_P(Dialects, ProxyTranslateMode,
                         ::testing::Values(TranslateMode::Anthropic, TranslateMode::Gemini,
                                           TranslateMode::Cohere),
                         [](const testing::TestParamInfo<TranslateMode>& i) { return mode_name(i.param); });

// ── Large bodies ───────────────────────────────────────────────────────────────
class ProxySize : public ProxyIT, public ::testing::WithParamInterface<size_t> {};

TEST_P(ProxySize, LargeResponsePassthroughIntact)
{
    const std::string big(GetParam(), 'R');
    _backend.set_response(http_ok(big));
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request("hi")));
    EXPECT_EQ(body_of(c.recv_response()), big);
    c.close();
    shutdown();
}

TEST_P(ProxySize, LargeRequestForwardedIntact)
{
    const std::string big(GetParam(), 'Q');
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    const std::string req = make_request(big);
    ASSERT_TRUE(c.send(req));
    ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos);
    EXPECT_EQ(_backend.last_request(), req); // byte-exact passthrough
    c.close();
    shutdown();
}

INSTANTIATE_TEST_SUITE_P(Sizes, ProxySize,
                         ::testing::Values(size_t{1024}, size_t{8192}, size_t{65536}, size_t{262144}),
                         [](const testing::TestParamInfo<size_t>& i) {
                             return "b" + std::to_string(i.param);
                         });

TEST_F(ProxyIT, LargeTranslateRoundTrip)
{
    const std::string long_reply(40000, 'Z');
    _backend.set_response(http_ok(anthropic_resp_body(long_reply)));
    start(0, true, TranslateMode::Anthropic);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request(std::string(20000, 'U'))));
    const std::string body = body_of(c.recv_response());
    EXPECT_NE(body.find("\"content\":\"" + long_reply + "\""), std::string::npos);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
}

// ── Pipelining: many requests in flight on one connection ──────────────────────
class ProxyPipeline : public ProxyIT, public ::testing::WithParamInterface<int> {};
TEST_P(ProxyPipeline, AllResponsesReturnedInOrder)
{
    const int n = GetParam();
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    std::string burst;
    for (int i = 0; i < n; ++i) burst += make_request("req" + std::to_string(i));
    ASSERT_TRUE(c.send(burst)); // all at once, before reading any response
    for (int i = 0; i < n; ++i)
        ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos) << "response " << i;
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, static_cast<uint64_t>(n));
    EXPECT_EQ(_backend.requests_seen(), n);
}
INSTANTIATE_TEST_SUITE_P(Depths, ProxyPipeline, ::testing::Values(2, 5, 16, 40),
                         [](const testing::TestParamInfo<int>& i) { return "n" + std::to_string(i.param); });

// ── Partial / trickled I/O: framing must survive byte-at-a-time arrival ────────
TEST_F(ProxyIT, TrickledClientRequestIsFramed)
{
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send_trickle(make_request("dribble"), 1)); // one byte at a time
    EXPECT_NE(c.recv_response().find("200 OK"), std::string::npos);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 1u);
}

TEST_F(ProxyIT, TrickledUpstreamResponseIsReassembled)
{
    _backend.set_trickle(3); // backend dribbles the response in 3-byte chunks
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request("hi")));
    EXPECT_EQ(body_of(c.recv_response()), kRespBody);
    c.close();
    shutdown();
}

// ── Error / abort paths ────────────────────────────────────────────────────────
TEST_F(ProxyIT, MalformedRequestClosedAndGatewaySurvives)
{
    start();
    {
        Client bad;
        ASSERT_TRUE(bad.connect(_proxy_port));
        // Valid framing, invalid Content-Length -> http::parse Error.
        ASSERT_TRUE(bad.send("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: notanumber\r\n\r\n"));
        EXPECT_TRUE(bad.wait_closed());
        bad.close();
    }
    // A fresh client must still be served — the loop survived the bad input.
    Client good;
    ASSERT_TRUE(good.connect(_proxy_port));
    ASSERT_TRUE(good.send(make_request("ok")));
    EXPECT_NE(good.recv_response().find("200 OK"), std::string::npos);
    good.close();
    shutdown();
    EXPECT_GE(_gw->stats().errors, 1u);
    EXPECT_EQ(_gw->stats().requests, 1u);
}

TEST_F(ProxyIT, MalformedTranslateBodyClosedAndGatewaySurvives)
{
    start(0, true, TranslateMode::Anthropic);
    {
        Client bad;
        ASSERT_TRUE(bad.connect(_proxy_port));
        ASSERT_TRUE(bad.send(make_request("this is not json {{{")));
        EXPECT_TRUE(bad.wait_closed()); // translate returns empty -> client aborted
        bad.close();
    }
    _backend.set_response(http_ok(anthropic_resp_body("recovered")));
    Client good;
    ASSERT_TRUE(good.connect(_proxy_port));
    ASSERT_TRUE(good.send(openai_request("ping")));
    EXPECT_NE(body_of(good.recv_response()).find("recovered"), std::string::npos);
    good.close();
    shutdown();
    EXPECT_GE(_gw->stats().errors, 1u);
}

TEST_F(ProxyIT, UpstreamClosesMidResponseAbortsClient)
{
    _backend.set_close_mid_response(true);
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request("hi")));
    // The proxy never gets a complete upstream response -> it aborts the client.
    EXPECT_TRUE(c.recv_response().empty());
    c.close();
    shutdown();
    EXPECT_GE(_gw->stats().errors, 1u);
}

// ── Load & lifecycle ───────────────────────────────────────────────────────────
class ProxyLoad : public ProxyIT, public ::testing::WithParamInterface<int> {};
TEST_P(ProxyLoad, ManyClientsLargeBodies)
{
    const int n = GetParam();
    const std::string big(8192, 'B');
    start();
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < n; ++i)
    {
        auto c = std::make_unique<Client>();
        ASSERT_TRUE(c->connect(_proxy_port));
        ASSERT_TRUE(c->send(make_request(big)));
        clients.push_back(std::move(c));
    }
    for (int i = 0; i < n; ++i)
        EXPECT_NE(clients[i]->recv_response().find("200 OK"), std::string::npos) << "client " << i;
    for (auto& c : clients) c->close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, static_cast<uint64_t>(n));
}
INSTANTIATE_TEST_SUITE_P(Counts, ProxyLoad, ::testing::Values(10, 30, 60),
                         [](const testing::TestParamInfo<int>& i) { return "n" + std::to_string(i.param); });

TEST_F(ProxyIT, ConnectionChurnNoLeak)
{
    start();
    for (int i = 0; i < 60; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port)) << "iter " << i;
        ASSERT_TRUE(c.send(make_request("x", /*keep_alive=*/false)));
        ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos) << "iter " << i;
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 60u);
}

TEST_F(ProxyIT, EscapedContentSurvivesTranslateForward)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    // Content with escaped quotes, backslash and newline.
    ASSERT_TRUE(c.send(openai_request("she said \\\"hi\\\" then\\nleft C:\\\\tmp")));
    (void)c.recv_response();
    const std::string upstream = _backend.last_request();
    EXPECT_NE(upstream.find("she said \\\"hi\\\" then\\nleft C:\\\\tmp"), std::string::npos) << upstream;
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
}

TEST_F(ProxyIT, EmptyContentTranslateRoundTrip)
{
    _backend.set_response(http_ok(anthropic_resp_body("")));
    start(0, true, TranslateMode::Anthropic);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("")));
    EXPECT_NE(c.recv_response().find("\"object\":\"chat.completion\""), std::string::npos);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
}

// ════════════════════════════════════════════════════════════════════════════
// Backend parity: the core scenarios must behave identically on epoll AND
// io_uring (Phase 1). Each test runs under both — if io_uring is unavailable the
// Gateway falls back to epoll, so this is always safe to instantiate.
// ════════════════════════════════════════════════════════════════════════════
namespace
{
    const char* be_name(llmbridge::IoBackend b)
    {
        return b == llmbridge::IoBackend::Uring ? "uring" : "epoll";
    }
} // namespace

class ProxyBackend : public ProxyIT, public ::testing::WithParamInterface<llmbridge::IoBackend> {};

TEST_P(ProxyBackend, RoundTripAndKeepAlive)
{
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    for (int i = 0; i < 5; ++i)
    {
        ASSERT_TRUE(c.send(make_request("req" + std::to_string(i))));
        EXPECT_EQ(body_of(c.recv_response()), kRespBody) << "iter " << i;
    }
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 5u);
    EXPECT_EQ(_gw->stats().errors, 0u);
}

TEST_P(ProxyBackend, MultipleClients)
{
    start(0, true, TranslateMode::None, GetParam());
    std::vector<std::unique_ptr<Client>> cs;
    for (int i = 0; i < 12; ++i)
    {
        auto c = std::make_unique<Client>();
        ASSERT_TRUE(c->connect(_proxy_port));
        ASSERT_TRUE(c->send(make_request("hi")));
        cs.push_back(std::move(c));
    }
    for (int i = 0; i < 12; ++i)
        EXPECT_NE(cs[i]->recv_response().find("200 OK"), std::string::npos) << "client " << i;
    for (auto& c : cs) c->close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 12u);
}

TEST_P(ProxyBackend, LargeResponseAndRequest)
{
    const std::string big(64 * 1024, 'Z');
    _backend.set_response(http_ok(big));
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    const std::string req = make_request(std::string(64 * 1024, 'Q'));
    ASSERT_TRUE(c.send(req));
    EXPECT_EQ(body_of(c.recv_response()), big);   // large response intact
    EXPECT_EQ(_backend.last_request(), req);       // large request forwarded intact
    c.close();
    shutdown();
}

TEST_P(ProxyBackend, Pipelining)
{
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    std::string burst;
    for (int i = 0; i < 16; ++i) burst += make_request("r" + std::to_string(i));
    ASSERT_TRUE(c.send(burst));
    for (int i = 0; i < 16; ++i)
        ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos) << "resp " << i;
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 16u);
}

TEST_P(ProxyBackend, TranslateRoundTripAndForward)
{
    _backend.set_response(http_ok(anthropic_resp_body("pong")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("unique-xyz")));
    const std::string body = body_of(c.recv_response());
    EXPECT_NE(body.find("\"content\":\"pong\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"finish_reason\":\"stop\""), std::string::npos);
    EXPECT_NE(_backend.last_request().find("unique-xyz"), std::string::npos);
    EXPECT_NE(_backend.last_request().find("/v1/messages"), std::string::npos);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
}

TEST_P(ProxyBackend, TrickledClientAndUpstream)
{
    _backend.set_trickle(3); // backend dribbles its reply
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send_trickle(make_request("dribble"), 1)); // client dribbles its request
    EXPECT_EQ(body_of(c.recv_response()), kRespBody);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 1u);
}

TEST_P(ProxyBackend, MalformedRequestSurvives)
{
    start(0, true, TranslateMode::None, GetParam());
    {
        Client bad;
        ASSERT_TRUE(bad.connect(_proxy_port));
        ASSERT_TRUE(bad.send("POST / HTTP/1.1\r\nContent-Length: nope\r\n\r\n"));
        EXPECT_TRUE(bad.wait_closed());
        bad.close();
    }
    Client good;
    ASSERT_TRUE(good.connect(_proxy_port));
    ASSERT_TRUE(good.send(make_request("ok")));
    EXPECT_NE(good.recv_response().find("200 OK"), std::string::npos);
    good.close();
    shutdown();
    EXPECT_GE(_gw->stats().errors, 1u);
    EXPECT_EQ(_gw->stats().requests, 1u);
}

TEST_P(ProxyBackend, MalformedTranslateBodySurvives)
{
    start(0, true, TranslateMode::Anthropic, GetParam());
    {
        Client bad;
        ASSERT_TRUE(bad.connect(_proxy_port));
        ASSERT_TRUE(bad.send(make_request("not json {{{")));
        EXPECT_TRUE(bad.wait_closed());
        bad.close();
    }
    _backend.set_response(http_ok(anthropic_resp_body("recovered")));
    Client good;
    ASSERT_TRUE(good.connect(_proxy_port));
    ASSERT_TRUE(good.send(openai_request("ping")));
    EXPECT_NE(body_of(good.recv_response()).find("recovered"), std::string::npos);
    good.close();
    shutdown();
    EXPECT_GE(_gw->stats().errors, 1u);
}

TEST_P(ProxyBackend, UpstreamClosesMidResponseAborts)
{
    _backend.set_close_mid_response(true);
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request("hi")));
    EXPECT_TRUE(c.recv_response().empty());
    c.close();
    shutdown();
    EXPECT_GE(_gw->stats().errors, 1u);
}

TEST_P(ProxyBackend, ConnectionCloseHonored)
{
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request("bye", /*keep_alive=*/false)));
    EXPECT_NE(c.recv_response().find("200 OK"), std::string::npos);
    EXPECT_TRUE(c.wait_closed());
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 1u);
}

TEST_P(ProxyBackend, ShutdownMidRequestIsClean)
{
    // Hold the upstream so the request is in flight when we tear down — exercises
    // the drain path (and, under ASan, the no-UAF-on-pending-completion property).
    _backend.set_trickle(2);
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request("hi")));
    // Don't wait for the response; shut down with work outstanding.
    shutdown();
    SUCCEED();
}

TEST_P(ProxyBackend, ConnectRefusedAbortsClient)
{
    start(0, /*with_backend=*/false, TranslateMode::None, GetParam()); // no upstream listener
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request("hi")));
    EXPECT_TRUE(c.wait_closed()); // upstream connect fails -> client aborted
    c.close();
    shutdown();
    EXPECT_GE(_gw->stats().errors, 1u);
    EXPECT_EQ(_gw->stats().requests, 0u);
}

TEST_P(ProxyBackend, ClientDisconnectsBeforeReadingSurvives)
{
    start(0, true, TranslateMode::None, GetParam());
    for (int i = 0; i < 12; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(make_request("hi")));
        c.close(); // close immediately, never read the response (response-send fails)
    }
    // The loop must survive every aborted response: a normal client still works.
    Client good;
    ASSERT_TRUE(good.connect(_proxy_port));
    ASSERT_TRUE(good.send(make_request("ok")));
    EXPECT_NE(good.recv_response().find("200 OK"), std::string::npos);
    good.close();
    shutdown();
    SUCCEED();
}

TEST_P(ProxyBackend, SequentialPoolReuse)
{
    start(0, true, TranslateMode::None, GetParam());
    for (int i = 0; i < 12; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(make_request("x")));
        ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos) << "iter " << i;
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 12u);
    EXPECT_LE(_gw->stats().upstream_conns_opened, 4u); // pooled & reused (connected -> send, no reconnect)
}

TEST_P(ProxyBackend, ConnectionChurn)
{
    start(0, true, TranslateMode::None, GetParam());
    for (int i = 0; i < 80; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port)) << "iter " << i;
        ASSERT_TRUE(c.send(make_request("x", /*keep_alive=*/false)));
        ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos) << "iter " << i;
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 80u);
}

TEST_P(ProxyBackend, HighConcurrency)
{
    const int n = 100;
    const std::string body(4096, 'B');
    start(0, true, TranslateMode::None, GetParam());
    std::vector<std::unique_ptr<Client>> cs;
    for (int i = 0; i < n; ++i)
    {
        auto c = std::make_unique<Client>();
        ASSERT_TRUE(c->connect(_proxy_port)) << i;
        ASSERT_TRUE(c->send(make_request(body)));
        cs.push_back(std::move(c));
    }
    for (int i = 0; i < n; ++i)
        EXPECT_NE(cs[i]->recv_response().find("200 OK"), std::string::npos) << "client " << i;
    for (auto& c : cs) c->close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, static_cast<uint64_t>(n));
}

TEST_P(ProxyBackend, VeryLargeBodyMultiOp)
{
    const std::string big(512 * 1024, 'Z'); // 32 recv chunks + many partial sends
    _backend.set_response(http_ok(big));
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    const std::string req = make_request(std::string(512 * 1024, 'Q'));
    ASSERT_TRUE(c.send(req));
    EXPECT_EQ(body_of(c.recv_response()), big);
    EXPECT_EQ(_backend.last_request(), req);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
}

TEST_P(ProxyBackend, DeepKeepAlive)
{
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    for (int i = 0; i < 64; ++i)
    {
        ASSERT_TRUE(c.send(make_request("r" + std::to_string(i))));
        ASSERT_EQ(body_of(c.recv_response()), kRespBody) << "iter " << i;
    }
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 64u);
}

TEST_P(ProxyBackend, WarmupGating)
{
    start(5'000'000'000LL, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(c.send(make_request("x")));
        ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos);
    }
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 0u); // all within warm-up -> not counted
}

TEST_P(ProxyBackend, ShutdownWithManyInFlightIsClean)
{
    _backend.set_trickle(4); // slow replies so requests are still in flight at teardown
    start(0, true, TranslateMode::None, GetParam());
    std::vector<std::unique_ptr<Client>> cs;
    for (int i = 0; i < 30; ++i)
    {
        auto c = std::make_unique<Client>();
        ASSERT_TRUE(c->connect(_proxy_port));
        ASSERT_TRUE(c->send(make_request("x")));
        cs.push_back(std::move(c)); // never read -> ~30 requests mid-flight
    }
    shutdown(); // exercises the drain path with many outstanding ops (ASan: no UAF)
    SUCCEED();
}

// ── Bug-1 regression (heavy): an upstream that WILL close must never be pooled or
// reused. Before the fix, io_uring pooled the corpse and the next request failed.
// 300/200 iterations so a single reuse-of-corpse trips an assertion.
TEST_P(ProxyBackend, StaleUpstreamNeverReused_ClientClose)
{
    start(0, true, TranslateMode::None, GetParam());
    for (int i = 0; i < 300; ++i) // each request forwards Connection: close -> backend closes
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port)) << i;
        ASSERT_TRUE(c.send(make_request("x", /*keep_alive=*/false)));
        ASSERT_EQ(body_of(c.recv_response()), kRespBody) << "iter " << i; // never a corpse reuse
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 300u);
    EXPECT_EQ(_gw->stats().errors, 0u);
}

TEST_P(ProxyBackend, StaleUpstreamNeverReused_BackendClose)
{
    _backend.set_response(http_close(kRespBody)); // upstream declares Connection: close
    start(0, true, TranslateMode::None, GetParam());
    for (int i = 0; i < 200; ++i) // keep-alive clients, but the upstream closes each time
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port)) << i;
        ASSERT_TRUE(c.send(make_request("x")));
        ASSERT_EQ(body_of(c.recv_response()), kRespBody) << "iter " << i;
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 200u);
    EXPECT_EQ(_gw->stats().errors, 0u);
}

// ── Stale-connection retry (heavy): the provider pools a keep-alive connection
// then drops it idle. The gateway must transparently resend on a fresh
// connection. epoll recovers via pool eviction; io_uring via the retry path —
// both must serve every request. 60 iterations => ~59 stale reuses on io_uring.
TEST_P(ProxyBackend, RetriesOnStalePooledConnection)
{
    _backend.set_response(http_ok(anthropic_resp_body("pong")));
    _backend.set_close_after_first(true); // respond keep-alive, then drop the connection
    start(0, true, TranslateMode::Anthropic, GetParam());
    for (int i = 0; i < 60; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port)) << i;
        ASSERT_TRUE(c.send(openai_request("req" + std::to_string(i))));
        EXPECT_NE(body_of(c.recv_response()).find("\"content\":\"pong\""), std::string::npos)
            << "iter " << i << " (stale connection should have been retried)";
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u); // stale connections recovered, never surfaced
    // Both backends keep a recv armed on idle pooled upstreams, so between sequential
    // clients they EVICT the dead connection on EOF before reuse (occasionally retry
    // if the timing races). Either recovery is correct — we only require every
    // request was served. The pipelined variant below forces the retry path itself.
}

// Pipelined variant: two requests on ONE keep-alive connection. The 2nd is
// forwarded by reusing the pooled (already-dropped) upstream INLINE — before the
// event loop can evict it — so BOTH backends are forced through the retry path.
TEST_P(ProxyBackend, RetriesOnStalePooledConnectionPipelined)
{
    _backend.set_response(http_ok(anthropic_resp_body("pong")));
    _backend.set_close_after_first(true);
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    const std::string burst = openai_request("one") + openai_request("two");
    ASSERT_TRUE(c.send(burst));
    EXPECT_NE(body_of(c.recv_response()).find("\"content\":\"pong\""), std::string::npos) << "response 1";
    EXPECT_NE(body_of(c.recv_response()).find("\"content\":\"pong\""), std::string::npos) << "response 2";
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u); // both requests served despite the dropped conn
    // Recovery mechanism: epoll only processes the dead conn's EOF at epoll_wait —
    // after the inline reuse — so it deterministically RETRIES. io_uring's always-
    // armed recv may instead evict the dead conn first (a completion-order race), so
    // we assert the retry path only where it's deterministic.
    if (GetParam() == llmbridge::IoBackend::Epoll)
        EXPECT_GT(_gw->stats().upstream_retries, 0u) << "epoll inline pipelined reuse must retry";
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyBackend,
                         ::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                         [](const testing::TestParamInfo<llmbridge::IoBackend>& i) { return be_name(i.param); });

// ── Bug-2 regression (deterministic, no sanitizer needed): every Connection the
// gateway allocates must be freed. Tear down with 50 requests in flight (acquired
// upstreams reachable only via peer — exactly what leaked) and assert the live
// Connection count returns to baseline after ~Gateway.
TEST(GatewayLeak, FreesAllConnectionsOnDestroy)
{
    for (const llmbridge::IoBackend backend : {llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring})
    {
        const long base = llmbridge::Connection::s_live.load();
        {
            TestBackend be;
            be.set_trickle(4); // slow replies so requests are still in flight at teardown
            be.start();
            Gateway gw(0, "127.0.0.1", be.port(), 0, TranslateMode::None, backend);
            const uint16_t port = gw.bound_port();
            std::thread t([&] { gw.run(); });

            std::vector<std::unique_ptr<Client>> cs;
            for (int i = 0; i < 50; ++i)
            {
                auto c = std::make_unique<Client>();
                ASSERT_TRUE(c->connect(port)) << i;
                ASSERT_TRUE(c->send(make_request("x")));
                cs.push_back(std::move(c)); // never read -> in flight at teardown
            }
            gw.request_stop();
            t.join();
            be.stop();
        } // ~Gateway runs here
        EXPECT_EQ(llmbridge::Connection::s_live.load(), base) << be_name(backend);
    }
}

// Multi-worker (SO_REUSEPORT) shared-nothing model: two independent Gateway loops
// on two threads bind the same port; the kernel shards connections across them.
// Each worker owns its connections/pools exclusively (no shared per-request state),
// so the only cross-thread state is the atomic stop flag + the s_live counter. Run
// under ThreadSanitizer (epoll backend) this proves there are no data races; here
// it also checks both workers together serve every request and free every conn.
TEST(GatewayMultiWorker, ShardedConcurrentClientsNoRaceNoLeak)
{
    const long base = llmbridge::Connection::s_live.load();
    {
        TestBackend be;
        be.start();
        const uint16_t port = free_port();
        std::vector<std::unique_ptr<Gateway>> gws;
        for (int i = 0; i < 2; ++i)
            gws.push_back(std::make_unique<Gateway>(port, "127.0.0.1", be.port(), 0,
                                                    TranslateMode::None, llmbridge::IoBackend::Epoll));
        std::vector<std::thread> ths;
        for (auto& g : gws) ths.emplace_back([gp = g.get()] { gp->run(); });

        const int M = 48;
        std::vector<std::unique_ptr<Client>> cs;
        for (int i = 0; i < M; ++i)
        {
            auto c = std::make_unique<Client>();
            ASSERT_TRUE(c->connect(port)) << i;
            ASSERT_TRUE(c->send(make_request("req" + std::to_string(i))));
            cs.push_back(std::move(c));
        }
        for (int i = 0; i < M; ++i)
            EXPECT_NE(cs[i]->recv_response().find("200 OK"), std::string::npos) << "client " << i;
        for (auto& c : cs) c->close();

        for (auto& g : gws) g->request_stop();
        for (auto& t : ths) t.join();
        be.stop();

        uint64_t total = 0;
        for (auto& g : gws) total += g->stats().requests; // safe: read after join
        EXPECT_EQ(total, static_cast<uint64_t>(M)); // the two workers together served all
    }
    EXPECT_EQ(llmbridge::Connection::s_live.load(), base); // no leak across workers
}

// Translation parity across BOTH backends AND all three dialects (2 x 3 = 6).
class ProxyXlate
    : public ProxyIT,
      public ::testing::WithParamInterface<std::tuple<llmbridge::IoBackend, TranslateMode>>
{
};
TEST_P(ProxyXlate, RoundTripAndForward)
{
    const auto [backend, mode] = GetParam();
    _backend.set_response(http_ok(provider_resp_body(mode, "pong")));
    start(0, true, mode, backend);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("marker-content-123")));
    const std::string body = body_of(c.recv_response());
    EXPECT_NE(body.find("\"content\":\"pong\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"object\":\"chat.completion\""), std::string::npos);
    EXPECT_NE(_backend.last_request().find("marker-content-123"), std::string::npos);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
}
INSTANTIATE_TEST_SUITE_P(
    Matrix, ProxyXlate,
    ::testing::Combine(::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                       ::testing::Values(TranslateMode::Anthropic, TranslateMode::Gemini,
                                         TranslateMode::Cohere)),
    [](const testing::TestParamInfo<std::tuple<llmbridge::IoBackend, TranslateMode>>& i) {
        std::string n = be_name(std::get<0>(i.param));
        const TranslateMode m = std::get<1>(i.param);
        n += m == TranslateMode::Anthropic ? "_anthropic" : m == TranslateMode::Gemini ? "_gemini" : "_cohere";
        return n;
    });

// ── Gateway unit checks (just construction, no run loop) ───────────
TEST(GatewayUnit, BoundPortIsNonZeroAfterEphemeralBind)
{
    Gateway io(0, "127.0.0.1", 9, /*warmup*/ 0);
    EXPECT_GT(io.bound_port(), 0);
}

TEST(GatewayUnit, StatsStartZeroed)
{
    Gateway io(0, "127.0.0.1", 9, /*warmup*/ 0);
    EXPECT_EQ(io.stats().requests, 0u);
    EXPECT_EQ(io.stats().errors, 0u);
    EXPECT_EQ(io.stats().upstream_conns_opened, 0u);
}
