// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// End-to-end TLS gateway tests: a real in-process TLS "provider" (blocking
// thread; SSL_set_fd is fine in test code; the memory-BIO constraint only
// applies inside the gateway's event loop) fronted by the Gateway with a TLS
// upstream, driven by a plaintext loopback client. Hermetic: self-signed cert
// generated in-process, nothing leaves 127.0.0.1.
//
// What this proves that net_tls_test cannot: the PUMP WIRING inside both event
// loops: handshake interleaved with connect/recv/send completions, the
// plaintext-invariant on rbuf/wbuf, session survival across the keep-alive
// pool, and certificate rejection surfacing as a client-visible 502.

#include "gateway/gateway.hpp"

#ifdef LLMBRIDGE_HAVE_TLS

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/http.hpp"

using llmbridge::Gateway;
using llmbridge::TlsConfig;
using llmbridge::TranslateMode;

namespace
{
    constexpr const char* kHost = "provider.test";

    struct SelfSigned
    {
        EVP_PKEY* key{nullptr};
        X509* crt{nullptr};
        SelfSigned()
        {
            key = EVP_RSA_gen(2048);
            crt = X509_new();
            ASN1_INTEGER_set(X509_get_serialNumber(crt), 1);
            X509_gmtime_adj(X509_getm_notBefore(crt), 0);
            X509_gmtime_adj(X509_getm_notAfter(crt), 3600);
            X509_set_pubkey(crt, key);
            X509_NAME* nm = X509_get_subject_name(crt);
            X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>(kHost), -1, -1, 0);
            X509_set_issuer_name(crt, nm);
            const std::string san = std::string("DNS:") + kHost;
            X509_EXTENSION* ext =
                X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str());
            X509_add_ext(crt, ext, -1);
            X509_EXTENSION_free(ext);
            X509_sign(crt, key, EVP_sha256());
        }
        ~SelfSigned()
        {
            if (crt) X509_free(crt);
            if (key) EVP_PKEY_free(key);
        }
        // The CA path must be unique per process AND per call. ctest runs this
        // binary many times in parallel (-j), and a fixed name meant one process
        // truncated the file another was mid-way through loading, surfacing as a
        // spurious "no certificate or crl found" in whichever test lost the race.
        // A flaky security suite is a suite people learn to ignore, so this is
        // worth the two lines.
        std::string write_pem() const
        {
            static std::atomic<unsigned> seq{0};
            std::string path = std::string(::testing::TempDir()) + "llmbridge_gw_tls_ca_" +
                               std::to_string(static_cast<long>(::getpid())) + "_" +
                               std::to_string(seq.fetch_add(1, std::memory_order_relaxed)) + ".pem";
            FILE* f = std::fopen(path.c_str(), "wb");
            EXPECT_NE(f, nullptr);
            PEM_write_X509(f, crt);
            std::fclose(f);
            return path;
        }
    };

    // A blocking TLS HTTP/1.1 provider. Each accepted connection gets a thread:
    // TLS-accept, then serve requests until the peer goes away. Counts handshakes
    // so tests can assert session/pool reuse (1 handshake, N requests).
    class TlsBackend
    {
      public:
        // mode: "json" = keep-alive 200 with a JSON body; "sse" = one chunked
        // Anthropic event-stream response, then keep-alive for the next request.
        void start(const SelfSigned& id, std::string mode = "json")
        {
            _mode = std::move(mode);
            _ctx = SSL_CTX_new(TLS_server_method());
            SSL_CTX_use_certificate(_ctx, id.crt);
            SSL_CTX_use_PrivateKey(_ctx, id.key);
            _lfd = ::socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            ::setsockopt(_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = 0;
            ::bind(_lfd, reinterpret_cast<sockaddr*>(&a), sizeof a);
            socklen_t len = sizeof a;
            ::getsockname(_lfd, reinterpret_cast<sockaddr*>(&a), &len);
            _port = ntohs(a.sin_port);
            ::listen(_lfd, 64);
            _acc = std::thread([this] { accept_loop(); });
        }

        void stop()
        {
            if (_lfd < 0) return;
            _stopping = true;
            ::shutdown(_lfd, SHUT_RDWR);
            ::close(_lfd);
            _lfd = -1;
            if (_acc.joinable()) _acc.join();
            // Unblock serve threads parked in SSL_read: the gateway POOLS its
            // keep-alive TLS upstream (by design), so a serve thread legitimately
            // waits for a next request that will never come once the test ends.
            // shutdown() (not close: the thread still owns the fd) makes SSL_read
            // return 0 so the join below completes.
            {
                std::lock_guard<std::mutex> g(_fds_mtx);
                for (const int fd : _fds) ::shutdown(fd, SHUT_RDWR);
            }
            for (auto& t : _conns)
                if (t.joinable()) t.join();
            if (_ctx) SSL_CTX_free(_ctx);
            _ctx = nullptr;
        }
        ~TlsBackend() { stop(); }

        uint16_t port() const { return _port; }
        int handshakes() const { return _handshakes.load(); }
        int requests() const { return _requests.load(); }

      private:
        void accept_loop()
        {
            for (;;)
            {
                const int fd = ::accept(_lfd, nullptr, nullptr);
                if (fd < 0) return; // listener closed
                {
                    std::lock_guard<std::mutex> g(_fds_mtx);
                    _fds.push_back(fd);
                }
                _conns.emplace_back([this, fd] { serve(fd); });
            }
        }

        void serve(int fd)
        {
            if (_mode == "reset")
            {
                // Provider drops the TCP connection before any TLS bytes.
                ::close(fd);
                return;
            }
            SSL* ssl = SSL_new(_ctx);
            SSL_set_fd(ssl, fd);
            if (SSL_accept(ssl) == 1)
            {
                ++_handshakes;
                std::string buf;
                char tmp[8192];
                for (;;)
                {
                    // Frame one request off the decrypted byte stream.
                    llmbridge::net::http::Message m;
                    while (llmbridge::net::http::parse_request(buf, m) != llmbridge::net::http::FrameStatus::Complete)
                    {
                        const int n = SSL_read(ssl, tmp, sizeof tmp);
                        if (n <= 0) goto done;
                        buf.append(tmp, static_cast<size_t>(n));
                    }
                    buf.erase(0, m.total_len);
                    ++_requests;
                    if (_mode == "sse-corrupt")
                    {
                        // Start a legitimate stream (headers + first token through
                        // TLS), then write RAW GARBAGE to the socket: a corrupt
                        // record. The gateway must abort the client stream, not
                        // finish it cleanly.
                        const std::string head = sse_corrupt_prefix();
                        if (SSL_write(ssl, head.data(), static_cast<int>(head.size())) <= 0)
                            goto done;
                        // Let the gateway PROCESS the prefix (stream begins, first
                        // token reaches the client) before the corruption lands
                        // otherwise both arrive in one drain and the gateway
                        // correctly 502s a stream that never started, which is a
                        // different (already-tested) path.
                        std::this_thread::sleep_for(std::chrono::milliseconds(120));
                        const char junk[64] = "GARBAGE-NOT-A-TLS-RECORD-GARBAGE-NOT-A-TLS-RECORD";
                        (void)!::write(fd, junk, sizeof junk); // bypass TLS on purpose
                        // Keep the conn open: the abort must come from the record
                        // failure, not from an EOF racing it.
                        char sink[256];
                        (void)SSL_read(ssl, sink, sizeof sink);
                        goto done;
                    }
                    const std::string resp = _mode == "sse" ? sse_response() : json_response();
                    size_t off = 0;
                    while (off < resp.size())
                    {
                        const int n =
                            SSL_write(ssl, resp.data() + off, static_cast<int>(resp.size() - off));
                        if (n <= 0) goto done;
                        off += static_cast<size_t>(n);
                    }
                    if (_mode == "close1") goto done; // provider closes after one response
                }
            }
        done:
            SSL_free(ssl); // frees nothing we still need; fd is ours to close
            ::close(fd);
        }

        static std::string json_response()
        {
            const std::string body = R"({"ok":true,"via":"tls"})";
            return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body;
        }

        static std::string chunk(const std::string& s)
        {
            char len[16];
            std::snprintf(len, sizeof len, "%zx", s.size());
            return std::string(len) + "\r\n" + s + "\r\n";
        }

        // Headers + message_start + first text delta only; the stream is mid-
        // flight when the corruption lands.
        static std::string sse_corrupt_prefix()
        {
            const std::string ev =
                "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\","
                "\"model\":\"claude\",\"usage\":{\"input_tokens\":1}}}\n\n"
                "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
                "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
                "\"delta\":{\"type\":\"text_delta\",\"text\":\"hola\"}}\n\n";
            return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                   "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n" +
                   chunk(ev);
        }

        static std::string sse_response()
        {
            // Minimal-but-real Anthropic event stream, chunked, keep-alive.
            const std::string ev =
                "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\","
                "\"model\":\"claude\",\"usage\":{\"input_tokens\":1}}}\n\n"
                "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
                "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
                "\"delta\":{\"type\":\"text_delta\",\"text\":\"hola\"}}\n\n"
                "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
                "\"end_turn\"},\"usage\":{\"output_tokens\":2}}\n\n"
                "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
            return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                   "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n" +
                   chunk(ev) + "0\r\n\r\n";
        }

        SSL_CTX* _ctx{nullptr};
        int _lfd{-1};
        uint16_t _port{0};
        std::string _mode{"json"};
        std::atomic<bool> _stopping{false};
        std::atomic<int> _handshakes{0};
        std::atomic<int> _requests{0};
        std::thread _acc;
        std::vector<std::thread> _conns;
        std::mutex _fds_mtx;
        std::vector<int> _fds; // accepted fds, for shutdown-on-stop
    };

    // Plaintext loopback client (the gateway's client side is not TLS).
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
            return ::connect(_fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
        }
        bool send(const std::string& s)
        {
            size_t off = 0;
            while (off < s.size())
            {
                const ssize_t n = ::write(_fd, s.data() + off, s.size() - off);
                if (n <= 0) return false;
                off += static_cast<size_t>(n);
            }
            return true;
        }
        std::string recv_response(int timeout_ms = 5000)
        {
            char tmp[8192];
            for (;;)
            {
                llmbridge::net::http::Message m;
                if (llmbridge::net::http::parse_request(_buf, m) == llmbridge::net::http::FrameStatus::Complete)
                {
                    std::string out = _buf.substr(0, m.total_len);
                    _buf.erase(0, m.total_len);
                    return out;
                }
                pollfd p{_fd, POLLIN, 0};
                if (::poll(&p, 1, timeout_ms) <= 0) return "";
                const ssize_t n = ::read(_fd, tmp, sizeof tmp);
                if (n <= 0) return "";
                _buf.append(tmp, static_cast<size_t>(n));
            }
        }
        std::string recv_all(int timeout_ms = 5000)
        {
            std::string out = std::move(_buf);
            _buf.clear();
            char tmp[8192];
            for (;;)
            {
                pollfd p{_fd, POLLIN, 0};
                if (::poll(&p, 1, timeout_ms) <= 0) return out;
                const ssize_t n = ::read(_fd, tmp, sizeof tmp);
                if (n <= 0) return out;
                out.append(tmp, static_cast<size_t>(n));
            }
        }
        static int status_of(const std::string& r)
        {
            if (r.size() < 12 || r.compare(0, 5, "HTTP/") != 0) return 0;
            return (r[9] - '0') * 100 + (r[10] - '0') * 10 + (r[11] - '0');
        }
        void close()
        {
            if (_fd >= 0)
            {
                ::close(_fd);
                _fd = -1;
            }
        }
        ~Client() { close(); }

      private:
        int _fd = -1;
        std::string _buf;
    };

    std::string make_request(const std::string& body = R"({"model":"gpt","messages":[]})")
    {
        return "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    }
} // namespace

// Parameterized over the two event-loop backends: the pump wiring is entirely
// different code on each, so every test runs on both.
class GatewayTls : public ::testing::TestWithParam<llmbridge::IoBackend>
{
  protected:
    void start(TranslateMode mode = TranslateMode::None, const std::string& backend_mode = "json",
               const std::string& sni = kHost)
    {
        _backend.start(_id, backend_mode);
        TlsConfig tls;
        tls.enabled = true;
        tls.sni_host = sni;
        tls.ca_file = _id.write_pem();
        _gw = std::make_unique<Gateway>(0, "127.0.0.1", _backend.port(), 0, mode, GetParam(),
                                        Gateway::kDefaultUpstreamIdleNs, tls);
        _port = _gw->bound_port();
        _gt = std::thread([this] { _gw->run(); });
    }
    void TearDown() override
    {
        if (_gw) _gw->request_stop();
        if (_gt.joinable()) _gt.join();
        _backend.stop();
    }

    SelfSigned _id;
    TlsBackend _backend;
    std::unique_ptr<Gateway> _gw;
    std::thread _gt;
    uint16_t _port{0};
};

TEST_P(GatewayTls, RoundTripThroughTlsUpstream)
{
    start();
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    ASSERT_FALSE(r.empty()) << "no response through TLS upstream";
    EXPECT_EQ(Client::status_of(r), 200);
    EXPECT_NE(r.find(R"("via":"tls")"), std::string::npos);
    EXPECT_EQ(_backend.handshakes(), 1);
    EXPECT_EQ(_backend.requests(), 1);
}

TEST_P(GatewayTls, PooledConnectionSkipsSecondHandshake)
{
    start();
    // Two sequential clients -> the second request must ride the pooled TLS
    // connection: same session, ONE handshake, two requests. This is the test
    // that fails if release/acquire drops or resets the Session.
    for (int i = 0; i < 2; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_port));
        ASSERT_TRUE(c.send(make_request()));
        const std::string r = c.recv_response();
        ASSERT_FALSE(r.empty()) << "request " << i;
        EXPECT_EQ(Client::status_of(r), 200) << "request " << i;
    }
    EXPECT_EQ(_backend.requests(), 2);
    EXPECT_EQ(_backend.handshakes(), 1) << "pooled reuse paid a second TLS handshake";
    EXPECT_GE(_gw->stats().upstream_reused, 1u);
}

TEST_P(GatewayTls, SseStreamsThroughTlsWithTranslation)
{
    start(TranslateMode::Anthropic, "sse");
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request(R"({"model":"m","stream":true,"messages":[]})")));
    const std::string all = c.recv_all();
    // The Anthropic event stream must come back as translated OpenAI chunks:
    // the token, a finish_reason, and the [DONE] sentinel.
    EXPECT_NE(all.find("hola"), std::string::npos) << all.substr(0, 400);
    EXPECT_NE(all.find("[DONE]"), std::string::npos);
    EXPECT_NE(all.find("chat.completion.chunk"), std::string::npos);
}

TEST_P(GatewayTls, HostnameMismatchYields502NotPlaintextFallback)
{
    // Gateway verifies the peer as "wrong.test"; the cert says provider.test.
    // The client must see a structured 502, and the provider must see ZERO
    // completed handshakes and ZERO requests (nothing was sent to an unverified
    // peer, which is the security property the whole TLS layer exists for).
    start(TranslateMode::None, "json", "wrong.test");
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    ASSERT_FALSE(r.empty()) << "expected a 502, got a hung/closed connection";
    EXPECT_EQ(Client::status_of(r), 502);
    EXPECT_EQ(_backend.handshakes(), 0);
    EXPECT_EQ(_backend.requests(), 0);
}

TEST_P(GatewayTls, ConcurrentTlsStreamsAllComplete)
{
    // 16 simultaneous SSE streams through one gateway loop: 16 independent TLS
    // sessions' handshakes, reads and writes interleave on a single thread. This
    // is the test that catches cross-session state bleed (a Session mistakenly
    // shared or a tls_out written by the wrong conn); any mixing corrupts a
    // record and kills at least one stream.
    start(TranslateMode::Anthropic, "sse");
    constexpr int kStreams = 16;
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < kStreams; ++i)
    {
        clients.push_back(std::make_unique<Client>());
        ASSERT_TRUE(clients.back()->connect(_port)) << i;
        ASSERT_TRUE(clients.back()->send(
            make_request(R"({"model":"m","stream":true,"messages":[]})"))) << i;
    }
    int done = 0;
    for (int i = 0; i < kStreams; ++i)
    {
        const std::string all = clients[i]->recv_all();
        EXPECT_NE(all.find("hola"), std::string::npos) << "stream " << i;
        if (all.find("[DONE]") != std::string::npos) ++done;
    }
    EXPECT_EQ(done, kStreams);
    EXPECT_EQ(_backend.handshakes(), kStreams); // one session per concurrent stream
}

TEST_P(GatewayTls, CorruptRecordMidStreamAbortsWithoutDone)
{
    // The stream STARTS cleanly (headers + first token through TLS), then the
    // provider writes raw garbage on the wire. The client must get the partial
    // stream and a hard close, never a well-formed [DONE]: finalizing a
    // corrupted stream as clean would hide the corruption entirely.
    start(TranslateMode::Anthropic, "sse-corrupt");
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request(R"({"model":"m","stream":true,"messages":[]})")));
    const std::string all = c.recv_all();
    EXPECT_NE(all.find("hola"), std::string::npos) << "stream never started: " << all.substr(0, 200);
    EXPECT_EQ(all.find("[DONE]"), std::string::npos)
        << "corrupt TLS stream was finalized as clean";
}

TEST_P(GatewayTls, ProviderClosingPooledConnDoesNotBreakNextRequest)
{
    // Provider closes its side after every response (keep-alive header, then
    // close, rude but real). Whichever way the gateway learns (pool eviction on
    // EOF, or stale-conn retry at reuse time), the NEXT request must still get a
    // 200 on a fresh session. Guards the retry/eviction paths' TLS attach.
    start(TranslateMode::None, "close1");
    for (int i = 0; i < 3; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_port)) << i;
        ASSERT_TRUE(c.send(make_request())) << i;
        const std::string r = c.recv_response();
        ASSERT_FALSE(r.empty()) << "request " << i;
        EXPECT_EQ(Client::status_of(r), 200) << "request " << i;
    }
    EXPECT_EQ(_backend.requests(), 3);
    EXPECT_EQ(_backend.handshakes(), 3); // every conn died; every request re-handshakes
}

TEST_P(GatewayTls, UpstreamClosingMidHandshakeYields502)
{
    // TCP accept then immediate close: the gateway's ClientHello meets an EOF.
    // Must surface as a structured 502, not a hang or a plaintext retry.
    start(TranslateMode::None, "reset");
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    ASSERT_FALSE(r.empty()) << "expected 502, got hang/close";
    EXPECT_EQ(Client::status_of(r), 502);
}

INSTANTIATE_TEST_SUITE_P(Backends, GatewayTls,
                         ::testing::Values(llmbridge::IoBackend::Epoll
#ifdef LLMBRIDGE_HAVE_URING
                                           ,
                                           llmbridge::IoBackend::Uring
#endif
                                           ),
                         [](const auto& info) {
                             return info.param == llmbridge::IoBackend::Epoll ? "epoll" : "uring";
                         });

#endif // LLMBRIDGE_HAVE_TLS
