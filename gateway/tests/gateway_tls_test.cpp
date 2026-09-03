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
// What this proves that net_tls_test cannot: the pump wiring inside both event
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
#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <fstream>
#include <fcntl.h>
#include <string>
#include <thread>
#include <vector>

#include "net/http.hpp"

using llmbridge::Gateway;
using llmbridge::TlsConfig;
using llmbridge::UpstreamDialect;
using llmbridge::Upstream;
using llmbridge::Policy;
using llmbridge::Decision;
using llmbridge::RequestFacts;
using llmbridge::FailureFacts;
using llmbridge::Retry;

namespace
{
    constexpr const char* kHost = "provider.test";

    struct SelfSigned
    {
        EVP_PKEY* key{nullptr};
        X509* crt{nullptr};
        explicit SelfSigned(const char* cn = kHost)
        {
            key = EVP_RSA_gen(2048);
            crt = X509_new();
            ASN1_INTEGER_set(X509_get_serialNumber(crt), 1);
            X509_gmtime_adj(X509_getm_notBefore(crt), 0);
            X509_gmtime_adj(X509_getm_notAfter(crt), 3600);
            X509_set_pubkey(crt, key);
            X509_NAME* nm = X509_get_subject_name(crt);
            X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
            X509_set_issuer_name(crt, nm);
            const std::string san = std::string("DNS:") + cn;
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
        // The CA path must be unique per process and per call. ctest runs this
        // binary many times in parallel (-j), and a fixed name meant one process
        // truncated the file another was mid-way through loading, surfacing as a
        // spurious "no certificate or crl found" in whichever test lost the race.
        // A flaky security suite is a suite people learn to ignore, so this is
        // worth the two lines.
        /// Write the private key, mode 600 so the gateway's startup guard accepts
        /// it. Needed because inbound TLS makes US the server, so the same
        /// self-signed identity has to be presented, and not merely trusted.
        std::string write_key_pem() const
        {
            static std::atomic<unsigned> seq{0};
            std::string path = std::string(::testing::TempDir()) + "llmbridge_gw_key_" +
                               std::to_string(static_cast<long>(::getpid())) + "_" +
                               std::to_string(seq.fetch_add(1, std::memory_order_relaxed)) + ".pem";
            FILE* f = std::fopen(path.c_str(), "wb");
            EXPECT_NE(f, nullptr);
            PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
            std::fclose(f);
            EXPECT_EQ(::chmod(path.c_str(), 0600), 0);
            return path;
        }

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
        // handshake_delay_ms stalls the server before SSL_accept, so the client
        // spends a known, large interval inside the TLS handshake and nowhere
        // else. That is what makes the t2 attribution testable.
        void start(const SelfSigned& id, std::string mode = "json", int handshake_delay_ms = 0)
        {
            _mode = std::move(mode);
            _hs_delay_ms = handshake_delay_ms;
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
            // Unblock serve threads parked in SSL_read: the gateway pools its
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
            if (_hs_delay_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(_hs_delay_ms));
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
                    if (_mode == "sse-badchunk-hold")
                    {
                        // Valid TLS throughout, but invalid chunked framing inside
                        // it. That reaches stream_step()'s Corrupt path, which calls
                        // stream_truncate(), and not the TLS record failure that
                        // "sse-corrupt" exercises. Then hold the connection: the
                        // point is whether the gateway tears the client down itself,
                        // or waits for a provider that never closes.
                        const std::string head = sse_corrupt_prefix();
                        if (SSL_write(ssl, head.data(), static_cast<int>(head.size())) <= 0)
                            goto done;
                        std::this_thread::sleep_for(std::chrono::milliseconds(120));
                        const std::string bad = "ZZZZNOTAHEXSIZE\r\n";
                        (void)SSL_write(ssl, bad.data(), static_cast<int>(bad.size()));
                        // Block forever. If the gateway is waiting for US to close,
                        // it waits until the 120 s idle sweep.
                        char sink[256];
                        (void)SSL_read(ssl, sink, sizeof sink);
                        goto done;
                    }
                    if (_mode == "sse-corrupt")
                    {
                        // Start a legitimate stream (headers + first token through
                        // TLS), then write raw garbage to the socket: a corrupt
                        // record. The gateway must abort the client stream, not
                        // finish it cleanly.
                        const std::string head = sse_corrupt_prefix();
                        if (SSL_write(ssl, head.data(), static_cast<int>(head.size())) <= 0)
                            goto done;
                        // Let the gateway process the prefix (stream begins, first
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
                    const std::string resp = _mode == "sse"          ? sse_response()
                                             : _mode == "sse-flood"      ? sse_flood()
                                             : _mode == "big"            ? big_response()
                                             : _mode == "anthropic-json" ? anthropic_json_response()
                                                                         : json_response();
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

        // A well-formed Anthropic message. The gateway only emits timing headers
        // where it rebuilds the response, so a non-streaming timing test needs a
        // body the translator accepts; the plain {"ok":true} body is byte-
        // forwarded and carries the provider's headers untouched.
        static std::string anthropic_json_response()
        {
            const std::string body =
                R"({"id":"msg_tls","type":"message","role":"assistant","model":"claude-x",)"
                R"("content":[{"type":"text","text":"hola"}],"stop_reason":"end_turn",)"
                R"("usage":{"input_tokens":7,"output_tokens":11}})";
            return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body;
        }

        /// A body far larger than any socket send buffer, so the gateway cannot
        /// hand it all to the kernel in one write. That is the only condition
        /// under which "fed to the transport" and "on the wire" come apart.
      public:
        // Public so PlainBackend serves BYTE-IDENTICAL responses. If the two mocks
        // differed, a plaintext-versus-TLS comparison would be measuring the mocks.
        static std::string sse_response_public() { return sse_response(); }
        static std::string big_response_public() { return big_response(); }
        static std::string json_response_public() { return json_response(); }

      private:
        static std::string big_response()
        {
            const std::string body(2 * 1024 * 1024, 'x');
            return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body;
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

        // ~32 MiB of well-formed deltas, one chunk per event. The first version of
        // this put the whole body in a single chunk, which the decoder correctly
        // refuses as a hostile chunk size (kMaxBodyLen is 16 MiB), so 96 bytes
        // reached the client and the "bounded" result below was measuring nothing.
        // The control test is what caught it.
        static std::string sse_flood()
        {
            const std::string head =
                "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\","
                "\"model\":\"claude\",\"usage\":{\"input_tokens\":1}}}\n\n"
                "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
                "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
            const std::string filler(4000, 'z');
            const std::string delta =
                "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
                "\"delta\":{\"type\":\"text_delta\",\"text\":\"" + filler + "\"}}\n\n";
            std::string out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                              "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n";
            out.reserve(34u * 1024 * 1024);
            out += chunk(head);
            for (int i = 0; i < 8000; ++i) out += chunk(delta); // ~32 MiB in 8000 chunks
            out += chunk(
                "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
                "\"end_turn\"},\"usage\":{\"output_tokens\":2}}\n\n"
                "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n");
            out += "0\r\n\r\n";
            return out;
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
        // Written by stop() on one thread, read by accept_loop on another. Plain
        // int here is a real data race, and TSan reported it.
        std::atomic<int> _lfd{-1};
        uint16_t _port{0};
        std::string _mode{"json"};
        std::atomic<bool> _stopping{false};
        int _hs_delay_ms = 0;
        std::atomic<int> _handshakes{0};
        std::atomic<int> _requests{0};
        std::thread _acc;
        std::vector<std::thread> _conns;
        std::mutex _fds_mtx;
        std::vector<int> _fds; // accepted fds, for shutdown-on-stop
    };

    // Plaintext loopback client (the gateway's client side is not TLS).
    /// A plaintext mock upstream, so the client leg can be TLS while the upstream
    /// leg is not. TlsBackend cannot serve that case: it always speaks TLS.
    class PlainBackend
    {
      public:
        void start(const std::string& mode)
        {
            _mode = mode;
            _fd = ::socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = 0;
            ::bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
            socklen_t len = sizeof a;
            ::getsockname(_fd, reinterpret_cast<sockaddr*>(&a), &len);
            _port = ntohs(a.sin_port);
            ::listen(_fd, 64);
            _run = true;
            _acc = std::thread([this] { loop(); });
        }

        void stop()
        {
            if (!_run) return;
            _run = false;
            ::shutdown(_fd, SHUT_RDWR);
            ::close(_fd);
            _fd = -1;
            if (_acc.joinable()) _acc.join();
        }
        ~PlainBackend() { stop(); }
        uint16_t port() const { return _port; }

      private:
        void loop()
        {
            while (_run)
            {
                const int c = ::accept(_fd, nullptr, nullptr);
                if (c < 0) return;
                _conns.emplace_back([this, c] { serve(c); });
            }
        }
        void serve(int c)
        {
            std::string in;
            char tmp[4096];
            while (_run)
            {
                llmbridge::net::http::Message m;
                while (llmbridge::net::http::parse_request(in, m) !=
                       llmbridge::net::http::FrameStatus::Complete)
                {
                    const ssize_t n = ::read(c, tmp, sizeof tmp);
                    if (n <= 0) { ::close(c); return; }
                    in.append(tmp, static_cast<size_t>(n));
                }
                in.erase(0, m.total_len);
                const std::string resp = _mode == "sse"   ? TlsBackend::sse_response_public()
                                         : _mode == "big" ? TlsBackend::big_response_public()
                                                          : TlsBackend::json_response_public();
                size_t off = 0;
                while (off < resp.size())
                {
                    const ssize_t n = ::write(c, resp.data() + off, resp.size() - off);
                    if (n <= 0) { ::close(c); return; }
                    off += static_cast<size_t>(n);
                }
            }
            ::close(c);
        }
        // See TlsBackend::_lfd: written by stop(), read by the accept loop.
        std::atomic<int> _fd{-1};
        uint16_t _port{0};
        std::string _mode{"json"};
        std::atomic<bool> _run{false};
        std::thread _acc;
        std::vector<std::jthread> _conns;
    };

    /// A real TLS client for the gateway's own listener. The existing `Client`
    /// below is plaintext; inbound TLS needs a peer that actually handshakes,
    /// because the interesting failures are in the handshake and its teardown.
    class TlsClient
    {
      public:
        ~TlsClient() { close(); }

        bool connect(uint16_t port, const std::string& ca_path)
        {
            _fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(port);
            if (::connect(_fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
            // Note: no SO_RCVTIMEO here, deliberately. Adding one seemed like cheap
            // insurance against a hanging test, and it broke three of them: with a
            // receive deadline SSL_read and SSL_connect return -1 with WANT_READ on
            // expiry, and these loops treat any non-positive return as fatal. The
            // SSE test then failed in 82 ms instead of reading a stream. Bounding
            // the hang properly means teaching every loop to retry on EAGAIN while
            // honouring its own deadline, which is a real change and not a one-liner.

            _ctx = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_verify(_ctx, SSL_VERIFY_PEER, nullptr);
            if (SSL_CTX_load_verify_locations(_ctx, ca_path.c_str(), nullptr) != 1) return false;
            _ssl = SSL_new(_ctx);
            SSL_set_fd(_ssl, _fd);
            SSL_set_tlsext_host_name(_ssl, kHost);
            SSL_set1_host(_ssl, kHost);
            return true;
        }

        bool handshake() { return SSL_connect(_ssl) == 1; }

        /// Send the ClientHello one byte at A time. The handshake then spans many
        /// reads on the gateway, which is the fragmentation case a single write
        /// never exercises.
        bool handshake_dribbled()
        {
            SSL_set_connect_state(_ssl);
            BIO* wb = BIO_new(BIO_s_mem());
            BIO* rb = BIO_new(BIO_s_mem());
            BIO_set_mem_eof_return(rb, -1);
            SSL* s2 = SSL_new(_ctx);
            SSL_set_bio(s2, rb, wb);
            SSL_set_connect_state(s2);
            SSL_set1_host(s2, kHost);
            (void)SSL_do_handshake(s2);
            char buf[16384];
            const int n = BIO_read(wb, buf, sizeof buf);
            SSL_free(s2);
            for (int i = 0; i < n; ++i)
                if (::write(_fd, buf + i, 1) != 1) return false;
            return n > 0;
        }

        bool send(const std::string& s)
        {
            size_t off = 0;
            while (off < s.size())
            {
                const int n = SSL_write(_ssl, s.data() + off, static_cast<int>(s.size() - off));
                if (n <= 0) return false;
                off += static_cast<size_t>(n);
            }
            return true;
        }

        /// Read until one complete HTTP response is framed, or the deadline passes.
        /// Raw bytes up to and including `marker`, for an interim `100 Continue`,
        /// which frames on nothing recv_response understands.
        std::string recv_until(std::string_view marker, int timeout_ms = 3000)
        {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms);
            char tmp[8192];
            for (;;)
            {
                const size_t at = _buf.find(marker);
                if (at != std::string::npos)
                {
                    std::string out = _buf.substr(0, at + marker.size());
                    _buf.erase(0, at + marker.size());
                    return out;
                }
                if (std::chrono::steady_clock::now() > deadline) return {};
                const int n = SSL_read(_ssl, tmp, sizeof tmp);
                if (n <= 0)
                {
                    const int e = SSL_get_error(_ssl, n);
                    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
                    return {};
                }
                _buf.append(tmp, static_cast<size_t>(n));
            }
        }
        std::string recv_response(int timeout_ms = 5000)
        {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms);
            char tmp[8192];
            for (;;)
            {
                llmbridge::net::http::ResponseHead h;
                if (llmbridge::net::http::parse_response_head(_buf, h) ==
                        llmbridge::net::http::FrameStatus::Complete &&
                    _buf.size() >= h.header_len + h.content_length)
                {
                    const size_t total = h.header_len + h.content_length;
                    std::string out = _buf.substr(0, total);
                    _buf.erase(0, total);
                    return out;
                }
                if (std::chrono::steady_clock::now() > deadline) return {};
                const int n = SSL_read(_ssl, tmp, sizeof tmp);
                if (n <= 0)
                {
                    const int e = SSL_get_error(_ssl, n);
                    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
                    return {};
                }
                _buf.append(tmp, static_cast<size_t>(n));
            }
        }

        /// Raw SSL_read for close-delimited bodies (SSE), where no Content-Length
        /// exists to frame against.
        int read_raw(char* buf, size_t n) { return SSL_read(_ssl, buf, static_cast<int>(n)); }

        void close()
        {
            if (_ssl) { SSL_free(_ssl); _ssl = nullptr; }
            if (_ctx) { SSL_CTX_free(_ctx); _ctx = nullptr; }
            if (_fd >= 0) { ::close(_fd); _fd = -1; }
        }
        int fd() const { return _fd; }

      private:
        int _fd{-1};
        SSL_CTX* _ctx{nullptr};
        SSL* _ssl{nullptr};
        std::string _buf;
    };

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
        /// A streamed reply read to its end: the terminating zero-length chunk on a
        /// clean stream, or the peer closing on a truncated one. `recv_all` waits for
        /// the close alone, and since llmbridge stopped closing a clean stream that
        /// means waiting out the full timeout every time. Sixteen concurrent streams
        /// turned this suite's slowest test from milliseconds into 80 seconds, which
        /// is what timed the TLS job out in CI.
        std::string recv_stream(int timeout_ms = 5000)
        {
            std::string out = std::move(_buf);
            _buf.clear();
            char tmp[8192];
            for (;;)
            {
                // The zero-length chunk. Safe to search for whole: an SSE payload
                // separates events with "\n\n", never "\r\n\r\n", so this appears
                // only as framing.
                if (out.find("0\r\n\r\n") != std::string::npos) return out;
                pollfd p{_fd, POLLIN, 0};
                if (::poll(&p, 1, timeout_ms) <= 0) return out;
                const ssize_t n = ::read(_fd, tmp, sizeof tmp);
                if (n <= 0) return out; // truncated: the close is the end
                out.append(tmp, static_cast<size_t>(n));
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
    void start(UpstreamDialect mode = UpstreamDialect::OpenAI, const std::string& backend_mode = "json",
               const std::string& sni = kHost, int handshake_delay_ms = 0,
               bool timing_headers = false)
    {
        _backend.start(_id, backend_mode, handshake_delay_ms);
        TlsConfig tls;
        tls.upstream_tls = true;
        tls.sni_host = sni;
        tls.ca_file = _id.write_pem();
        _gw = std::make_unique<Gateway>(0, "127.0.0.1", _backend.port(), 0, mode, GetParam(),
                                        Gateway::kDefaultUpstreamIdleNs, tls, timing_headers);
        _port = _gw->bound_port();
        _gt = std::thread([this] { _gw->run(); });
    }
    /// Inbound TLS: the gateway becomes a TLS server for its own listener. The
    /// upstream leg stays TLS too, so one test covers a request crossing two
    /// independent TLS sessions in opposite roles, which is the real deployment.
    /// `setup_ns` is applied before the loop thread starts. Setting it afterwards
    /// writes state the loop thread reads in sweep_idle, which is a genuine data
    /// race that TSan reports; the seam is not for live reconfiguration.
    void start_inbound(UpstreamDialect mode = UpstreamDialect::OpenAI,
                       const std::string& backend_mode = "json", int64_t setup_ns = 0)
    {
        _backend.start(_id, backend_mode, 0);
        TlsConfig tls;
        tls.upstream_tls = true;
        tls.sni_host = kHost;
        tls.ca_file = _id.write_pem();
        tls.client_tls = true;
        tls.cert_file = _id.write_pem();
        tls.key_file = _id.write_key_pem();
        _ca_path = tls.ca_file;
        _gw = std::make_unique<Gateway>(0, "127.0.0.1", _backend.port(), 0, mode, GetParam(),
                                        Gateway::kDefaultUpstreamIdleNs, tls, false);
        if (setup_ns > 0) _gw->set_client_setup_ns(setup_ns);
        _port = _gw->bound_port();
        _gt = std::thread([this] { _gw->run(); });
    }

    /// The missing row of the matrix: TLS on the client leg, plaintext upstream.
    /// This is `--listen-tls` with `--upstream 127.0.0.1:8000`, which is what
    /// terminating TLS at the edge in front of a local model looks like, and it is
    /// the configuration where a leg mix-up in tls_required() or wbuf_on_wire()
    /// would show, because the two legs disagree.
    void start_client_tls_only(UpstreamDialect mode = UpstreamDialect::OpenAI,
                               const std::string& backend_mode = "json")
    {
        _plain.start(backend_mode);
        TlsConfig tls;
        tls.upstream_tls = false; // plaintext upstream, on purpose
        tls.client_tls = true;
        tls.cert_file = _id.write_pem();
        tls.key_file = _id.write_key_pem();
        _ca_path = _id.write_pem();
        _gw = std::make_unique<Gateway>(0, "127.0.0.1", _plain.port(), 0, mode, GetParam(),
                                        Gateway::kDefaultUpstreamIdleNs, tls, false);
        _port = _gw->bound_port();
        _gt = std::thread([this] { _gw->run(); });
    }

    void TearDown() override
    {
        if (_gw) _gw->request_stop();
        if (_gt.joinable()) _gt.join();
        _backend.stop();
        _plain.stop();
    }

    SelfSigned _id;
    TlsBackend _backend;
    PlainBackend _plain;
    std::unique_ptr<Gateway> _gw;
    std::thread _gt;
    uint16_t _port{0};
    std::string _ca_path;
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

// The TLS handshake must land in connect-us (t2-t1), not in upwrite-us (t3-t2).
//
// The defect this locks down: t2 was stamped when the TCP connect completed,
// which is before start_handshake(). t3 is stamped when the request ciphertext
// is flushed, which cannot happen until the handshake finishes. So on a cold TLS
// connection the whole handshake was reported as "the write() into the kernel's
// socket buffer": a live run showed upwrite-us at 32-43 ms against 34-107 us on
// a warm one, and LATENCY.md claims upwrite-us is ~4.4 us and that connect-us is
// "TCP + TLS".
//
// The backend stalls kDelayMs before SSL_accept, so that interval is spent
// unambiguously inside the handshake and nowhere else.
// The TLS handshake must land in connect-us (t2-t1), not in upwrite-us (t3-t2).
//
// The defect these lock down (fixed in 0.10.1): t2 was stamped when the TCP
// connect completed, which is before start_handshake(), while t3 waits for the
// request ciphertext to flush, which cannot happen until the handshake ends. So
// a cold TLS connection reported the entire handshake as "the write() into the
// kernel's socket buffer". A live run measured upwrite-us at 32-43 ms cold
// against 34 us warm, against a documented ~4.4 us.
//
// Run on both response paths, because they build their headers in different
// code: the non-streaming path rebuilds the response after the body arrives, the
// streaming path emits headers up front at first upstream byte. Both are
// parameterized over both event loops, so four tests in total.
//
// The mock stalls kDelayMs before SSL_accept, so that interval is spent
// unambiguously inside the handshake and nowhere else.
namespace
{
    constexpr int kHandshakeStallMs = 300;

    long long header_us(const std::string& r, const char* name)
    {
        const size_t k = r.find(name);
        if (k == std::string::npos) return -1;
        const size_t v = r.find(": ", k);
        if (v == std::string::npos) return -1;
        return std::strtoll(r.c_str() + v + 2, nullptr, 10);
    }

    void expect_handshake_in_connect(const std::string& r)
    {
        const long long connect_us = header_us(r, "x-llmbridge-connect-us");
        const long long upwrite_us = header_us(r, "x-llmbridge-upwrite-us");
        ASSERT_GE(connect_us, 0) << "no connect-us header:\n" << r.substr(0, 400);
        ASSERT_GE(upwrite_us, 0) << "no upwrite-us header:\n" << r.substr(0, 400);
        EXPECT_GE(connect_us, kHandshakeStallMs * 1000LL * 8 / 10)
            << "connect-us " << connect_us << " us does not contain the "
            << kHandshakeStallMs << " ms handshake; it was attributed elsewhere";
        EXPECT_LT(upwrite_us, kHandshakeStallMs * 1000LL / 2)
            << "upwrite-us " << upwrite_us << " us contains the handshake; t2 is "
            << "being stamped at TCP connect instead of at handshake completion";
    }
} // namespace

TEST_P(GatewayTls, TlsHandshakeIsAttributedToConnectNonStreaming)
{
    start(UpstreamDialect::Anthropic, "anthropic-json", kHost, kHandshakeStallMs,
          /*timing_headers=*/true);
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    ASSERT_FALSE(r.empty());
    ASSERT_EQ(Client::status_of(r), 200) << r.substr(0, 400);
    expect_handshake_in_connect(r);
}

TEST_P(GatewayTls, TlsHandshakeIsAttributedToConnectStreaming)
{
    start(UpstreamDialect::Anthropic, "sse", kHost, kHandshakeStallMs,
          /*timing_headers=*/true);
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request(R"({"model":"m","stream":true,"messages":[]})")));
    const std::string all = c.recv_stream();
    ASSERT_FALSE(all.empty());
    expect_handshake_in_connect(all);
}

TEST_P(GatewayTls, PooledConnectionSkipsSecondHandshake)
{
    start();
    // Two sequential clients -> the second request must ride the pooled TLS
    // connection: same session, one handshake, two requests. This is the test
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
    // stats() belongs to the loop thread; join before reading it. See the note on
    // pooled_upstream_count().
    _gw->request_stop();
    if (_gt.joinable()) _gt.join();
    EXPECT_GE(_gw->stats().upstream_reused, 1u);
}

TEST_P(GatewayTls, SseStreamsThroughTlsWithTranslation)
{
    start(UpstreamDialect::Anthropic, "sse");
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request(R"({"model":"m","stream":true,"messages":[]})")));
    const std::string all = c.recv_stream();
    // The Anthropic event stream must come back as translated OpenAI chunks:
    // the token, a finish_reason, and the [DONE] sentinel.
    EXPECT_NE(all.find("hola"), std::string::npos) << all.substr(0, 400);
    EXPECT_NE(all.find("[DONE]"), std::string::npos);
    EXPECT_NE(all.find("chat.completion.chunk"), std::string::npos);
}

TEST_P(GatewayTls, HostnameMismatchYields502NotPlaintextFallback)
{
    // Gateway verifies the peer as "wrong.test"; the cert says provider.test.
    // The client must see a structured 502, and the provider must see zero
    // completed handshakes and zero requests (nothing was sent to an unverified
    // peer, which is the security property the whole TLS layer exists for).
    start(UpstreamDialect::OpenAI, "json", "wrong.test");
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    ASSERT_FALSE(r.empty()) << "expected a 502, got a hung/closed connection";
    EXPECT_EQ(Client::status_of(r), 502);
    EXPECT_EQ(_backend.handshakes(), 0);
    EXPECT_EQ(_backend.requests(), 0);
    // The counter is the inbound leg only. An upstream handshake failure is a
    // provider or trust-store problem, it keeps its WARN, and it must not move this
    // number: otherwise scanner noise and a broken provider are indistinguishable in
    // the one metric that exists to separate them.
    EXPECT_EQ(_gw->stats().client_tls_handshake_failures, 0u);
}

TEST_P(GatewayTls, ConcurrentTlsStreamsAllComplete)
{
    // 16 simultaneous SSE streams through one gateway loop: 16 independent TLS
    // sessions' handshakes, reads and writes interleave on a single thread. This
    // is the test that catches cross-session state bleed (a Session mistakenly
    // shared or a tls_out written by the wrong conn); any mixing corrupts a
    // record and kills at least one stream.
    start(UpstreamDialect::Anthropic, "sse");
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
        const std::string all = clients[i]->recv_stream();
        EXPECT_NE(all.find("hola"), std::string::npos) << "stream " << i;
        if (all.find("[DONE]") != std::string::npos) ++done;
    }
    EXPECT_EQ(done, kStreams);
    EXPECT_EQ(_backend.handshakes(), kStreams); // one session per concurrent stream
}

TEST_P(GatewayTls, CorruptRecordMidStreamAbortsWithoutDone)
{
    // The stream starts cleanly (headers + first token through TLS), then the
    // provider writes raw garbage on the wire. The client must get the partial
    // stream and a hard close, never a well-formed [DONE]: finalizing a
    // corrupted stream as clean would hide the corruption entirely.
    start(UpstreamDialect::Anthropic, "sse-corrupt");
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
    // EOF, or stale-conn retry at reuse time), the next request must still get a
    // 200 on a fresh session. Guards the retry/eviction paths' TLS attach.
    start(UpstreamDialect::OpenAI, "close1");
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
    start(UpstreamDialect::OpenAI, "reset");
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    ASSERT_FALSE(r.empty()) << "expected 502, got hang/close";
    EXPECT_EQ(Client::status_of(r), 502);
}

// ---------------------------------------------------------------------------
// Cross-venue upstream TLS: each venue must be verified against its own hostname.
//
// tls_attach_upstream once passed the global TlsConfig::sni_host for every venue,
// so init_client drove both SNI and SSL_set1_host from the first venue's name.
// The consequence is a valid-cert-wrong-server hole across the table: a request
// translated and credentialed for venue 1's dialect is sent on a socket whose
// certificate was only ever checked against venue 0's name. These tests fail
// against that code and pass against the per-venue fix.
// ---------------------------------------------------------------------------

namespace
{
    constexpr const char* kHostTwo = "venue-two.test";

    /// Concatenate two self-signed leaves into one trust bundle, so the gateway's
    /// single upstream context trusts both venues. Without this the second venue
    /// would fail for lack of a trust anchor, masking the hostname question.
    std::string write_ca_bundle(const SelfSigned& a, const SelfSigned& b)
    {
        const std::string pa = a.write_pem(), pb = b.write_pem();
        std::string path = std::string(::testing::TempDir()) + "llmbridge_gw_bundle_" +
                           std::to_string(static_cast<long>(::getpid())) + "_" +
                           std::to_string(reinterpret_cast<uintptr_t>(&a)) + ".pem";
        std::ofstream out(path, std::ios::binary);
        std::ifstream fa(pa, std::ios::binary), fb(pb, std::ios::binary);
        out << fa.rdbuf() << "\n" << fb.rdbuf();
        return path;
    }

    /// A port with nothing listening: bind, read the number back, close. Used to make
    /// a venue fail its connect deterministically.
    uint16_t closed_port()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        socklen_t len = sizeof(a);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        ::close(fd);
        return ntohs(a.sin_port);
    }

    class PinnedPolicy final : public Policy
    {
      public:
        explicit PinnedPolicy(int idx) : _idx(idx) {}
        Decision decide(const RequestFacts&) noexcept override
        {
            return {.allow = true, .upstream_index = _idx};
        }
      private:
        int _idx;
    };

    /// Starts on `first`, moves to `next` once. Failover matters most on the TLS leg,
    /// where a retry must build a new session against the new venue's hostname; reusing
    /// the failed venue's SNI would either fail verification or, far worse, accept it.
    class TlsFailoverPolicy final : public Policy
    {
      public:
        TlsFailoverPolicy(int first, int next) : _first(first), _next(next) {}
        Decision decide(const RequestFacts&) noexcept override
        {
            return {.allow = true, .upstream_index = _first, .tag = kTag};
        }
        Retry on_failure(const FailureFacts& f) noexcept override
        {
            ++failures;
            seen_tag = f.tag;
            return f.attempt == 0 ? Retry{true, _next} : Retry{};
        }
        static constexpr uint64_t kTag = 0xF00DFACEu;
        int failures = 0;
        uint64_t seen_tag = 0;
      private:
        int _first, _next;
    };
} // namespace

class GatewayCrossVenueTls : public ::testing::TestWithParam<llmbridge::IoBackend>
{
  protected:
    // venue_one_cert lets a test hand venue 1 the wrong identity (venue 0's), to
    // prove per-venue verification actually discriminates instead of passing
    // everything. Default gives each venue its matching cert.
    void start(int route_to, bool venue_one_serves_its_own_cert = true)
    {
        _b0.start(_id0, "json");
        _b1.start(venue_one_serves_its_own_cert ? _id1 : _id0, "json");
        TlsConfig tls;
        tls.ca_file = write_ca_bundle(_id0, _id1); // trust both leaves
        std::vector<Upstream> table = {
            Upstream{.ip = "127.0.0.1", .port = _b0.port(), .tls = true, .sni_host = kHost,
                 .dialect = UpstreamDialect::OpenAI},
            Upstream{.ip = "127.0.0.1", .port = _b1.port(), .tls = true, .sni_host = kHostTwo,
                 .dialect = UpstreamDialect::OpenAI},
        };
        _policy = std::make_unique<PinnedPolicy>(route_to);
        _gw = std::make_unique<Gateway>(uint16_t{0}, std::move(table), int64_t{0}, GetParam(),
                                        Gateway::kDefaultUpstreamIdleNs, tls, false,
                                        _policy.get(), std::vector<std::string>{});
        _port = _gw->bound_port();
        _gt = std::thread([this] { _gw->run(); });
    }

    void TearDown() override
    {
        if (_gw) _gw->request_stop();
        if (_gt.joinable()) _gt.join();
        _b0.stop();
        _b1.stop();
    }

    SelfSigned _id0{kHost};
    SelfSigned _id1{kHostTwo};
    TlsBackend _b0, _b1;
    std::unique_ptr<PinnedPolicy> _policy;
    std::unique_ptr<Gateway> _gw;
    std::thread _gt;
    uint16_t _port{0};
};

// Route to venue 1, which presents its own valid cert for venue-two.test. With the
// bug the gateway verifies against venue 0's name (provider.test) and the handshake
// fails, surfacing as a 502; the fix verifies against venue-two.test and it is 200.
TEST_P(GatewayCrossVenueTls, SecondVenueIsVerifiedAgainstItsOwnHostname)
{
    start(/*route_to=*/1);
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    ASSERT_FALSE(r.empty()) << "no response routing to venue 1";
    EXPECT_EQ(Client::status_of(r), 200)
        << "venue 1 rejected despite a valid cert for its own name: the gateway "
        << "verified it against another venue's hostname. " << r.substr(0, 200);
    EXPECT_EQ(_b1.handshakes(), 1) << "venue 1 was never actually reached";
    EXPECT_EQ(_b0.handshakes(), 0) << "request leaked to venue 0";
}

// The negative control that makes the test above meaningful: venue 1 serves venue
// 0's cert (wrong for venue-two.test). Correct per-venue verification must reject
// it. If this passed, the check would be verifying against nothing.
TEST_P(GatewayCrossVenueTls, SecondVenueWithTheWrongCertIsRejected)
{
    start(/*route_to=*/1, /*venue_one_serves_its_own_cert=*/false);
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    // Fails closed: a rejected upstream handshake is a 5xx, never a 200.
    EXPECT_NE(Client::status_of(r), 200)
        << "venue 1 presenting venue 0's certificate was ACCEPTED; hostname "
        << "verification is not discriminating per venue. " << r.substr(0, 200);
}

// Failover on the TLS leg. Venue 0 is a closed port, so the connect fails before any
// handshake; the policy names venue 1, and the retry must build a fresh session and
// verify against venue 1's hostname. Carrying the failed venue's SNI would either
// reject a healthy provider or, far worse, accept the wrong one.
TEST_P(GatewayCrossVenueTls, FailoverBuildsANewSessionForTheNewVenue)
{
    _b1.start(_id1, "json");
    TlsConfig tls;
    tls.ca_file = write_ca_bundle(_id0, _id1);
    const uint16_t dead = closed_port();
    std::vector<Upstream> table = {
        Upstream{.ip = "127.0.0.1", .port = dead, .tls = true, .sni_host = kHost,
                 .dialect = UpstreamDialect::OpenAI},
        Upstream{.ip = "127.0.0.1", .port = _b1.port(), .tls = true, .sni_host = kHostTwo,
                 .dialect = UpstreamDialect::OpenAI},
    };
    TlsFailoverPolicy pol(0, 1);
    _gw = std::make_unique<Gateway>(uint16_t{0}, std::move(table), int64_t{0}, GetParam(),
                                    Gateway::kDefaultUpstreamIdleNs, tls, false, &pol,
                                    std::vector<std::string>{});
    _port = _gw->bound_port();
    _gt = std::thread([this] { _gw->run(); });

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    EXPECT_EQ(Client::status_of(r), 200)
        << "TLS failover did not reach the healthy venue: " << r.substr(0, 200);
    c.close();
    _gw->request_stop();
    if (_gt.joinable()) _gt.join();
    EXPECT_EQ(pol.failures, 1);
    // The Decision::tag round trip on the TLS path: the failed connect here is a TLS
    // venue, so the tag has to survive the session teardown the retry causes.
    EXPECT_EQ(pol.seen_tag, TlsFailoverPolicy::kTag);
    EXPECT_EQ(_gw->stats().upstream_failovers, 1u);
    EXPECT_EQ(_b1.handshakes(), 1) << "the retry never completed a handshake with venue 1";
}

// The negative control. Venue 1 serves venue 0's certificate, so the failover target
// must be rejected exactly as a directly-routed one is. Without this, a failover could
// be the hole that skips verification.
TEST_P(GatewayCrossVenueTls, AFailoverTargetIsStillVerified)
{
    _b1.start(_id0, "json"); // wrong identity for kHostTwo
    TlsConfig tls;
    tls.ca_file = write_ca_bundle(_id0, _id1);
    const uint16_t dead = closed_port();
    std::vector<Upstream> table = {
        Upstream{.ip = "127.0.0.1", .port = dead, .tls = true, .sni_host = kHost,
                 .dialect = UpstreamDialect::OpenAI},
        Upstream{.ip = "127.0.0.1", .port = _b1.port(), .tls = true, .sni_host = kHostTwo,
                 .dialect = UpstreamDialect::OpenAI},
    };
    TlsFailoverPolicy pol(0, 1);
    _gw = std::make_unique<Gateway>(uint16_t{0}, std::move(table), int64_t{0}, GetParam(),
                                    Gateway::kDefaultUpstreamIdleNs, tls, false, &pol,
                                    std::vector<std::string>{});
    _port = _gw->bound_port();
    _gt = std::thread([this] { _gw->run(); });

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    EXPECT_NE(Client::status_of(r), 200)
        << "a failover target with the WRONG certificate was accepted: " << r.substr(0, 200);
    c.close();
    _gw->request_stop();
    if (_gt.joinable()) _gt.join();
}

INSTANTIATE_TEST_SUITE_P(Backends, GatewayCrossVenueTls,
                         ::testing::Values(llmbridge::IoBackend::Epoll
#ifdef LLMBRIDGE_HAVE_URING
                                           ,
                                           llmbridge::IoBackend::Uring
#endif
                                           ));

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

// ── Task 5: inbound TLS, the gateway as a TLS server ────────────────────────
//
// Everything below runs on both backends. That is not ceremony: when inbound
// TLS was first wired, 920 tests passed and the convention checker was clean
// while io_uring was completely broken, because no test reached the new path.
// Thirty seconds of curl found it. These exist so the next such break is caught
// by CI instead of by hand.

// Build the request from the body so Content-Length cannot disagree with it.
// Hardcoding it was wrong twice here, and the byte-forwarding tests passed anyway
// because a truncated body still reaches a mock that answers 200 to anything. Only
// the translating test noticed. Compute, never count by hand.
static std::string make_req(const std::string& body)
{
    return "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
static const std::string kBody = R"({"model":"m","messages":[{"role":"u","content":"h"}]})";
static const std::string kStreamBody =
    R"({"model":"m","stream":true,"messages":[{"role":"u","content":"h"}]})";
static const std::string kReq = make_req(kBody);

TEST_P(GatewayTls, InboundHandshakeAndRoundTrip)
{
    // Anthropic mode on purpose: translation parses the body, so a malformed or
    // truncated request fails here instead of being byte-forwarded to a mock that
    // answers 200 to anything.
    start_inbound(UpstreamDialect::Anthropic);
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake());
    ASSERT_TRUE(c.send(kReq));
    const std::string resp = c.recv_response();
    EXPECT_NE(resp.find("200 OK"), std::string::npos) << resp.substr(0, 120);
}

// The interim has to cross the TLS session too, and it takes a different path there:
// into OpenSSL, then the backend's own ciphertext drain, never wbuf. That path is
// the one a production listener actually runs, so it gets its own test.
TEST_P(GatewayTls, InboundAnswersExpectContinueThroughTls)
{
    start_inbound(UpstreamDialect::Anthropic);
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake());
    const std::string body = R"({"model":"m","messages":[{"role":"user","content":"hi"}]})";
    ASSERT_TRUE(c.send("POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n"
                       "Expect: 100-continue\r\nContent-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n"));
    ASSERT_EQ(c.recv_until("\r\n\r\n"), "HTTP/1.1 100 Continue\r\n\r\n")
        << "over TLS the client is waiting on us just the same";
    ASSERT_TRUE(c.send(body));
    const std::string resp = c.recv_response();
    EXPECT_NE(resp.find("200 OK"), std::string::npos) << resp.substr(0, 120);
    EXPECT_EQ(resp.find("100 Continue"), std::string::npos);
}

TEST_P(GatewayTls, InboundKeepAliveServesSeveralRequestsOnOneSession)
{
    // One TLS session, three requests. Catches a write path that only works for
    // the first response, and a Session left in a bad state after a flush.
    start_inbound();
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake());
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(c.send(kReq)) << "send " << i;
        EXPECT_NE(c.recv_response().find("200 OK"), std::string::npos) << "response " << i;
    }
}

TEST_P(GatewayTls, InboundHandshakeSplitAcrossSingleByteWrites)
{
    // The ClientHello arrives one byte per read, so the gateway must hold partial
    // TLS records across many events. A single-write handshake never tests this.
    start_inbound();
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake_dribbled());
    // The gateway must still be serving afterwards: a fresh client succeeds.
    TlsClient c2;
    ASSERT_TRUE(c2.connect(_port, _ca_path));
    ASSERT_TRUE(c2.handshake());
    ASSERT_TRUE(c2.send(kReq));
    EXPECT_NE(c2.recv_response().find("200 OK"), std::string::npos);
}

TEST_P(GatewayTls, PlaintextSentToTlsListenerIsRefusedAndNothingIsServed)
{
    // A plaintext request to a TLS listener must never be answered in plaintext:
    // the reply would be readable by anyone, and the request may carry a key.
    start_inbound();
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(kReq));
    EXPECT_TRUE(c.recv_response(1500).empty()) << "a plaintext response escaped a TLS listener";
    // stats() is loop-thread state: gateway.hpp says it is valid only once that
    // thread has been joined, and reading it live is a data race TSan reports.
    // The empty response above already means the gateway processed the bytes,
    // counted the failure and closed the connection; the join makes the read safe.
    _gw->request_stop();
    if (_gt.joinable()) _gt.join();
    EXPECT_EQ(_gw->stats().requests, 0u);
    // The log line for this sits at DEBUG, because a public listener collects
    // scanners and one WARN each buries everything else. The counter is what keeps
    // it visible: silenced and uncounted would mean a customer who cannot handshake
    // produces no evidence at all on a production build.
    EXPECT_EQ(_gw->stats().client_tls_handshake_failures, 1u);
}


// ── 3.8: lifetime. These are the hazards, expressed as tests ────────────────
//
// io_uring can hold submitted operations referencing a Connection after the
// point the code would like to free it. Inbound TLS applies that hazard to a
// second class of connection and hangs an SSL object with its own lifetime off
// it. Run under ASan for these to mean anything.

TEST_P(GatewayTls, ClientVanishesMidHandshake)
{
    start_inbound();
    for (int i = 0; i < 20; ++i)
    {
        TlsClient c;
        ASSERT_TRUE(c.connect(_port, _ca_path));
        // Half a ClientHello, then gone: a send of our handshake reply may well be
        // in flight when the peer disappears.
        const char partial[] = "\x16\x03\x01\x00\x2f\x01";
        // Best-effort by design: the peer is about to vanish, so a short write is the
        // scenario, not an error. `(void)!` is how this repo discards a
        // warn_unused_result write; GCC rejects a bare (void) cast.
        (void)!::write(c.fd(), partial, sizeof partial - 1);
        c.close();
    }
    // Survival check: the gateway still serves.
    TlsClient ok;
    ASSERT_TRUE(ok.connect(_port, _ca_path));
    ASSERT_TRUE(ok.handshake());
    ASSERT_TRUE(ok.send(kReq));
    EXPECT_NE(ok.recv_response().find("200 OK"), std::string::npos);
}

TEST_P(GatewayTls, ClientVanishesMidRequest)
{
    start_inbound();
    for (int i = 0; i < 20; ++i)
    {
        TlsClient c;
        ASSERT_TRUE(c.connect(_port, _ca_path));
        ASSERT_TRUE(c.handshake());
        ASSERT_TRUE(c.send(kReq.substr(0, 40)));  // headers only, no body
        c.close();
    }
    TlsClient ok;
    ASSERT_TRUE(ok.connect(_port, _ca_path));
    ASSERT_TRUE(ok.handshake());
    ASSERT_TRUE(ok.send(kReq));
    EXPECT_NE(ok.recv_response().find("200 OK"), std::string::npos);
}

TEST_P(GatewayTls, ClientVanishesImmediatelyAfterSendingAFullRequest)
{
    // The response is built and a send submitted while the peer is already gone.
    // On io_uring that is the case where a Session could be freed with an SQE
    // still pointing into its ciphertext buffer.
    start_inbound();
    for (int i = 0; i < 20; ++i)
    {
        TlsClient c;
        ASSERT_TRUE(c.connect(_port, _ca_path));
        ASSERT_TRUE(c.handshake());
        ASSERT_TRUE(c.send(kReq));
        c.close();  // no read at all
    }
    TlsClient ok;
    ASSERT_TRUE(ok.connect(_port, _ca_path));
    ASSERT_TRUE(ok.handshake());
    ASSERT_TRUE(ok.send(kReq));
    EXPECT_NE(ok.recv_response().find("200 OK"), std::string::npos);
}

TEST_P(GatewayTls, ManyConcurrentInboundTlsClientsAllComplete)
{
    start_inbound();
    constexpr int kN = 24;
    std::vector<std::thread> ts;
    std::atomic<int> ok{0};
    for (int i = 0; i < kN; ++i)
        ts.emplace_back([&] {
            TlsClient c;
            if (!c.connect(_port, _ca_path) || !c.handshake()) return;
            if (!c.send(kReq)) return;
            if (c.recv_response(8000).find("200 OK") != std::string::npos) ++ok;
        });
    for (auto& t : ts) t.join();
    EXPECT_EQ(ok.load(), kN);
}

TEST_P(GatewayTls, InboundTlsStreamsSseEndToEnd)
{
    start_inbound(UpstreamDialect::Anthropic, "sse");
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake());
    ASSERT_TRUE(c.send(make_req(kStreamBody)));
    // Streamed responses are close-delimited, so read raw until the stream ends.
    std::string all;
    char tmp[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const int n = c.read_raw(tmp, sizeof tmp);
        if (n <= 0) break;
        all.append(tmp, static_cast<size_t>(n));
        if (all.find("[DONE]") != std::string::npos) break;
    }
    EXPECT_NE(all.find("text/event-stream"), std::string::npos) << all.substr(0, 200);
    EXPECT_NE(all.find("[DONE]"), std::string::npos);
}

// 4.7: a client that completes the handshake and then never reads must not grow
// the gateway without bound through the write BIO. Written as a measurement, not an
// argument: the upstream floods ~32 MiB of stream while the client reads nothing,
// and the assertion is on the peak ciphertext staged for that connection. If the
// buffer were unbounded the peak would track what was produced.
TEST_P(GatewayTls, ClientThatNeverReadsCannotGrowUsWithoutBound)
{
    start_inbound(UpstreamDialect::Anthropic, "sse-flood");
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake());
    ASSERT_TRUE(c.send(make_req(kStreamBody)));

    // Read nothing. Give the gateway time to pull everything the upstream will give
    // it and stage as much as it is willing to stage.
    std::this_thread::sleep_for(std::chrono::seconds(3));

    c.close();
    _gw->request_stop();
    if (_gt.joinable()) _gt.join();
    const uint64_t peak = _gw->stats().tls_buffered_peak;
    std::fprintf(stderr, "MEASURED tls_buffered_peak = %llu bytes\n",
                 static_cast<unsigned long long>(peak));
    // The DESIGN bound: one event stages at most kEpMaxReadPerEvent (1 MiB) plus a TLS
    // record, and the next event cannot start until the client's write makes progress,
    // because a partial write pauses the upstream read.
    //
    // The old bound was 12 MiB and it passed by luck. Back-pressure only engaged after
    // a pump, and the drain loop had no byte budget, so one readable event could pull
    // as much as a local flooder could supply: measured 100 KB idle and 33 MB on a
    // loaded machine, which read as a flaky test and was an unbounded-growth bug.
    // Measured after the fix, stable across runs and under 12 busy cores:
    // ~1.06 MB on epoll, ~283 KB on io_uring.
    // 4 MiB: above the ~1.4 MiB one event can stage (kEpMaxReadPerEvent plus a TLS
    // record) and far below what the unbounded version reached.
    //
    // Honest limit of this test: it detects the unbounded version only sometimes,
    // because that bug needs the event loop to fall behind a flooding provider, which
    // depends on machine load. Measured against the pre-fix code: 2 failures in 6 runs
    // under 12 busy cores, 1 in 6 idle. Shrinking the client's receive window did not
    // help. What actually proved the fix was a 5-run A/B on process RSS, recorded in
    // the CHANGELOG: 228 MB peak before, 58 MB after. Treat this as a smoke test.
    EXPECT_LT(peak, 4u * 1024 * 1024)
        << "staged " << peak << " bytes for one non-reading client";
    // Not vacuous: a peak of zero would mean the flood never flowed, and the bound
    // above would be measuring nothing. TheFloodControlActuallyDelivers is the other
    // half of that argument.
    EXPECT_GT(peak, 0u) << "nothing was ever staged, so the bound proves nothing";
}

// The control for the test above, and it is not optional. A peak of ~1 KiB is the
// right answer only if the flood actually flows when somebody reads it; if the mock
// were broken, the bounded result would be measuring nothing. This drains the same
// stream and asserts megabytes arrive.
TEST_P(GatewayTls, TheFloodControlActuallyDelivers)
{
    start_inbound(UpstreamDialect::Anthropic, "sse-flood");
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake());
    ASSERT_TRUE(c.send(make_req(kStreamBody)));

    // Target 4 MiB, deliberately below the 8 MiB io_uring drop cap
    // (kUrStreamBufCap).
    size_t total = 0;
    char tmp[65536];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const int n = c.read_raw(tmp, sizeof tmp);
        if (n <= 0) break;
        total += static_cast<size_t>(n);
        if (total > 4u * 1024 * 1024) break; // enough to prove it flows
    }
    std::fprintf(stderr, "CONTROL delivered %zu bytes to a reading client\n", total);
    EXPECT_GT(total, 4u * 1024 * 1024);
}

// 4.8: a connection still in the handshake must not be able to reach the pooled
// upstream. Structurally it cannot, because acquiring an upstream needs a framed
// request and framing needs plaintext, but "structurally it cannot" is the kind of
// claim this file has been wrong about before. Measured with a pool that is known
// to be non-empty, so the test can tell "did not touch it" from "there was nothing
// to touch".
TEST_P(GatewayTls, HandshakingClientCannotReachThePooledUpstream)
{
    start_inbound(UpstreamDialect::Anthropic);

    // Put exactly one upstream in the pool via a completed request. Release runs
    // synchronously with the response the client just read, so no polling is needed,
    // and polling would be wrong: the seams below read gateway state that only the
    // loop thread may touch while it runs. See the note on pooled_upstream_count().
    {
        TlsClient warm;
        ASSERT_TRUE(warm.connect(_port, _ca_path));
        ASSERT_TRUE(warm.handshake());
        ASSERT_TRUE(warm.send(kReq));
        ASSERT_NE(warm.recv_response().find("200 OK"), std::string::npos);
    }

    // Six clients that start a handshake and never finish one.
    std::vector<int> fds;
    for (int i = 0; i < 6; ++i)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(_port);
        ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
        ASSERT_GT(::write(fd, "\x16\x03\x01\x00\x40", 5), 0); // record header, no body
        fds.push_back(fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    for (int fd : fds) ::close(fd);

    _gw->request_stop();
    if (_gt.joinable()) _gt.join();
    // Exactly one upstream was ever opened, by the warm request. Had a handshaking
    // client reached the pool it would have taken that connection and the next
    // request would have opened another; had it bypassed the pool it would have
    // opened one directly. Either way this count moves.
    EXPECT_EQ(_gw->stats().upstream_conns_opened, 1u);
    EXPECT_EQ(_gw->pooled_upstream_count(), 1u) << "the pooled conn was taken or dropped";
}

// The end-to-end half. The unit test in net/tests proves no ERROR string
// carries key material; this proves nothing the process actually emits does, over a
// run that includes the failure paths. stderr is captured for the whole run and
// every byte any client received is kept, then both are searched for the base64
// body of the private key the listener is using.
TEST_P(GatewayTls, InboundTlsEmitsNoKeyMaterialAnywhere)
{
    // Capture stderr for the duration, including the gateway's startup lines.
    const std::string cap = std::string(::testing::TempDir()) + "llmbridge_46_" +
                            std::to_string(static_cast<long>(::getpid())) + ".log";
    const int saved = ::dup(STDERR_FILENO);
    ASSERT_GE(saved, 0);
    const int capfd = ::open(cap.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    ASSERT_GE(capfd, 0);
    ASSERT_GE(::dup2(capfd, STDERR_FILENO), 0);

    std::string client_bytes;
    start_inbound(UpstreamDialect::Anthropic);
    {
        // (a) a healthy request
        TlsClient ok;
        ASSERT_TRUE(ok.connect(_port, _ca_path));
        ASSERT_TRUE(ok.handshake());
        ASSERT_TRUE(ok.send(kReq));
        client_bytes += ok.recv_response();

        // (b) plaintext at the TLS listener, which fails the handshake
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(_port);
        ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
        const std::string plain = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        ASSERT_GT(::write(fd, plain.data(), plain.size()), 0);
        char buf[4096];
        for (;;)
        {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n <= 0) break;
            client_bytes.append(buf, static_cast<size_t>(n));
        }
        ::close(fd);

        // (c) a truncated handshake, abandoned
        const int fd2 = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_EQ(::connect(fd2, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
        ASSERT_GT(::write(fd2, "\x16\x03\x01\x00\x40", 5), 0);
        ::close(fd2);
    }
    _gw->request_stop();
    if (_gt.joinable()) _gt.join();

    ::fflush(stderr);
    ASSERT_GE(::dup2(saved, STDERR_FILENO), 0);
    ::close(saved);
    ::close(capfd);
    std::ifstream in(cap);
    const std::string log((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ::unlink(cap.c_str());

    // The material: the base64 body of the key this listener is serving with.
    std::ifstream kin(_id.write_key_pem());
    std::vector<std::string> secret;
    for (std::string line; std::getline(kin, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("-----", 0) != 0 && line.size() >= 32) secret.push_back(line);
    }
    ASSERT_FALSE(secret.empty());
    ASSERT_FALSE(log.empty()) << "captured nothing, so this test would pass vacuously";

    for (const std::string& line : secret)
    {
        EXPECT_EQ(log.find(line), std::string::npos) << "key material reached stderr";
        EXPECT_EQ(client_bytes.find(line), std::string::npos) << "key material reached a client";
    }
}

// 6.1: the inbound handshake completes before t0, so every other number in
// LATENCY.md is blind to it. accept(TLS) is the only place its cost is visible.
// Three claims, because one of them alone would pass for the wrong reason: it
// records on an inbound-TLS listener, it counts one sample per handshake (not per
// request), and it stays empty when the listener is plaintext.
TEST_P(GatewayTls, InboundHandshakeIsRecordedInAcceptTls)
{
    start_inbound(UpstreamDialect::Anthropic);
    for (int i = 0; i < 3; ++i)
    {
        TlsClient c;
        ASSERT_TRUE(c.connect(_port, _ca_path));
        ASSERT_TRUE(c.handshake());
        ASSERT_TRUE(c.send(kReq)); // two requests on the second session, so samples
        if (i == 1)                // cannot be per-request
        {
            ASSERT_TRUE(c.send(kReq)); // braced: ASSERT_* expands to an if/else, and an
        }                              // unbraced body is a dangling else under GCC
        ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos);
        if (i == 1)
        {
            ASSERT_NE(c.recv_response().find("200 OK"), std::string::npos);
        }
    }
    _gw->request_stop();
    if (_gt.joinable()) _gt.join();

    EXPECT_EQ(_gw->stats().accept_tls.total(), 3u) << "one sample per inbound handshake";
    EXPECT_EQ(_gw->stats().requests, 4u);
}

TEST_P(GatewayTls, AcceptTlsStaysEmptyOnAPlaintextListener)
{
    start(UpstreamDialect::Anthropic); // TLS upstream, plaintext listener
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    ASSERT_EQ(Client::status_of(c.recv_response()), 200);
    _gw->request_stop();
    if (_gt.joinable()) _gt.join();

    // The upstream handshake must not be booked here: it is the one that cancels.
    EXPECT_EQ(_gw->stats().accept_tls.total(), 0u);
    EXPECT_GT(_gw->stats().connect.total(), 0u) << "the upstream handshake still lands in connect";
}

// ── The missing row of the matrix: TLS in, plaintext upstream ───────────────
//
// Every other combination of {client leg} x {upstream leg} x {streaming} was
// covered; this row was not, and it is the one where the two legs disagree.
// tls_required(), wbuf_on_wire() and tls_invariant_ok() all read direction from
// the leg, so a leg mix-up is invisible until the legs differ.

TEST_P(GatewayTls, ClientTlsWithPlaintextUpstreamRoundTrips)
{
    start_client_tls_only();
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake());
    ASSERT_TRUE(c.send(kReq));
    EXPECT_NE(c.recv_response().find("200 OK"), std::string::npos);
}

TEST_P(GatewayTls, ClientTlsWithPlaintextUpstreamStreams)
{
    start_client_tls_only(UpstreamDialect::Anthropic, "sse");
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake());
    ASSERT_TRUE(c.send(make_req(kStreamBody)));
    std::string all;
    char tmp[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const int n = c.read_raw(tmp, sizeof tmp);
        if (n <= 0) break;
        all.append(tmp, static_cast<size_t>(n));
        if (all.find("[DONE]") != std::string::npos) break;
    }
    EXPECT_NE(all.find("text/event-stream"), std::string::npos) << all.substr(0, 160);
    EXPECT_NE(all.find("[DONE]"), std::string::npos);
}

// ── A stalled client and a 2 MB body, through TLS ──────────────────────────
//
// This forces the partial-write path: the response cannot fit in the socket
// buffer, the client stops reading, and the gateway has to carry ciphertext
// across many writes without losing or duplicating any of it. Nothing else in
// the suite covered that.
//
// What it does not test, stated because the first version of this comment
// claimed otherwise. It does not distinguish "fed to the Session" from "on the
// wire", even though that distinction is real and documented on `woff`.
// Replacing wbuf_on_wire() with woff alone leaves this test passing, because
// both call sites already establish the same fact by other means: on epoll
// `*done` is gated on ep_tls_flush's `flushed`, which is set only after tls_out
// drains completely, and on io_uring the send_inflight guard covers it. So
// wbuf_on_wire() is defensive today and not load-bearing, and no test at the
// current call sites can show otherwise. It earns its place by being the correct
// thing for a future call site that lacks those guards, which is a weaker claim
// than "verified" and should not be written up as one.
TEST_P(GatewayTls, SlowClientReceivesAFullLargeBodyThroughTls)
{
    start_inbound(UpstreamDialect::OpenAI, "big");
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake());
    ASSERT_TRUE(c.send(kReq));

    // Stall long enough for the gateway to fill the socket buffer and be forced
    // into a partial write. This is the whole point of the test.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    constexpr size_t kExpectBody = 2 * 1024 * 1024;
    std::string all;
    char tmp[16384];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const int n = c.read_raw(tmp, sizeof tmp);
        if (n <= 0) break;
        all.append(tmp, static_cast<size_t>(n));
        const size_t hdr = all.find("\r\n\r\n");
        if (hdr != std::string::npos && all.size() - (hdr + 4) >= kExpectBody) break;
    }
    const size_t hdr = all.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos) << "no response head";
    EXPECT_EQ(all.size() - (hdr + 4), kExpectBody)
        << "body truncated: the gateway treated 'encrypted' as 'sent'";
}

// The client setup deadline, which a mutation sweep found untested: deleting it
// outright broke no test, because 30 seconds is longer than any suite will wait.
// An untested deadline on an internet-facing listener is the same as no deadline.
TEST_P(GatewayTls, HalfOpenClientsAreDroppedAtTheSetupDeadline)
{
    // 300 ms deadline, applied before the loop thread starts. See start_inbound().
    start_inbound(UpstreamDialect::OpenAI, "json", 300 * 1000 * 1000LL);

    // Connect and send a fragment that can never frame as a request. On a TLS
    // listener these never even finish a handshake, which is the case that used to
    // hold a slot forever.
    std::vector<int> fds;
    for (int i = 0; i < 12; ++i)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(_port);
        ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
        ASSERT_GT(::write(fd, "\x16\x03\x01", 3), 0);
        fds.push_back(fd);
    }

    // Comfortably past the 300 ms deadline. This used to poll stats() in a loop,
    // which reads loop-thread state from the test thread; TSan reported it, and a
    // suite with known-benign races is a suite where a real one gets ignored.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // A real client is unaffected: the deadline must not touch a peer that
    // completes a request. Done before the join, since it needs a live gateway.
    TlsClient ok;
    ASSERT_TRUE(ok.connect(_port, _ca_path));
    ASSERT_TRUE(ok.handshake());
    ASSERT_TRUE(ok.send(kReq));
    EXPECT_NE(ok.recv_response().find("200 OK"), std::string::npos);

    for (int fd : fds) ::close(fd);
    _gw->request_stop();
    if (_gt.joinable()) _gt.join();
    EXPECT_GE(_gw->stats().client_setup_timeouts, 12u)
        << "half-open clients were not dropped; the deadline is not enforced";
}

// The gap a mutation sweep found: deleting `stream_ended = true` from
// stream_truncate() broke no test, because every mock closes promptly after
// sending and the client goes away either way.
//
// Here the provider corrupts the chunked framing and then holds the connection
// open. stream_ended is what gates finalize_stream, so without it the gateway
// resumes reading from an upstream that will never speak again and the client
// socket is held until the 120 s idle sweep. With it, the client is closed at
// once. The assertion is therefore about time, which is the only way the two
// behaviours differ.
TEST_P(GatewayTls, CorruptStreamFromAProviderThatHoldsTheConnectionStillClosesTheClient)
{
    start_inbound(UpstreamDialect::Anthropic, "sse-badchunk-hold");
    TlsClient c;
    ASSERT_TRUE(c.connect(_port, _ca_path));
    ASSERT_TRUE(c.handshake());
    ASSERT_TRUE(c.send(make_req(kStreamBody)));

    // Read until the gateway closes us. A clean EOF here is the whole assertion:
    // it must arrive in seconds, never at the 120 s upstream idle timeout.
    const auto t0 = std::chrono::steady_clock::now();
    std::string all;
    char tmp[4096];
    bool closed = false;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(15))
    {
        const int n = c.read_raw(tmp, sizeof tmp);
        if (n <= 0) { closed = true; break; }
        all.append(tmp, static_cast<size_t>(n));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    ASSERT_TRUE(closed) << "the gateway never closed the client; it is waiting for a "
                        << "provider that never will. elapsed=" << elapsed.count() << "ms";
    EXPECT_LT(elapsed.count(), 10000)
        << "closed only after " << elapsed.count() << "ms, which suggests the idle "
        << "sweep did it rather than the truncation path";
    // Truncated honestly: the client saw real tokens and no fabricated terminator.
    EXPECT_NE(all.find("hola"), std::string::npos) << "no tokens reached the client";
    EXPECT_EQ(all.find("[DONE]"), std::string::npos) << "fabricated a clean finish";
}
