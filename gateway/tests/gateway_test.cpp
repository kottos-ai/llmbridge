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

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/http.hpp"

using llmbridge::Gateway;

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
            if (_fd >= 0) { ::shutdown(_fd, SHUT_RDWR); ::close(_fd); _fd = -1; }
            if (_acc.joinable()) _acc.join();
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
            char tmp[8192];
            const std::string resp = canned_response();
            while (!_stop)
            {
                llmbridge::http::Message m;
                while (llmbridge::http::parse(buf, m) != llmbridge::http::ParseStatus::Complete)
                {
                    ssize_t n = ::read(c, tmp, sizeof(tmp));
                    if (n <= 0) { ::close(c); return; }
                    buf.append(tmp, static_cast<size_t>(n));
                }
                buf.erase(0, m.total_len);
                if (::write(c, resp.data(), resp.size()) < 0) { ::close(c); return; }
                if (!m.keep_alive) { ::close(c); return; }
            }
            ::close(c);
        }

        int _fd = -1;
        uint16_t _port = 0;
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
        void start(int64_t warmup_ns = 0, bool with_backend = true)
        {
            uint16_t up_port;
            if (with_backend) { _backend.start(); up_port = _backend.port(); }
            else up_port = free_port(); // nothing listening -> connection refused

            _gw = std::make_unique<Gateway>(0, "127.0.0.1", up_port, warmup_ns);
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
