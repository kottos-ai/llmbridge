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
#include <array>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "net/http.hpp"
#include "provider/json.hpp" // reassemble the streamed OpenAI chunks

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
    // Same, but the upstream declares Connection: close, so the gateway must not pool it.
    std::string http_close(const std::string& body)
    {
        return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    }

    // A minimal valid OpenAI chat-completion request carrying `content`.
    // OpenAI-shaped request with arbitrary extra header lines (auth tests).
    std::string openai_request_hdrs(const std::string& content, const std::string& extra_hdrs)
    {
        const std::string body =
            "{\"model\":\"gpt-4o\",\"max_tokens\":64,\"messages\":[{\"role\":\"user\",\"content\":\"" +
            content + "\"}]}";
        return "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n" + extra_hdrs +
               "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }

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
            if (_rcvbuf > 0) ::setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &_rcvbuf, sizeof(_rcvbuf));
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
            // won't interrupt: shut the accepted fds down to unblock them.
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
        // Respond once (keep-alive), then close the connection, which simulates a
        // provider dropping an idle pooled keep-alive connection.
        void set_close_after_first(bool b) { _close_after_first = b; }
        // Kill only the first N accepted connections after one response. Where
        // set_close_after_first drops EVERY connection, this leaves later ones alive
        // and poolable, which is what a test needs when the thing under test is where
        // a RETRIED connection ends up, not the retry itself.
        void set_close_after_first_n(int n) { _close_first_n = n; }
        // Frame the reply with Transfer-Encoding: chunked and NO Content-Length
        // what real providers actually do for a non-streaming completion, since the
        // body length is unknown when headers are sent. `n` = chunks to split into.
        void set_chunked_response(int n) { _chunked_chunks = n; }
        // Stall modes for timeout tests: 1 = read the request then never reply;
        // 2 = send half the response, then hold the connection open forever.
        void set_stall(int mode) { _stall = mode; }
        // Answer with a complete keep-alive response the instant the connection is
        // accepted, WITHOUT reading the request, and never read afterwards. Combined
        // with set_small_rcvbuf() this pins the gateway's request send half-written
        // while a full response is already framed, which is what a provider doing an
        // early reject (413/401 on the headers) of a large body looks like.
        void set_reply_before_read(bool b) { _reply_before_read = b; }
        // Shrink the receive window on accepted sockets, so the gateway's write to
        // this backend blocks after a few hundred KB instead of many MB.
        void set_small_rcvbuf(int b) { _rcvbuf = b; }
        int requests_seen() const { return _requests_seen.load(); }
        void clear_last_request()
        {
            std::lock_guard<std::mutex> lk(_mu);
            _last_request.clear();
        }

        std::string last_request()
        {
            std::lock_guard<std::mutex> lk(_mu);
            return _last_request;
        }
        // EVERY request the upstream saw, concatenated, for leak tests that must
        // assert a credential appeared in NO request, not merely the most recent.
        std::string all_requests()
        {
            std::lock_guard<std::mutex> lk(_mu);
            return _all_requests;
        }

        // Re-frame a Content-Length reply as a chunked one, splitting the body.
        // Line-wise on purpose: an earlier version erased the Content-Length header
        // by byte range, and because it is the LAST header that left a dangling CRLF
        // which terminated the header block early, producing a response with
        // neither framing header. Rebuild from lines so header order cannot matter.
        static std::string to_chunked(const std::string& resp, int nchunks)
        {
            const size_t hdr_end = resp.find("\r\n\r\n");
            if (hdr_end == std::string::npos) return resp;
            const std::string body = resp.substr(hdr_end + 4);

            std::string out;
            size_t pos = 0;
            const std::string head = resp.substr(0, hdr_end);
            while (pos <= head.size())
            {
                size_t eol = head.find("\r\n", pos);
                if (eol == std::string::npos) eol = head.size();
                const std::string line = head.substr(pos, eol - pos);
                // Drop Content-Length: the two framings must never both appear.
                if (line.rfind("Content-Length:", 0) != 0 && !line.empty())
                {
                    out += line;
                    out += "\r\n";
                }
                if (eol == head.size()) break;
                pos = eol + 2;
            }
            out += "Transfer-Encoding: chunked\r\n\r\n";

            const size_t per = nchunks > 0 ? (body.size() + nchunks - 1) / nchunks : body.size();
            for (size_t off = 0; off < body.size(); off += per)
            {
                const std::string part = body.substr(off, std::min(per, body.size() - off));
                char len[16];
                std::snprintf(len, sizeof len, "%zx", part.size());
                out += std::string(len) + "\r\n" + part + "\r\n";
            }
            out += "0\r\n\r\n";
            return out;
        }

    private:
        void accept_loop()
        {
            for (;;)
            {
                int c = ::accept(_fd, nullptr, nullptr);
                if (c < 0) return;
                const int idx = _accepted++;
                { std::lock_guard<std::mutex> lk(_mu); _client_fds.push_back(c); }
                _conns.emplace_back([this, c, idx] { handle(c, idx); });
            }
        }
        void handle(int c, int conn_index = 0)
        {
            std::string buf;
            char tmp[16384];
            if (_reply_before_read)
            {
                const std::string resp = _resp_override.empty() ? canned_response() : _resp_override;
                (void)!::write(c, resp.data(), resp.size());
                while (!_stop) { timespec ts{0, 20000000}; nanosleep(&ts, nullptr); }
                ::close(c);
                return;
            }
            while (!_stop)
            {
                llmbridge::net::http::Message m;
                while (llmbridge::net::http::parse_request(buf, m) != llmbridge::net::http::FrameStatus::Complete)
                {
                    ssize_t n = ::read(c, tmp, sizeof(tmp));
                    if (n <= 0) { ::close(c); return; }
                    buf.append(tmp, static_cast<size_t>(n));
                }
                {
                    std::lock_guard<std::mutex> lk(_mu);
                    _last_request.assign(buf.data(), m.total_len);
                    _all_requests.append(buf.data(), m.total_len);
                }
                _requests_seen.fetch_add(1, std::memory_order_relaxed);
                buf.erase(0, m.total_len);

                std::string resp = _resp_override.empty() ? canned_response() : _resp_override;
                if (_chunked_chunks > 0) resp = to_chunked(resp, _chunked_chunks);
                if (_stall == 1) // never answer; hold the connection open
                {
                    while (!_stop) { timespec ts{0, 20000000}; nanosleep(&ts, nullptr); }
                    ::close(c);
                    return;
                }
                if (_stall == 2) // partial answer, then hang (stalled mid-stream)
                {
                    (void)!::write(c, resp.data(), resp.size() / 2);
                    while (!_stop) { timespec ts{0, 20000000}; nanosleep(&ts, nullptr); }
                    ::close(c);
                    return;
                }
                if (_close_mid)
                {
                    // Send a partial response then drop. This exercises the gateway's
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
                if (_close_first_n > 0 && conn_index < _close_first_n) { ::close(c); return; }
                if (!m.keep_alive) { ::close(c); return; }
            }
            ::close(c);
        }

        int _fd = -1;
        uint16_t _port = 0;
        std::string _resp_override;
        int _trickle_chunk = 0;
        int _chunked_chunks = 0;
        bool _close_mid = false;
        bool _close_after_first = false;
        int _close_first_n = 0;
        std::atomic<int> _accepted{0};
        int _stall = 0;
        bool _reply_before_read = false;
        int _rcvbuf = 0;
        std::atomic<int> _requests_seen{0};
        std::string _last_request;
        std::string _all_requests;
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
        bool connect(uint16_t port, int rcvbuf = 0)
        {
            _fd = ::socket(AF_INET, SOCK_STREAM, 0);
            // A tiny receive window makes the gateway's writes block quickly, so a
            // non-reading client deterministically triggers write backpressure.
            if (rcvbuf > 0) ::setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
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
                llmbridge::net::http::Message m;
                if (llmbridge::net::http::parse_request(_buf, m) == llmbridge::net::http::FrameStatus::Complete)
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
        // HTTP status code of an already-received response ("HTTP/1.1 XYZ ...").
        static int status_of(const std::string& r)
        {
            if (r.size() < 12 || r.compare(0, 5, "HTTP/") != 0) return 0;
            return (r[9] - '0') * 100 + (r[10] - '0') * 10 + (r[11] - '0');
        }
        // HTTP status code of the next framed response ("HTTP/1.1 XYZ ..."); 0 if
        // no response arrives (closed/timeout).
        int recv_status(int timeout_ms = 2000)
        {
            const std::string r = recv_response(timeout_ms);
            if (r.size() < 12 || r.compare(0, 5, "HTTP/") != 0) return 0;
            return (r[9] - '0') * 100 + (r[10] - '0') * 10 + (r[11] - '0');
        }
        // Read everything until the peer closes the connection (for close-delimited
        // responses like a streamed SSE body). Returns all bytes received.
        std::string recv_all(int timeout_ms = 3000)
        {
            std::string out = std::move(_buf);
            _buf.clear();
            char tmp[8192];
            for (;;)
            {
                pollfd p{_fd, POLLIN, 0};
                if (::poll(&p, 1, timeout_ms) <= 0) return out; // timeout: return what we have
                ssize_t n = ::read(_fd, tmp, sizeof(tmp));
                if (n <= 0) return out; // EOF or error: stream ended
                out.append(tmp, static_cast<size_t>(n));
            }
        }

        /// One read. Whatever the gateway has produced SO FAR, which is the only way
        /// to tell a stream from a buffered response: recv_all cannot distinguish
        /// "arrived in pieces" from "arrived at the end".
        std::string recv_some(int timeout_ms = 2000)
        {
            if (!_buf.empty()) { std::string out = std::move(_buf); _buf.clear(); return out; }
            char tmp[8192];
            pollfd p{_fd, POLLIN, 0};
            if (::poll(&p, 1, timeout_ms) <= 0) return {};
            const ssize_t n = ::read(_fd, tmp, sizeof(tmp));
            return n > 0 ? std::string(tmp, static_cast<size_t>(n)) : std::string{};
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
                   llmbridge::IoBackend backend = llmbridge::IoBackend::Epoll,
                   int64_t upstream_idle_ns = Gateway::kDefaultUpstreamIdleNs,
                   unsigned uring_buf_count = 0, bool timing_headers = false,
                   int64_t client_idle_ns = -1, int64_t pool_idle_ns = -1)
        {
            uint16_t up_port;
            if (with_backend) { _backend.start(); up_port = _backend.port(); }
            else up_port = free_port(); // nothing listening -> connection refused

            // `_policy` is a member: start() already takes nine positional args and a
            // tenth is how a caller silently passes the wrong one. Set before start().
            if (_upstreams.empty())
                _gw = std::make_unique<Gateway>(0, "127.0.0.1", up_port, warmup_ns, translate,
                                                backend, upstream_idle_ns, llmbridge::TlsConfig{},
                                                timing_headers, _policy, _strip_headers);
            else
                _gw = std::make_unique<Gateway>(0, _upstreams, warmup_ns, backend,
                                                upstream_idle_ns, llmbridge::TlsConfig{},
                                                timing_headers, _policy, _strip_headers);
            if (uring_buf_count) _gw->set_uring_buf_count_for_test(uring_buf_count);
            if (_sink) _gw->set_request_sink(_sink, _sink_capture);
            // Applied BEFORE the loop thread exists. Setting it afterwards writes
            // state sweep_idle reads, which is a genuine data race TSan reports.
            if (client_idle_ns >= 0) _gw->set_client_idle_ns(client_idle_ns);
            if (pool_idle_ns >= 0) _gw->set_pool_idle_ns(pool_idle_ns);
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
        llmbridge::Policy* _policy = nullptr; // null = stock build, no seam consulted
        llmbridge::RequestSink* _sink = nullptr; // set before start(), like _policy
        std::vector<std::string> _sink_capture;
        std::vector<std::string> _strip_headers; // empty = stock build, nothing dropped
        std::vector<llmbridge::Upstream> _upstreams; // empty = the single-upstream form
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

// A provider may answer BEFORE it has read the whole request (an early 413/401 on
// the headers of a large body), and the upstream recv is armed before the send, so
// a complete response can be framed while our request SEND is still outstanding.
// Releasing the upstream then scrubs and re-uses a buffer the transport has not
// finished with. Under io_uring that buffer is the target of a live SQE.
// An established client that goes quiet must eventually lose its descriptor. The
// setup deadline does not cover this: ever_framed latches on the first request and
// that check stops applying for the connection's life, so before this a client could
// send one request and then hold an fd forever. That is a descriptor bound for an
// exposed listener, NOT a load-balancing device: the default is three days, because
// anything short charges a reconnecting client a fresh TCP+TLS handshake to solve a
// problem the client cannot see.
// --pool-idle: how long a pooled upstream may sit unused before being closed. The
// 30 s default is a compromise for one upstream and one worker, and it is the wrong
// number as soon as either multiplies: each worker has its own pool, so traffic
// divides, each pool crosses the line more often, and every crossing costs the next
// request a full reconnect. Hence a knob.
class ProxyPoolIdle : public ProxyIT,
                      public ::testing::WithParamInterface<llmbridge::IoBackend> {};

TEST_P(ProxyPoolIdle, ShortWindowReapsThePooledUpstream)
{
    start(0, true, TranslateMode::None, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, false,
          -1, 200 * 1000 * 1000LL); // 200 ms pool window
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(make_request()));
        ASSERT_EQ(Client::status_of(c.recv_response()), 200);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // 6x the window
    shutdown();
    EXPECT_EQ(_gw->pooled_upstream_count(), 0u) << "the idle upstream was not reaped";
}

// The control, and it is what makes the test above mean anything: with the default
// window the same connection is still pooled at the same instant, so the reaping is
// attributable to the flag and not to the connection dying on its own.
TEST_P(ProxyPoolIdle, DefaultWindowKeepsItPooled)
{
    start(0, true, TranslateMode::None, GetParam());
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(make_request()));
        ASSERT_EQ(Client::status_of(c.recv_response()), 200);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    shutdown();
    EXPECT_EQ(_gw->pooled_upstream_count(), 1u) << "reaped far inside the 30 s default";
}

// 0 disables reaping outright, for a deployment that would rather hold connections
// than pay reconnects.
TEST_P(ProxyPoolIdle, ZeroDisablesPoolReaping)
{
    start(0, true, TranslateMode::None, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, false,
          -1, 0);
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(make_request()));
        ASSERT_EQ(Client::status_of(c.recv_response()), 200);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    shutdown();
    EXPECT_EQ(_gw->pooled_upstream_count(), 1u);
}
INSTANTIATE_TEST_SUITE_P(Backends, ProxyPoolIdle,
                         ::testing::Values(llmbridge::IoBackend::Epoll,
                                           llmbridge::IoBackend::Uring));

class ProxyClientIdle : public ProxyIT,
                        public ::testing::WithParamInterface<llmbridge::IoBackend> {};

TEST_P(ProxyClientIdle, QuietEstablishedClientIsReaped)
{
    // 300 ms, applied before the loop thread starts; see start().
    start(0, true, TranslateMode::None, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, false,
          300 * 1000 * 1000LL);

    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    ASSERT_EQ(Client::status_of(c.recv_response()), 200); // now established
    // ... and then say nothing at all.
    EXPECT_TRUE(c.wait_closed(5000)) << "a quiet established client kept its descriptor";

    shutdown();
    EXPECT_GE(_gw->stats().client_idle_timeouts, 1u);
    EXPECT_EQ(_gw->stats().client_setup_timeouts, 0u) << "wrong reaper fired";
}

// The half that matters more: it must not fire on a client that is still being served.
// A slow provider is not an idle client, and reaping mid-request would turn somebody
// else's latency into our dropped connection.
TEST_P(ProxyClientIdle, InFlightRequestIsNotReaped)
{
    _backend.set_stall(1); // read the request, never reply
    start(0, true, TranslateMode::None, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, false,
          300 * 1000 * 1000LL);

    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    ASSERT_TRUE(c.send(make_request())); // establish, then a request that hangs
    std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // 4x the deadline

    shutdown();
    EXPECT_EQ(_gw->stats().client_idle_timeouts, 0u)
        << "a client waiting on a slow provider was reaped as idle";
}

// And it must be disableable, because a loopback sidecar has no reason to pay for it.
TEST_P(ProxyClientIdle, ZeroDisablesTheReaper)
{
    start(0, true, TranslateMode::None, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, false, 0);

    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    ASSERT_EQ(Client::status_of(c.recv_response()), 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    shutdown();
    EXPECT_EQ(_gw->stats().client_idle_timeouts, 0u);
}
INSTANTIATE_TEST_SUITE_P(Backends, ProxyClientIdle,
                         ::testing::Values(llmbridge::IoBackend::Epoll,
                                           llmbridge::IoBackend::Uring));

class ProxyEarlyResponse : public ProxyIT,
                           public ::testing::WithParamInterface<llmbridge::IoBackend> {};
TEST_P(ProxyEarlyResponse, PooledUpstreamStaysUsable)
{
    _backend.set_small_rcvbuf(4096); // gateway's write blocks after a few hundred KB
    _backend.set_reply_before_read(true);
    start(0, true, TranslateMode::None, GetParam());

    // Larger than any socket buffer, so the send cannot have completed.
    const std::string big(8 * 1024 * 1024, 'x');
    Client c1;
    ASSERT_TRUE(c1.connect(_proxy_port));
    ASSERT_TRUE(c1.send(make_request(big)));
    EXPECT_EQ(c1.recv_status(5000), 200);
    c1.close();

    // The upstream was pooled with that send still in flight. The next client
    // acquires it; the request must still be served.
    Client c2;
    ASSERT_TRUE(c2.connect(_proxy_port));
    ASSERT_TRUE(c2.send(make_request("second")));
    EXPECT_EQ(c2.recv_status(5000), 200) << "pooled upstream was released mid-send";
    c2.close();
    shutdown();
    // Assert the mechanism, not just the outcome: a test that passes because the
    // send happened to finish would prove nothing about the guard.
    EXPECT_GE(_gw->stats().upstream_unsent, 1u);
}
INSTANTIATE_TEST_SUITE_P(Backends, ProxyEarlyResponse,
                         ::testing::Values(llmbridge::IoBackend::Epoll,
                                           llmbridge::IoBackend::Uring));

TEST_F(ProxyIT, ResponseBodyIntegrity)
{
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    std::string resp = c.recv_response();
    llmbridge::net::http::Message m;
    ASSERT_EQ(llmbridge::net::http::parse_request(resp, m), llmbridge::net::http::FrameStatus::Complete);
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
    EXPECT_EQ(c.recv_status(), 502); // upstream connect fails -> 502 to the client
    EXPECT_TRUE(c.wait_closed());    // then the proxy closes the connection
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
// Extended correctness suite: the backend-agnostic gate that BOTH the epoll loop
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
        llmbridge::net::http::Message m;
        if (llmbridge::net::http::parse_request(resp, m) != llmbridge::net::http::FrameStatus::Complete) return {};
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
    if (mode == TranslateMode::Anthropic)
    {
        EXPECT_NE(upstream.find("/v1/messages"), std::string::npos);
    }
    if (mode == TranslateMode::Gemini)
    {
        EXPECT_NE(upstream.find("generateContent"), std::string::npos);
    }
    if (mode == TranslateMode::Cohere)
    {
        EXPECT_NE(upstream.find("/v2/chat"), std::string::npos);
    }
    c.close();
    shutdown();
}

INSTANTIATE_TEST_SUITE_P(Dialects, ProxyTranslateMode,
                         ::testing::Values(TranslateMode::Anthropic, TranslateMode::Gemini,
                                           TranslateMode::Cohere),
                         [](const testing::TestParamInfo<TranslateMode>& i) { return mode_name(i.param); });

// ── Auth-header passthrough (translate modes rebuild the upstream request,
//     so credentials must be explicitly mapped across the dialect boundary) ────
class ProxyAuth : public ProxyIT, public ::testing::WithParamInterface<llmbridge::IoBackend> {};
class ProxyStrip : public ProxyIT, public ::testing::WithParamInterface<llmbridge::IoBackend> {};

TEST_P(ProxyAuth, BearerTokenMapsToAnthropicApiKey)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "Authorization: Bearer sk-test-123\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find("x-api-key: sk-test-123\r\n"), std::string::npos) << up;
    // The OpenAI-style header must NOT also cross: one credential, one shape.
    EXPECT_EQ(up.find("Authorization:"), std::string::npos) << up;
    // Anthropic's required version header is pinned when the client has none.
    EXPECT_NE(up.find("anthropic-version: 2023-06-01\r\n"), std::string::npos) << up;
}

// BYOK, the shape the hosted pilot uses: Authorization carries a KOTTOS tenant
// token that a policy reads, x-api-key carries the customer's own provider key.
// x-api-key must win, or the tenant token goes to the provider as a credential:
// a guaranteed 401, and our token handed to a third party.
TEST_P(ProxyAuth, XApiKeyWinsOverBearerSoATenantTokenNeverReachesTheProvider)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "Authorization: Bearer kb_live_TENANT\r\n"
                                                 "x-api-key: sk-ant-REAL\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find("x-api-key: sk-ant-REAL\r\n"), std::string::npos) << up;
    EXPECT_EQ(up.find("kb_live_TENANT"), std::string::npos)
        << "the tenant token was forwarded to the provider: " << up;
}

// The sharp edge that comes with it. Passthrough forwards the client's bytes
// verbatim, so BOTH headers cross and the tenant token does reach the upstream.
// A tenant token is therefore only safe in a TRANSLATING mode, where the request
// is rebuilt from a whitelist. Asserted so nobody points the pilot at an
// OpenAI-compatible venue (no translation) and leaks it.
TEST_P(ProxyAuth, PassthroughForwardsEveryHeaderIncludingATenantToken)
{
    _backend.set_response(http_ok("{}"));
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "Authorization: Bearer kb_live_TENANT\r\n"
                                                 "x-api-key: sk-ant-REAL\r\n")));
    (void)c.recv_response();
    EXPECT_NE(_backend.last_request().find("kb_live_TENANT"), std::string::npos)
        << "if this ever fails, passthrough started rebuilding requests and the "
           "tenant-token guidance above can be relaxed";
}

// ── Stripping headers off the upstream request ───────────────────────────────
//
// A hosted deployment puts its OWN tenant token in Authorization. Without this,
// passthrough copies it to the provider verbatim and the translating path maps it
// onto x-api-key, so a third party ends up holding our credential.

TEST_P(ProxyStrip, PassthroughDropsTheNamedHeader)
{
    _strip_headers = {"Authorization"}; // mixed case on purpose: names are normalized
    _backend.set_response(http_ok("{}"));
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "Authorization: Bearer kb_live_TENANT\r\n"
                                                 "x-api-key: sk-ant-REAL\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_EQ(up.find("kb_live_TENANT"), std::string::npos) << up;
    EXPECT_NE(up.find("x-api-key: sk-ant-REAL"), std::string::npos) << up;
    EXPECT_NE(up.find("POST /v1/chat/completions"), std::string::npos) << "request line lost";
}

// The case the strip list exists for. With no x-api-key to win the contest, the
// tenant token WOULD become the provider credential. Stripping has to happen
// before credential mapping, not just before the copy.
TEST_P(ProxyStrip, TranslatedRequestNeverMapsAStrippedTokenOntoTheProviderKey)
{
    _strip_headers = {"authorization"};
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "Authorization: Bearer kb_live_TENANT\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_EQ(up.find("kb_live_TENANT"), std::string::npos) << up;
    EXPECT_EQ(up.find("x-api-key:"), std::string::npos)
        << "the tenant token was promoted to the provider credential: " << up;
}

// Body and framing must survive untouched: a wrong Content-Length here would
// desync the shared upstream connection for the NEXT client.
TEST_P(ProxyStrip, BodyAndFramingSurviveTheRewrite)
{
    _strip_headers = {"x-drop-me"};
    _backend.set_response(http_ok("{}"));
    start(0, true, TranslateMode::None, GetParam());
    const std::string body = "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"xyz\"}]}";
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send("POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n"
                       "X-Drop-Me: secret\r\nX-Keep-Me: kept\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_EQ(up.find("secret"), std::string::npos) << up;
    EXPECT_NE(up.find("X-Keep-Me: kept"), std::string::npos) << up;
    EXPECT_NE(up.find("Content-Length: " + std::to_string(body.size())), std::string::npos) << up;
    ASSERT_GE(up.size(), body.size());
    EXPECT_EQ(up.substr(up.size() - body.size()), body) << up;
}

// With nothing to strip, byte-forward changes exactly ONE header: Host, which must
// name the venue this request is being sent to and not the name the client used for
// us. Everything else, including headers the gateway has no opinion about, survives
// untouched.
TEST_P(ProxyStrip, EmptyListChangesOnlyTheHost)
{
    _backend.set_response(http_ok("{}"));
    start(0, true, TranslateMode::None, GetParam());
    const std::string req = openai_request_hdrs("hi", "Authorization: Bearer keep\r\n"
                                                      "X-Odd-Header: v\r\n");
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(req));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    const std::string expect_host = "Host: 127.0.0.1:" + std::to_string(_backend.port());
    EXPECT_NE(up.find(expect_host), std::string::npos)
        << "Host must name the venue, not the client's idea of us: " << up;
    EXPECT_EQ(up.find("Host: x"), std::string::npos) << "the client's Host was forwarded";
    // One Host, however many arrive, and nothing else touched.
    EXPECT_EQ(up.find("Host:"), up.rfind("Host:")) << "more than one Host went upstream";
    EXPECT_NE(up.find("Authorization: Bearer keep"), std::string::npos) << up;
    EXPECT_NE(up.find("X-Odd-Header: v"), std::string::npos) << up;
    // The body is whatever the client sent, unchanged: compare it against the tail
    // of the request that went in, never against a guess at its last bytes.
    const size_t body_at = req.find("\r\n\r\n");
    ASSERT_NE(body_at, std::string::npos);
    const std::string body = req.substr(body_at + 4);
    EXPECT_EQ(up.substr(up.size() - body.size()), body) << "the body was altered: " << up;
}

// THE REASON BYTE-FORWARD REWRITES HOST. This path sends the request to a different
// origin than the client addressed, so forwarding the client's Host names a host the
// provider was never asked about: a CDN-fronted or multi-vhost provider misroutes it
// or refuses it. The translating path has always emitted the venue's Host; this makes
// byte-forward agree. Several client Hosts still collapse to exactly one upstream.
TEST_P(ProxyStrip, ByteForwardHostNamesTheVenueNotTheClient)
{
    _backend.set_response(http_ok("{}"));
    start(0, true, TranslateMode::None, GetParam());
    // Two Host lines, the shape a confused client or a smuggling attempt produces.
    const std::string req = openai_request_hdrs("hi", "Host: someone-elses-name\r\n");
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(req));
    (void)c.recv_response();

    const std::string up = _backend.last_request();
    const std::string expect = "Host: 127.0.0.1:" + std::to_string(_backend.port());
    EXPECT_NE(up.find(expect), std::string::npos) << "Host does not name the venue: " << up;
    EXPECT_EQ(up.find("someone-elses-name"), std::string::npos)
        << "a client-supplied Host reached the provider: " << up;
    EXPECT_EQ(up.find("Host: x"), std::string::npos) << up;
    EXPECT_EQ(up.find("Host:"), up.rfind("Host:")) << "more than one Host went upstream";
    // It belongs directly after the request line, where a reader looks for it.
    EXPECT_EQ(up.find(expect), up.find("\r\n") + 2) << up;
}

// The colon appended to every strip name is what makes the match exact. Without
// it, "x-drop" eats "x-dropper", and nothing stops a name matching the request
// line either. Both are one deleted line away.
TEST_P(ProxyStrip, StrippingIsExactNotAPrefix)
{
    _strip_headers = {"x-drop"};
    _backend.set_response(http_ok("{}"));
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "X-Drop: gone\r\nX-Dropper: kept\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_EQ(up.find("gone"), std::string::npos) << up;
    EXPECT_NE(up.find("X-Dropper: kept"), std::string::npos)
        << "a longer header name was stripped by prefix: " << up;
    EXPECT_NE(up.find("POST /v1/chat/completions"), std::string::npos) << "request line lost";
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyStrip,
                         ::testing::Values(llmbridge::IoBackend::Epoll,
                                           llmbridge::IoBackend::Uring));

// ── venue base path ─────────────────────────────────────────────────────────
//
// Several providers serve an OpenAI-compatible API below the root (Groq at
// /openai, OpenRouter at /api), so a venue may carry a prefix. It is joined in
// front of whatever target the request would otherwise use, on BOTH legs: the
// client's own target when forwarding bytes, and ours when translating.

class ProxyBasePath : public ProxyIT,
                      public ::testing::WithParamInterface<llmbridge::IoBackend>
{
  protected:
    void start_with_base(const std::string& base, TranslateMode mode)
    {
        _backend.start();
        _upstreams.push_back(llmbridge::Upstream{.ip = "127.0.0.1",
                                                 .port = _backend.port(),
                                                 .translate = mode,
                                                 .base_path = base});
        start(0, false, TranslateMode::None, GetParam());
    }
};

TEST_P(ProxyBasePath, ByteForwardPrefixesTheClientsOwnTarget)
{
    _backend.set_response(http_ok("{}"));
    start_with_base("/openai", TranslateMode::None);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_EQ(up.find("POST /openai/v1/chat/completions HTTP/1.1\r\n"), 0u) << up;
}

TEST_P(ProxyBasePath, TranslatePrefixesOurOwnTarget)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start_with_base("/openai", TranslateMode::Anthropic);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "Authorization: Bearer sk-test-123\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_EQ(up.find("POST /openai/v1/messages HTTP/1.1\r\n"), 0u) << up;
}

// The regression guard: no base path must leave the target byte-identical, on
// both legs. This is the configuration every existing deployment runs.
TEST_P(ProxyBasePath, NoBasePathChangesNothing)
{
    _backend.set_response(http_ok("{}"));
    start_with_base("", TranslateMode::None);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "")));
    (void)c.recv_response();
    EXPECT_EQ(_backend.last_request().find("POST /v1/chat/completions HTTP/1.1\r\n"), 0u)
        << _backend.last_request();
}

// FAIL CLOSED. An absolute-form target ("POST http://evil/x") is a request we
// cannot prefix without deciding what it means, and deciding wrongly sends a
// caller's credential to a host the operator never listed. Refuse, and make sure
// NOTHING reached the upstream: the pool means the next request on that
// connection belongs to someone else.
TEST_P(ProxyBasePath, ANonOriginFormTargetIsRefusedAndNeverForwarded)
{
    _backend.set_response(http_ok("{}"));
    start_with_base("/openai", TranslateMode::None);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    const std::string body = R"({"model":"m","messages":[]})";
    ASSERT_TRUE(c.send("POST http://evil.example/v1/chat/completions HTTP/1.1\r\n"
                       "Host: proxy\r\nContent-Type: application/json\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body));
    const std::string resp = c.recv_response();
    EXPECT_NE(resp.find("400"), std::string::npos) << resp;
    EXPECT_TRUE(_backend.last_request().empty())
        << "a request we refused still reached the upstream: " << _backend.last_request();
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyBasePath,
                         ::testing::Values(llmbridge::IoBackend::Epoll,
                                           llmbridge::IoBackend::Uring));

// ── Content-Length disagreeing with the actual body ─────────────────────────
//
// The parser-level cases live in HttpDesync (non-numeric, chunked+CL, conflicting
// duplicates). These are the END-TO-END ones: what a CLIENT sees, and what the
// NEXT client sees, when the bytes on the wire do not match the declared length.
// The second question is the dangerous one, because a keep-alive upstream is
// shared, so one response's residue is the next customer's problem.

TEST_P(ProxyAuth, UpstreamBodyShorterThanContentLengthNeverBecomesASuccess)
{
    // The provider declares 4096 bytes, sends a handful, then closes. The client
    // must NOT receive a 200 carrying a truncated body: a half answer presented as
    // a whole one is a silent wrong result for an agent loop.
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                       "Content-Length: 4096\r\n\r\n{\"partial\":true}";
    _backend.set_response(resp);
    _backend.set_close_mid_response(true);
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "")));
    const std::string got = c.recv_response(4000);
    // Either a structured error or nothing at all. Never a 200.
    EXPECT_EQ(got.find("HTTP/1.1 200"), std::string::npos)
        << "a truncated upstream body was presented to the client as success: "
        << got.substr(0, 120);
}

TEST_P(ProxyAuth, UpstreamBodyLongerThanContentLengthDoesNotPoisonTheNextRequest)
{
    // The provider declares N and sends N PLUS trailing bytes on a keep-alive
    // connection. The extra must never survive into the pool: the next client to
    // reuse that connection would read it as the head of ITS response, which is a
    // cross-client desync and the most severe failure this codebase can have.
    const std::string body = R"({"content":[{"type":"text","text":"ok"}]})";
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: keep-alive\r\n\r\n" + body +
                       "X-Injected: yes\r\nTRAILING-GARBAGE\r\n";
    _backend.set_response(resp);
    start(0, true, TranslateMode::Anthropic, GetParam());

    Client c1;
    ASSERT_TRUE(c1.connect(_proxy_port));
    ASSERT_TRUE(c1.send(openai_request_hdrs("one", "")));
    const std::string r1 = c1.recv_response(4000);
    ASSERT_NE(r1.find("HTTP/1.1 200"), std::string::npos) << r1.substr(0, 120);

    // Second client, which is what the pooled connection gets handed to.
    Client c2;
    ASSERT_TRUE(c2.connect(_proxy_port));
    ASSERT_TRUE(c2.send(openai_request_hdrs("two", "")));
    const std::string r2 = c2.recv_response(4000);
    ASSERT_FALSE(r2.empty()) << "second client got nothing; the pooled connection "
                                "was left unusable by the first response";
    // WITHOUT THIS the test is vacuous, and it was: it passed with the pooled
    // rbuf.clear() deleted, because the second request had opened a FRESH upstream
    // and never touched the poisoned one. Assert the reuse actually happened.
    // Reuse is asserted after the join at the end: stats() belongs to the loop
    // thread, and polling it live is a data race TSan reports.
    // THE CANARY MUST CONTAIN CRLF, and the first version did not. Residue with
    // no space and no CRLF is silently ABSORBED into the next response's status
    // line, because parse_response() finds the first space anywhere in the head
    // and reads three digits after it; it never checks the line begins with
    // "HTTP/". So "TRAILING-GARBAGE...POOL" merged into "...POOLHTTP/1.1 200 OK",
    // yielded status 200, and the test passed with the pooled clear DELETED.
    // Residue carrying CRLF cannot be absorbed that way, so it reaches the header
    // parser and the message is refused, which is the difference this asserts.
    //
    // It proves the observable contract: an over-long upstream body does not stop
    // the next client on a REUSED connection from getting a correct answer.
    // upstream_reused is asserted above so the reuse really happens.
    //
    // It does NOT prove that ep/ur_release_upstream's rbuf.clear() is what
    // protects that. Deleting BOTH clears leaves this test passing and the second
    // client still receiving a 200, so some other part of the path is keeping the
    // connection sane. Which part is not yet identified. The clear is obviously
    // right and stays, but calling this test its guard would be a claim the
    // measurement does not support.
    EXPECT_NE(r2.find("HTTP/1.1 200"), std::string::npos)
        << "the second client did not get a successful response, which is what a "
           "poisoned pooled connection looks like from outside: " << r2.substr(0, 160);
    EXPECT_EQ(r2.find("TRAILING-GARBAGE"), std::string::npos)
        << "residue from the FIRST response reached the SECOND client: " << r2.substr(0, 160);

    c2.close();
    shutdown();
    EXPECT_GT(_gw->stats().upstream_reused, 0u)
        << "no upstream was reused, so the residue path was never exercised";
}

TEST_P(ProxyAuth, ClientBodyLongerThanContentLengthDoesNotSmuggleASecondRequest)
{
    // The classic smuggling shape from the other direction: the client declares N
    // and sends N plus something that looks like another request. Exactly ONE
    // request may reach the provider.
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));

    const std::string body = R"({"model":"m","messages":[{"role":"u","content":"hi"}]})";
    const std::string smuggled =
        "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nX-Smuggled: yes\r\n"
        "Content-Length: 0\r\n\r\n";
    ASSERT_TRUE(c.send("POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n"
                       "Content-Type: application/json\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body + smuggled));
    (void)c.recv_response(4000);
    EXPECT_EQ(_backend.last_request().find("X-Smuggled"), std::string::npos)
        << "a second request rode in on the first one's body";
}

TEST_P(ProxyAuth, CredentialIsScrubbedFromAPooledUpstreamBuffer)
{
    // A keep-alive upstream goes back into the pool holding the REBUILT REQUEST,
    // credential included, and idles there for up to 30 s before being handed to
    // whichever client asks next. Scrubbing it is what stops one customer's API
    // key sitting in a buffer another customer's request will reuse.
    //
    // A mutation sweep found secure_clear() could be deleted with nothing failing:
    // every existing auth test inspects what reached the UPSTREAM, and none looks
    // at what stays behind. This looks at what stays behind.
    static constexpr const char* kCanary = "sk-canary-DO-NOT-LEAVE-IN-THE-POOL";

    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs(
        "hi", std::string("Authorization: Bearer ") + kCanary + "\r\n")));
    ASSERT_FALSE(c.recv_response().empty());

    // The credential did reach the provider ...
    EXPECT_NE(_backend.last_request().find(kCanary), std::string::npos)
        << "the canary never reached the upstream; the test proves nothing";

    // pooled_upstream_count() and pooled_buffer_contains() read state owned by the
    // loop thread, so join first. Polling them live is a data race that TSan reports,
    // and a suite carrying known-benign races is one where a real report is ignored.
    // Release runs synchronously with the response already received, so no poll is
    // needed to observe the pooling.
    c.close();
    shutdown();

    // It must really have been pooled, or this asserts nothing at all.
    ASSERT_GT(_gw->pooled_upstream_count(), 0u)
        << "no upstream was pooled, so the scrub was never exercised";
    // ... and the credential must not still be sitting in the pooled buffer.
    EXPECT_FALSE(_gw->pooled_buffer_contains(kCanary))
        << "a customer credential is idling in a pooled buffer that the next "
           "client's request will reuse";
}

TEST_P(ProxyAuth, NativeApiKeyAndVersionForwardVerbatim)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs(
        "hi", "x-api-key: sk-native-456\r\nanthropic-version: 2024-10-22\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find("x-api-key: sk-native-456\r\n"), std::string::npos) << up;
    // The client's own version wins over the pinned default.
    EXPECT_NE(up.find("anthropic-version: 2024-10-22\r\n"), std::string::npos) << up;
    EXPECT_EQ(up.find("2023-06-01"), std::string::npos) << up;
}

TEST_P(ProxyAuth, NoCredentialMeansNoAuthHeaderUpstream)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("hi")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    // No credential invented; the provider's own 401 is the correct outcome.
    EXPECT_EQ(up.find("x-api-key"), std::string::npos) << up;
    EXPECT_EQ(up.find("Authorization"), std::string::npos) << up;
}

TEST_P(ProxyAuth, UnrelatedClientHeadersDoNotCross)
{
    // WHITELIST semantics: a rebuilt request must not echo arbitrary client
    // headers; that is the smuggling surface the rebuild exists to close.
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs(
        "hi", "Authorization: Bearer sk-1\r\nX-Evil: 1\r\nCookie: session=abc\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find("x-api-key: sk-1"), std::string::npos) << up;
    EXPECT_EQ(up.find("X-Evil"), std::string::npos) << up;
    EXPECT_EQ(up.find("Cookie"), std::string::npos) << up;
}

TEST_P(ProxyAuth, RebuiltRequestCarriesHostHeader)
{
    // HTTP/1.1 requires Host; mocks tolerate its absence, api.anthropic.com
    // does not. Without TlsConfig the gateway falls back to ip:port.
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("hi")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find("Host: 127.0.0.1:"), std::string::npos) << up;
}

TEST_P(ProxyAuth, BearerTokenMapsToGoogApiKeyForGemini)
{
    _backend.set_response(http_ok(provider_resp_body(TranslateMode::Gemini, "ok")));
    start(0, true, TranslateMode::Gemini, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "Authorization: Bearer AIza-test\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find("x-goog-api-key: AIza-test\r\n"), std::string::npos) << up;
    EXPECT_EQ(up.find("Authorization:"), std::string::npos) << up;
}

TEST_P(ProxyAuth, BearerForwardsVerbatimForCohere)
{
    _backend.set_response(http_ok(provider_resp_body(TranslateMode::Cohere, "ok")));
    start(0, true, TranslateMode::Cohere, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "Authorization: Bearer co-test-9\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find("Authorization: Bearer co-test-9\r\n"), std::string::npos) << up;
}

// SECURITY: a credential value containing a BARE CR (not CRLF) must not be able
// to inject a header line into the rebuilt upstream request. This matters more
// than usual here because upstream connections are POOLED AND SHARED across
// clients: smuggling on one is a cross-client request/response-splitting vector,
// not just a self-inflicted malformed request.
TEST_P(ProxyAuth, CredentialWithBareCrCannotInjectUpstreamHeader)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs(
        "hi", "Authorization: Bearer sk\rX-Smuggled: yes\r\n")));
    (void)c.recv_response();
    // Fails CLOSED: 400 to the client, and the upstream is never contacted at all.
    EXPECT_EQ(_backend.last_request().find("X-Smuggled"), std::string::npos)
        << _backend.last_request();
    EXPECT_TRUE(_backend.last_request().empty()) << "request reached upstream despite bad credential";
}

// A second Authorization header cannot smuggle past the check by hiding behind a
// clean first one (first-wins means only the first is ever emitted).
TEST_P(ProxyAuth, SecondCredentialHeaderCannotBypassValidation)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs(
        "hi", "Authorization: Bearer clean\r\nAuthorization: Bearer bad\rX-S: 1\r\n")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_EQ(up.find("X-S:"), std::string::npos) << up;
    if (!up.empty())
    {
        EXPECT_NE(up.find("x-api-key: clean"), std::string::npos) << up;
    }
}

// An oversized credential is refused instead of forwarded (bounded work, and a
// 8 KiB "key" is an attack or a bug, never a real provider key).
TEST_P(ProxyAuth, OversizedCredentialRejected)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs(
        "hi", "Authorization: Bearer " + std::string(9000, 'k') + "\r\n")));
    const int st = c.recv_status();
    EXPECT_TRUE(st == 400 || st == 0) << "status " << st;
    EXPECT_TRUE(_backend.last_request().empty());
}

// Same class, other control characters: NUL, bare LF, tabs in the middle.
TEST_P(ProxyAuth, CredentialWithControlCharsIsRejectedNotForwarded)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    std::string hdr = "Authorization: Bearer sk";
    hdr.push_back('\x01');
    hdr += "bad\r\n";
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", hdr)));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_EQ(up.find('\x01'), std::string::npos) << up;
    EXPECT_TRUE(up.empty()) << "control-char credential reached upstream";
}

// ── Credential-leak audit ────────────────────────────────────────────────────
// The question these answer: can one client's key ever reach the provider on a
// DIFFERENT client's request? Upstream connections are pooled and shared, so this
// is not obvious by inspection, so assert it.

TEST_P(ProxyAuth, CredentialDoesNotLeakToAnotherClientOnPooledConnection)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());

    // Client A authenticates. Its upstream connection goes back to the pool.
    {
        Client a;
        ASSERT_TRUE(a.connect(_proxy_port));
        ASSERT_TRUE(a.send(openai_request_hdrs("one", "Authorization: Bearer sk-CLIENT-A\r\n")));
        ASSERT_FALSE(a.recv_response().empty());
    }
    // Client B sends NO credential and very likely reuses A's pooled connection.
    {
        Client b;
        ASSERT_TRUE(b.connect(_proxy_port));
        ASSERT_TRUE(b.send(openai_request("two")));
        ASSERT_FALSE(b.recv_response().empty());
    }
    shutdown();

    // A's key must appear EXACTLY ONCE across everything the provider ever saw
    // on A's own request, never re-sent on B's.
    const std::string all = _backend.all_requests();
    size_t occurrences = 0;
    for (size_t p = all.find("sk-CLIENT-A"); p != std::string::npos;
         p = all.find("sk-CLIENT-A", p + 1))
        ++occurrences;
    EXPECT_EQ(occurrences, 1u) << "credential re-sent on another client's request:\n" << all;
    EXPECT_GE(_gw->stats().upstream_reused, 1u) << "pool was not exercised; test proves little";
}

TEST_P(ProxyAuth, DifferentClientsCredentialsNeverCross)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    for (const char* key : {"sk-AAA", "sk-BBB", "sk-CCC"})
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(openai_request_hdrs(
            "hi", std::string("Authorization: Bearer ") + key + "\r\n")));
        ASSERT_FALSE(c.recv_response().empty());
    }
    shutdown();
    const std::string all = _backend.all_requests();
    // Each request must carry exactly one key, and each key exactly once.
    for (const char* key : {"sk-AAA", "sk-BBB", "sk-CCC"})
    {
        size_t n = 0;
        for (size_t p = all.find(key); p != std::string::npos; p = all.find(key, p + 1)) ++n;
        EXPECT_EQ(n, 1u) << key << " appeared " << n << " times:\n" << all;
    }
}

TEST_P(ProxyAuth, ClientCredentialNeverReturnedToTheClient)
{
    // A credential must not come back in any response body/headers, not even in an error
    // path that echoed the request would leak it into client-side logs.
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request_hdrs("hi", "Authorization: Bearer sk-ECHO-ME\r\n")));
    const std::string resp = c.recv_response();
    EXPECT_EQ(resp.find("sk-ECHO-ME"), std::string::npos) << resp;
}

TEST_P(ProxyAuth, MalformedRequestErrorDoesNotEchoCredential)
{
    // Same, on the 400 path: the most likely place for a "helpful" echo.
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    // Valid framing, body that fails translation -> 400 built by the gateway.
    const std::string body = "{not json";
    const std::string req = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n"
                            "Authorization: Bearer sk-SECRET-400\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    ASSERT_TRUE(c.send(req));
    const std::string resp = c.recv_response();
    EXPECT_EQ(resp.find("sk-SECRET-400"), std::string::npos) << resp;
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyAuth,
                         ::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                         [](const testing::TestParamInfo<llmbridge::IoBackend>& i) {
                             return i.param == llmbridge::IoBackend::Epoll ? "epoll" : "uring";
                         });

// ── Chunked non-streaming responses ──────────────────────────────────────────
// REGRESSION: real providers return non-streaming completions with
// Transfer-Encoding: chunked and no Content-Length (Anthropic does over HTTP/1.1;
// it is invisible over HTTP/2, which has native framing). Every mock in this file
// used to reply with Content-Length, so the whole-body path's inability to read a
// chunked response survived 767 tests and only surfaced against the live API as a
// 502. These tests make the mocks behave like the real thing.
class ProxyChunkedResp : public ProxyIT,
                         public ::testing::WithParamInterface<std::tuple<llmbridge::IoBackend, int>>
{
};

TEST_P(ProxyChunkedResp, TranslatedChunkedResponseRoundTrips)
{
    const auto [backend, nchunks] = GetParam();
    _backend.set_response(http_ok(anthropic_resp_body("chunked-pong")));
    _backend.set_chunked_response(nchunks);
    start(0, true, TranslateMode::Anthropic, backend);

    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("hi")));
    const std::string r = c.recv_response();
    ASSERT_FALSE(r.empty()) << "no response for a chunked upstream reply";
    EXPECT_EQ(Client::status_of(r), 200) << r;
    EXPECT_NE(body_of(r).find("\"content\":\"chunked-pong\""), std::string::npos) << body_of(r);
}

TEST_P(ProxyChunkedResp, PassthroughChunkedResponseReframedWithContentLength)
{
    const auto [backend, nchunks] = GetParam();
    _backend.set_chunked_response(nchunks);
    start(0, true, TranslateMode::None, backend);

    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string r = c.recv_response();
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(Client::status_of(r), 200) << r;
    EXPECT_EQ(body_of(r), kRespBody) << r;
    // The client must get a self-framed message, not our upstream's chunking.
    EXPECT_EQ(r.find("Transfer-Encoding"), std::string::npos) << r;
    EXPECT_NE(r.find("Content-Length:"), std::string::npos) << r;
}

TEST_P(ProxyChunkedResp, PooledConnectionSurvivesAChunkedResponse)
{
    // The end of a chunked message is found by decoding, not by Content-Length. If
    // total_len were wrong, leftover bytes would poison the pooled connection and
    // the SECOND request would mis-frame, so two requests is the real test.
    const auto [backend, nchunks] = GetParam();
    _backend.set_response(http_ok(anthropic_resp_body("again")));
    _backend.set_chunked_response(nchunks);
    start(0, true, TranslateMode::Anthropic, backend);

    for (int i = 0; i < 3; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(openai_request("hi")));
        const std::string r = c.recv_response();
        ASSERT_FALSE(r.empty()) << "request " << i;
        EXPECT_EQ(Client::status_of(r), 200) << "request " << i;
        EXPECT_NE(body_of(r).find("again"), std::string::npos) << "request " << i;
    }
    shutdown();
    EXPECT_GE(_gw->stats().upstream_reused, 1u) << "pool never exercised; test proves little";
    EXPECT_EQ(_gw->stats().errors, 0u);
}

INSTANTIATE_TEST_SUITE_P(
    Backends, ProxyChunkedResp,
    ::testing::Combine(::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                       ::testing::Values(1, 3, 17)), // whole-body, split, many tiny chunks
    [](const testing::TestParamInfo<std::tuple<llmbridge::IoBackend, int>>& i) {
        return std::string(std::get<0>(i.param) == llmbridge::IoBackend::Epoll ? "epoll" : "uring") +
               "_" + std::to_string(std::get<1>(i.param)) + "chunks";
    });

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
    // Byte-exact except the rewritten Host, which is one short line: the body is what
    // this case is about, and it must arrive whole at every size.
    const std::string up = _backend.last_request();
    EXPECT_EQ(up.substr(up.size() - big.size()), big) << "the body did not arrive intact";
    EXPECT_NE(up.find("Host: 127.0.0.1:"), std::string::npos) << "Host was not rewritten";
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
        // Valid framing, invalid Content-Length -> net::http::parse Error.
        ASSERT_TRUE(bad.send("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: notanumber\r\n\r\n"));
        EXPECT_EQ(bad.recv_status(), 400); // malformed framing -> 400 Bad Request
        EXPECT_TRUE(bad.wait_closed());
        bad.close();
    }
    // A fresh client must still be served: the loop survived the bad input.
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
        EXPECT_EQ(bad.recv_status(), 400); // unparseable body -> 400 Bad Request
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

TEST_F(ProxyIT, UpstreamClosesMidResponseAbortsClient)
{
    _backend.set_close_mid_response(true);
    start();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request("hi")));
    // Incomplete upstream response -> the proxy replies 502 (was: bare close).
    EXPECT_EQ(c.recv_status(), 502);
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
// io_uring (Phase 1). Each test runs under both; if io_uring is unavailable the
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
    EXPECT_EQ(body_of(c.recv_response()), big); // large response intact
    // Large request intact: byte-forward rewrites Host and nothing else, so the
    // comparison is against the body sent, not the whole framed request.
    const std::string sent_body(64 * 1024, 'Q');
    EXPECT_EQ(_backend.last_request().substr(_backend.last_request().size() - sent_body.size()),
              sent_body);
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
        EXPECT_EQ(bad.recv_status(), 400); // malformed framing -> 400
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
        EXPECT_EQ(bad.recv_status(), 400); // unparseable body -> 400
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
    EXPECT_EQ(c.recv_status(), 502); // incomplete upstream response -> 502
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
    // Hold the upstream so the request is in flight when we tear down, which exercises
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
    EXPECT_EQ(c.recv_status(), 502); // upstream connect fails -> 502 to the client
    EXPECT_TRUE(c.wait_closed());
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
    const std::string sent_body(512 * 1024, 'Q');
    const std::string req = make_request(sent_body);
    ASSERT_TRUE(c.send(req));
    EXPECT_EQ(body_of(c.recv_response()), big);
    // Body intact across many recv/send operations; Host is the one rewritten header.
    EXPECT_EQ(_backend.last_request().substr(_backend.last_request().size() - sent_body.size()),
              sent_body);
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
// connection. epoll recovers via pool eviction; io_uring via the retry path
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
    // if the timing races). Either recovery is correct; we only require every
    // request was served. The pipelined variant below forces the retry path itself.
}

// Pipelined variant: two requests on ONE keep-alive connection. The 2nd is
// forwarded by reusing the pooled (already-dropped) upstream INLINE, before the
// event loop can evict it, so BOTH backends are forced through the retry path.
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
    // Recovery mechanism: epoll only processes the dead conn's EOF at epoll_wait
    // after the inline reuse, so it deterministically RETRIES. io_uring's always-
    // armed recv may instead evict the dead conn first (a completion-order race), so
    // we assert the retry path only where it's deterministic.
    if (GetParam() == llmbridge::IoBackend::Epoll)
    {
            EXPECT_GT(_gw->stats().upstream_retries, 0u) << "epoll inline pipelined reuse must retry";
    }
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyBackend,
                         ::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                         [](const testing::TestParamInfo<llmbridge::IoBackend>& i) { return be_name(i.param); });

// ── Bug-2 regression (deterministic, no sanitizer needed): every Connection the
// gateway allocates must be freed. Tear down with 50 requests in flight (acquired
// upstreams reachable only via peer, exactly what leaked) and assert the live
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

// ════════════════════════════════════════════════════════════════════════════
// Streaming (SSE), end-to-end on the epoll backend (Phase B). A mock upstream
// returns a chunked Anthropic event stream; the gateway must translate it into
// an OpenAI chat.completion.chunk stream and deliver it to the client, surviving
// small chunks and byte-dribbled (partial-read) delivery.
// (io_uring streaming is a follow-up; these run explicitly on IoBackend::Epoll.)
// ════════════════════════════════════════════════════════════════════════════
namespace
{
    // A realistic Anthropic text stream.
    std::string anthropic_sse_events()
    {
        return
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":"
            "{\"id\":\"msg_1\",\"model\":\"claude-3-5-sonnet-20241022\"}}\n\n"
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
            "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
            "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n"
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
            "\"delta\":{\"type\":\"text_delta\",\"text\":\", world\"}}\n\n"
            "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
            "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
            "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    }

    std::string sse_chunk_encode(std::string_view p, size_t chunk)
    {
        std::string s;
        size_t i = 0;
        while (i < p.size())
        {
            const size_t n = std::min(chunk, p.size() - i);
            char hex[32];
            std::snprintf(hex, sizeof hex, "%zx", n);
            s += hex;
            s += "\r\n";
            s.append(p.substr(i, n));
            s += "\r\n";
            i += n;
        }
        s += "0\r\n\r\n";
        return s;
    }

    // Full chunked SSE HTTP response, with the event body split into `chunk`-byte
    // transfer-encoding chunks.
    std::string sse_chunked_response(size_t chunk)
    {
        return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
               "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n" +
               sse_chunk_encode(anthropic_sse_events(), chunk);
    }

    // An Anthropic event stream that CALLS TOOLS: a text block first (so the
    // Anthropic block index and the OpenAI tool ordinal diverge), then two tool
    // blocks with fragmented arguments.
    std::string anthropic_sse_tool_events()
    {
        return
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":"
            "{\"id\":\"msg_t\",\"model\":\"claude\",\"usage\":{\"input_tokens\":9}}}\n\n"
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
            "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
            "\"delta\":{\"type\":\"text_delta\",\"text\":\"Checking.\"}}\n\n"
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,"
            "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_A\",\"name\":\"get_weather\","
            "\"input\":{}}}\n\n"
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,"
            "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"city\\\":\"}}\n\n"
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,"
            "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"Paris\\\"}\"}}\n\n"
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":2,"
            "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_B\",\"name\":\"get_time\","
            "\"input\":{}}}\n\n"
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":2,"
            "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n"
            "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":"
            "{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":14}}\n\n"
            "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    }

    std::string sse_tool_response(size_t chunk)
    {
        return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
               "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n" +
               sse_chunk_encode(anthropic_sse_tool_events(), chunk);
    }

    std::string openai_stream_request(const std::string& content)
    {
        return make_request("{\"model\":\"gpt-4o\",\"stream\":true,\"messages\":[{\"role\":\"user\","
                            "\"content\":\"" + content + "\"}]}");
    }

    // Reassemble the client-facing OpenAI SSE stream back into a whole message.
    struct Streamed { std::string content, finish; bool done = false, sse_headers = false, role = false; };
    Streamed parse_streamed(const std::string& raw)
    {
        Streamed s;
        const size_t hb = raw.find("\r\n\r\n");
        s.sse_headers = raw.substr(0, hb == std::string::npos ? raw.size() : hb).find("text/event-stream") !=
                        std::string::npos;
        std::string body = hb == std::string::npos ? raw : raw.substr(hb + 4);
        size_t i = 0;
        while (i < body.size())
        {
            const size_t nl = body.find('\n', i);
            std::string line = body.substr(i, (nl == std::string::npos ? body.size() : nl) - i);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            i = (nl == std::string::npos ? body.size() : nl + 1);
            if (line.rfind("data: ", 0) != 0) continue;
            const std::string pay = line.substr(6);
            if (pay == "[DONE]") { s.done = true; continue; }
            bool ok = false;
            llmbridge::provider::json::Value v = llmbridge::provider::json::parse(pay, ok);
            if (!ok) continue;
            const llmbridge::provider::json::Value* ch = v.find("choices");
            if (!ch || !ch->is_array() || ch->arr.empty()) continue;
            const llmbridge::provider::json::Value& c0 = ch->arr[0];
            if (const llmbridge::provider::json::Value* d = c0.find("delta"))
            {
                if (!d->str_or("role").empty()) s.role = true;
                s.content += std::string(d->str_or("content"));
            }
            if (const std::string_view fr = c0.str_or("finish_reason"); !fr.empty()) s.finish.assign(fr);
        }
        return s;
    }
} // namespace

// The streaming correctness tests run on BOTH backends (epoll + io_uring). With
// io_uring unavailable the Gateway falls back to epoll, so this is always safe.
class ProxyStream : public ProxyIT, public ::testing::WithParamInterface<llmbridge::IoBackend> {};

TEST_P(ProxyStream, TranslatesAnthropicSseToOpenAiChunks)
{
    _backend.set_response(sse_chunked_response(4096)); // whole event body in one chunk
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));

    const Streamed s = parse_streamed(c.recv_all());
    EXPECT_TRUE(s.sse_headers);            // client got text/event-stream
    EXPECT_TRUE(s.role);                   // first chunk carried the assistant role
    EXPECT_EQ(s.content, "Hello, world");  // deltas reassemble to the full message
    EXPECT_EQ(s.finish, "stop");           // end_turn -> stop
    EXPECT_TRUE(s.done);                   // terminal [DONE]

    // End-to-end wiring: the upstream request carried stream:true to /v1/messages.
    EXPECT_NE(_backend.last_request().find("\"stream\":true"), std::string::npos);
    EXPECT_NE(_backend.last_request().find("/v1/messages"), std::string::npos);

    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
    EXPECT_EQ(_gw->stats().requests, 1u);
}

TEST_P(ProxyStream, SurvivesTinyUpstreamChunks)
{
    _backend.set_response(sse_chunked_response(5)); // 5-byte chunks: many decode/translate boundaries
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const Streamed s = parse_streamed(c.recv_all());
    EXPECT_EQ(s.content, "Hello, world");
    EXPECT_TRUE(s.done);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
}

TEST_P(ProxyStream, SurvivesTrickledUpstream)
{
    _backend.set_response(sse_chunked_response(9));
    _backend.set_trickle(2); // dribble the whole chunked response 2 bytes at a time
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const Streamed s = parse_streamed(c.recv_all(5000));
    EXPECT_EQ(s.content, "Hello, world"); // incremental pump across TCP read boundaries
    EXPECT_TRUE(s.done);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
}

// Non-chunked (close-delimited) SSE upstream: some providers/proxies stream
// without transfer-encoding and just close. The pump's non-chunked branch.
TEST_P(ProxyStream, NonChunkedCloseDelimitedUpstream)
{
    _backend.set_response("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Connection: close\r\n\r\n" + anthropic_sse_events());
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const Streamed s = parse_streamed(c.recv_all());
    EXPECT_EQ(s.content, "Hello, world");
    EXPECT_TRUE(s.done); // EOF finalizes the stream
    c.close();
    shutdown();
}

// Upstream truncates mid-stream (no message_stop, connection just drops): the
// client still gets a well-formed terminal chunk + [DONE] so its SSE parser ends.
TEST_P(ProxyStream, TruncatedUpstreamStillTerminatesClientStream)
{
    const std::string ev = anthropic_sse_events();
    const std::string cut = ev.substr(0, ev.find("event: message_delta")); // drop the tail
    _backend.set_response("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Connection: close\r\n\r\n" + cut);
    _backend.set_close_after_first(true); // the upstream really drops the connection
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const Streamed s = parse_streamed(c.recv_all());
    EXPECT_EQ(s.content, "Hello, world"); // everything received is delivered
    EXPECT_TRUE(s.done);                  // finish() at EOF closes the stream cleanly
    c.close();
    shutdown();
}

// Corrupt chunked framing mid-stream must NOT fabricate a clean finish: the
// client sees the partial content and an aborted stream (no [DONE]).
TEST_P(ProxyStream, CorruptChunkFramingAbortsWithoutDone)
{
    const std::string ev = anthropic_sse_events();
    const size_t half = ev.size() / 2;
    std::string wire = sse_chunk_encode(ev.substr(0, half), 64);
    wire.erase(wire.size() - 5);   // drop the 0-terminator
    wire += "ZZZZ\r\ngarbage\r\n"; // invalid chunk-size line -> decoder error
    _backend.set_response("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n" + wire);
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const Streamed s = parse_streamed(c.recv_all());
    EXPECT_FALSE(s.done) << "corrupt framing must not emit a fabricated [DONE]";
    c.close();
    shutdown();
    EXPECT_GE(_gw->stats().errors, 1u);   // counted as an error...
    EXPECT_EQ(_gw->stats().requests, 0u); // ...not as a served request
}

// A streaming REQUEST whose upstream answers with a plain JSON completion (no
// event-stream): the head-peek must fall through to the whole-body path.
TEST_P(ProxyStream, StreamRequestWithNonStreamingUpstreamFallsBack)
{
    _backend.set_response(http_ok(anthropic_resp_body("pong")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const std::string body = body_of(c.recv_response());
    EXPECT_NE(body.find("\"content\":\"pong\""), std::string::npos) << body;
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
}

// Client disconnects mid-stream: the gateway must tear the pair down and survive.
TEST_P(ProxyStream, ClientDisconnectsMidStreamSurvives)
{
    _backend.set_response(sse_chunked_response(5));
    _backend.set_trickle(2); // slow: the client leaves while the stream is live
    start(0, true, TranslateMode::Anthropic, GetParam());
    for (int i = 0; i < 5; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port)) << i;
        ASSERT_TRUE(c.send(openai_stream_request("hi")));
        c.close(); // abandon mid-stream
    }
    // The loop survived: a normal (non-streaming) request still works.
    _backend.set_trickle(0);
    _backend.set_response(http_ok(anthropic_resp_body("alive")));
    Client good;
    ASSERT_TRUE(good.connect(_proxy_port));
    ASSERT_TRUE(good.send(openai_request("ping")));
    EXPECT_NE(body_of(good.recv_response()).find("alive"), std::string::npos);
    good.close();
    shutdown();
}

// ── Upstream model/provider errors are relayed, not masked ─────────────────
// A provider failure (rate limit, overloaded GPU, context length, auth) must
// reach the client with the upstream's OWN status code and message.
class ProxyUpstreamError
    : public ProxyIT,
      public ::testing::WithParamInterface<std::tuple<llmbridge::IoBackend, int>>
{
};

TEST_P(ProxyUpstreamError, RelaysStatusAndMessage)
{
    const auto [backend, status] = GetParam();
    const std::string err =
        R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded, retry"}})";
    _backend.set_response("HTTP/1.1 " + std::to_string(status) + " Err\r\n"
                          "Content-Type: application/json\r\nConnection: keep-alive\r\n"
                          "Content-Length: " + std::to_string(err.size()) + "\r\n\r\n" + err);
    start(0, true, TranslateMode::Anthropic, backend);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("hi")));
    const std::string resp = c.recv_response();
    EXPECT_EQ(c.status_of(resp), status) << "upstream status must be relayed, not masked as 502";
    const std::string body = body_of(resp);
    EXPECT_NE(body.find("overloaded_error"), std::string::npos) << body;   // real type
    EXPECT_NE(body.find("Overloaded, retry"), std::string::npos) << body;  // real message
    EXPECT_NE(body.find("\"error\""), std::string::npos);                  // OpenAI envelope
    c.close();
    shutdown();
}

// Same, but the provider sends the error as an event-stream (some do): it must be
// relayed as an error, never laundered into a successful 200 stream.
TEST_P(ProxyUpstreamError, EventStreamErrorIsNotLaunderedTo200)
{
    const auto [backend, status] = GetParam();
    const std::string err = R"({"type":"error","error":{"type":"api_error","message":"boom"}})";
    _backend.set_response("HTTP/1.1 " + std::to_string(status) + " Err\r\n"
                          "Content-Type: text/event-stream\r\nConnection: keep-alive\r\n"
                          "Content-Length: " + std::to_string(err.size()) + "\r\n\r\n" + err);
    start(0, true, TranslateMode::Anthropic, backend);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const std::string resp = c.recv_response();
    EXPECT_NE(c.status_of(resp), 200) << "an error response must never become a 200 stream";
    EXPECT_EQ(c.status_of(resp), status);
    c.close();
    shutdown();
}

INSTANTIATE_TEST_SUITE_P(
    Codes, ProxyUpstreamError,
    ::testing::Combine(::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                       ::testing::Values(400, 401, 429, 500, 529)),
    [](const testing::TestParamInfo<std::tuple<llmbridge::IoBackend, int>>& i) {
        return std::string(be_name(std::get<0>(i.param))) + "_" + std::to_string(std::get<1>(i.param));
    });

// stream_options.include_usage: the client must receive a final usage chunk
// carrying the provider's real token counts, just before [DONE].
TEST_P(ProxyStream, IncludeUsageEmitsFinalUsageChunk)
{
    const std::string ev =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"model\":\"claude-3-5-sonnet\","
        "\"usage\":{\"input_tokens\":13,\"output_tokens\":1}}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
        "{\"type\":\"text_delta\",\"text\":\"Hello, world\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
        "\"usage\":{\"output_tokens\":9}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    _backend.set_response("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n" +
                          sse_chunk_encode(ev, 4096));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request(
        "{\"model\":\"gpt-4o\",\"stream\":true,\"stream_options\":{\"include_usage\":true},"
        "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}")));

    const std::string raw = c.recv_all();
    const Streamed s = parse_streamed(raw);
    EXPECT_EQ(s.content, "Hello, world");
    EXPECT_TRUE(s.done);
    // The real provider counts reached the client.
    EXPECT_NE(raw.find("\"prompt_tokens\":13"), std::string::npos) << raw.substr(0, 400);
    EXPECT_NE(raw.find("\"completion_tokens\":9"), std::string::npos);
    EXPECT_NE(raw.find("\"total_tokens\":22"), std::string::npos);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
}

TEST_P(ProxyStream, NoUsageChunkWhenNotRequested)
{
    _backend.set_response(sse_chunked_response(4096));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi"))); // no stream_options
    const std::string raw = c.recv_all();
    EXPECT_EQ(raw.find("prompt_tokens"), std::string::npos) << "usage must be opt-in";
    EXPECT_TRUE(parse_streamed(raw).done);
    c.close();
    shutdown();
}

// ── Upstream idle timeouts ────────────────────────────────────────────────
// A stalled provider must not pin the client + two fds forever.

TEST_P(ProxyStream, StalledUpstreamTimesOutWith504)
{
    _backend.set_stall(1); // read the request, never answer
    start(0, true, TranslateMode::Anthropic, GetParam(), /*idle=*/300'000'000LL); // 300 ms
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("hi")));
    EXPECT_EQ(c.recv_status(3000), 504) << "stalled upstream must time out, not hang";
    c.close();
    shutdown();
    EXPECT_GE(_gw->stats().upstream_timeouts, 1u);
}

TEST_P(ProxyStream, StalledMidStreamTimesOutAndTruncates)
{
    _backend.set_response(sse_chunked_response(64));
    _backend.set_stall(2); // send half the stream, then hang
    start(0, true, TranslateMode::Anthropic, GetParam(), /*idle=*/300'000'000LL);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const Streamed s = parse_streamed(c.recv_all(3000)); // returns when the gateway closes
    EXPECT_FALSE(s.done) << "a timed-out stream must not emit a fabricated [DONE]";
    c.close();
    shutdown();
    EXPECT_GE(_gw->stats().upstream_timeouts, 1u);
    EXPECT_EQ(_gw->stats().requests, 0u); // aborted, not served
}

TEST_P(ProxyStream, HealthyStreamIsNotTimedOut)
{
    // Guard against an over-eager sweep killing live streams: a trickled (but
    // progressing) upstream must complete even with a short idle timeout.
    _backend.set_response(sse_chunked_response(16));
    _backend.set_trickle(4); // slow but continuously progressing
    start(0, true, TranslateMode::Anthropic, GetParam(), /*idle=*/1'000'000'000LL); // 1 s
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const Streamed s = parse_streamed(c.recv_all(8000));
    EXPECT_EQ(s.content, "Hello, world");
    EXPECT_TRUE(s.done);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().upstream_timeouts, 0u);
}

// ── Backpressure: a slow client must pause upstream reads (epoll) ─────────
// Deterministic: a tiny client receive window + a multi-MB stream + a client that
// stalls before reading forces the gateway's writes to block, which must engage
// the pause path, and every byte must still arrive once the client drains.
TEST_P(ProxyStream, SlowClientEngagesBackpressureAndLosesNothing)
{
    // ~1.6 MB of SSE: many deltas, each large enough to fill socket buffers fast.
    const std::string filler(400, 'x');
    std::string ev =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"model\":\"c\"}}\n\n";
    const int kDeltas = 4000;
    for (int i = 0; i < kDeltas; ++i)
        ev += "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
              "{\"type\":\"text_delta\",\"text\":\"" + filler + "\"}}\n\n";
    ev += "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
          "data: {\"type\":\"message_stop\"}\n\n";
    _backend.set_response("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n" +
                          sse_chunk_encode(ev, 16384));
    start(0, true, TranslateMode::Anthropic, GetParam(), /*idle=*/0); // no timeout: client is slow on purpose

    Client c;
    ASSERT_TRUE(c.connect(_proxy_port, /*rcvbuf=*/4096)); // tiny window
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    // Don't read for a while: the gateway's client writes block and back up.
    timespec ts{0, 400'000'000};
    nanosleep(&ts, nullptr);

    const Streamed s = parse_streamed(c.recv_all(15000)); // now drain everything
    EXPECT_EQ(s.content.size(), filler.size() * kDeltas) << "backpressure must not lose bytes";
    EXPECT_TRUE(s.done);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
    // epoll implements backpressure by pausing upstream reads; io_uring instead
    // bounds the buffer (kUrStreamBufCap), so only assert the counter on epoll.
    if (GetParam() == llmbridge::IoBackend::Epoll)
    {
            EXPECT_GT(_gw->stats().stream_pauses, 0u) << "slow client should have paused upstream reads";
    }
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyStream,
                         ::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                         [](const testing::TestParamInfo<llmbridge::IoBackend>& i) { return be_name(i.param); });

// ════════════════════════════════════════════════════════════════════════════
// Upstream connection REUSE across streaming requests.
//
// A streaming request used to close its upstream unconditionally at stream end,
// so every stream paid a fresh connect. Measured at 4096 concurrent streams that
// was 706 connects/sec and the single largest component of time-to-first-token
// on BOTH backends. The upstream is now returned to the keep-alive pool when the
// framing proves that is safe.
//
// "Safe" is conjunctive and each clause has its own test below, because the
// failure mode of getting this wrong is silent corruption of the NEXT request's
// response instead of a visible error.
// ════════════════════════════════════════════════════════════════════════════
class ProxyStreamPool : public ProxyIT, public ::testing::WithParamInterface<llmbridge::IoBackend> {};

TEST_P(ProxyStreamPool, ReusesUpstreamAcrossSequentialStreams)
{
    _backend.set_response(sse_chunked_response(4096)); // chunked + Connection: keep-alive
    start(0, true, TranslateMode::Anthropic, GetParam());

    // Three sequential streams, each on its own CLIENT connection: only the
    // upstream side should be reused.
    for (int i = 0; i < 3; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port)) << "stream " << i;
        ASSERT_TRUE(c.send(openai_stream_request("hi"))) << "stream " << i;
        const Streamed s = parse_streamed(c.recv_all());
        EXPECT_EQ(s.content, "Hello, world") << "stream " << i;
        EXPECT_TRUE(s.done) << "stream " << i;
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().errors, 0u);
    EXPECT_EQ(_gw->stats().requests, 3u);
    // The point: one connect, two reuses, not three connects.
    EXPECT_EQ(_gw->stats().upstream_conns_opened, 1u) << "each stream re-connected instead of reusing";
    EXPECT_EQ(_gw->stats().upstream_reused, 2u);
}

TEST_P(ProxyStreamPool, ReusedUpstreamCarriesNoStreamStateIntoTheNextRequest)
{
    // The dangerous bug this guards: a pooled connection resurrected with a
    // half-finished SSE translator or undrained chunk decoder would splice one
    // stream's bytes into the next. Distinct payloads make that visible.
    _backend.set_response(sse_chunked_response(5)); // tiny chunks: many decoder boundaries
    start(0, true, TranslateMode::Anthropic, GetParam());

    std::string first, second;
    for (std::string* out : {&first, &second})
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(openai_stream_request("hi")));
        const Streamed s = parse_streamed(c.recv_all());
        *out = s.content;
        EXPECT_TRUE(s.done);
        EXPECT_TRUE(s.role) << "each response must start its own assistant role delta";
        c.close();
    }
    shutdown();
    EXPECT_EQ(first, "Hello, world");
    EXPECT_EQ(second, "Hello, world") << "second stream inherited state from the pooled connection";
    EXPECT_EQ(_gw->stats().errors, 0u);
    EXPECT_EQ(_gw->stats().upstream_reused, 1u);
}

TEST_P(ProxyStreamPool, DoesNotPoolWhenUpstreamSaysConnectionClose)
{
    // Connection: close means the provider is about to hang up; pooling it would
    // only buy a stale-connection retry on the next request.
    std::string resp = sse_chunked_response(4096);
    const std::string ka = "Connection: keep-alive\r\n";
    const size_t at = resp.find(ka);
    ASSERT_NE(at, std::string::npos);
    resp.replace(at, ka.size(), "Connection: close\r\n");

    _backend.set_response(resp);
    start(0, true, TranslateMode::Anthropic, GetParam());
    for (int i = 0; i < 2; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(openai_stream_request("hi")));
        const Streamed s = parse_streamed(c.recv_all());
        EXPECT_EQ(s.content, "Hello, world") << "stream " << i;
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().upstream_reused, 0u) << "pooled a connection the provider said it would close";
    EXPECT_EQ(_gw->stats().upstream_conns_opened, 2u);
}

TEST_P(ProxyStreamPool, DoesNotPoolACloseDelimitedStream)
{
    // No Transfer-Encoding: chunked => the body ends only at EOF, so "response
    // finished" and "connection died" are indistinguishable. Never reuse.
    _backend.set_response(
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n\r\n" + anthropic_sse_events());
    _backend.set_close_after_first(true); // close-delimited: EOF is what ends it
    start(0, true, TranslateMode::Anthropic, GetParam());

    // TWO requests: with only one, upstream_reused is trivially 0 and the test
    // proves nothing. The second request is what would consume a wrongly-pooled
    // connection.
    for (int i = 0; i < 2; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(openai_stream_request("hi")));
        const Streamed s = parse_streamed(c.recv_all());
        EXPECT_EQ(s.content, "Hello, world") << "stream " << i;
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().upstream_reused, 0u) << "pooled a close-delimited stream";
}

TEST_P(ProxyStreamPool, DoesNotPoolAnAbortedStream)
{
    // Upstream vanishes mid-body: framing is untrustworthy, so the conn must be
    // dropped instead of handed to the next request.
    _backend.set_response(sse_chunked_response(4096));
    _backend.set_close_mid_response(true);
    start(0, true, TranslateMode::Anthropic, GetParam());

    // Two requests, for the same reason as the close-delimited case: a single
    // request can never observe a bad pooling decision.
    for (int i = 0; i < 2; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(openai_stream_request("hi")));
        (void)c.recv_all(); // truncated stream; content is not the assertion here
        c.close();
    }
    shutdown();
    EXPECT_EQ(_gw->stats().upstream_reused, 0u) << "pooled a connection whose stream was aborted";
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyStreamPool,
                         ::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                         [](const testing::TestParamInfo<llmbridge::IoBackend>& i) { return be_name(i.param); });

// ════════════════════════════════════════════════════════════════════════════
// Provided-buffer exhaustion, io_uring only.
//
// The io_uring backend feeds its multishot recvs from a fixed pool of provided
// buffers. When that pool is momentarily empty the kernel may end a multishot recv
// with -ENOBUFS *without* consuming a buffer. That is a transient resource
// shortage, not a connection error, and the only correct response is to re-arm.
// Get it wrong and the failure is silent and ugly: the client pair is aborted, or
// mid-stream the client is told the stream ended early: truncated output, not an
// error the caller can see.
//
// Why the pool is shrunk here instead of driven by load: at the shipped size the
// branch is not reachable in practice. Measured on this kernel at 8192 concurrent
// streams (16384 armed recvs against a 4096-buffer pool), `uring_enobufs` stayed
// at 0; under pool pressure the kernel instead ends the multishot with res > 0
// and F_MORE clear, which the ordinary re-arm already covers. So a load-driven
// test would prove nothing and pass whether or not the recovery works. Shrinking
// the pool to a single buffer forces the real condition, and the assertion on the
// counter is what proves the test is actually exercising it.
// ════════════════════════════════════════════════════════════════════════════
TEST_F(ProxyIT, UringSurvivesProvidedBufferExhaustion)
{
    constexpr int kClients = 8;
    // Small upstream chunks => many separate arrivals => many buffer acquisitions,
    // across 8 concurrent streams sharing a pool of exactly one buffer.
    _backend.set_response(sse_chunked_response(5));
    start(0, true, TranslateMode::Anthropic, llmbridge::IoBackend::Uring,
          Gateway::kDefaultUpstreamIdleNs, /*uring_buf_count=*/1);

    std::vector<std::unique_ptr<Client>> cs;
    for (int i = 0; i < kClients; ++i)
    {
        auto c = std::make_unique<Client>();
        ASSERT_TRUE(c->connect(_proxy_port)) << "client " << i;
        ASSERT_TRUE(c->send(openai_stream_request("hi"))) << "client " << i;
        cs.push_back(std::move(c));
    }

    // Every stream must arrive complete: buffer starvation may delay bytes, but it
    // must never truncate them or fabricate an early end.
    for (int i = 0; i < kClients; ++i)
    {
        const Streamed s = parse_streamed(cs[i]->recv_all(15000));
        EXPECT_TRUE(s.sse_headers) << "client " << i << " never got the SSE head";
        EXPECT_EQ(s.content, "Hello, world") << "client " << i << " lost or truncated deltas";
        EXPECT_EQ(s.finish, "stop") << "client " << i;
        EXPECT_TRUE(s.done) << "client " << i << " missing terminal [DONE]";
        cs[i]->close();
    }

    shutdown(); // join BEFORE reading stats(); the loop owns it
    const uint64_t enobufs = _gw->stats().uring_enobufs;
    EXPECT_EQ(_gw->stats().errors, 0u) << "buffer starvation must not be reported as an error";
    EXPECT_EQ(_gw->stats().requests, static_cast<uint64_t>(kClients));

    // The point of the test. If this fires, the pool never actually ran dry and the
    // assertions above passed without touching the recovery path, so either the
    // hook stopped working or the kernel no longer reports -ENOBUFS here, and the
    // recovery branch in ur_on_recv is dead code that needs re-examining.
    EXPECT_GT(enobufs, 0u)
        << "pool of 1 buffer across " << kClients
        << " streams did not produce -ENOBUFS; this test is not exercising what it claims";
}

// ── Timing headers (opt-in) ──────────────────────────────────────────────────
class ProxyTiming : public ProxyIT, public ::testing::WithParamInterface<llmbridge::IoBackend> {};

TEST_P(ProxyTiming, OffByDefaultNoHeadersAppear)
{
    // Default must stay byte-identical: a header is a visible API change.
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("hi")));
    const std::string r = c.recv_response();
    EXPECT_EQ(r.find("x-llmbridge-"), std::string::npos) << r;
}

TEST_P(ProxyTiming, EmitsOrderableT0AndDurations)
{
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, true);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("hi")));
    const std::string r = c.recv_response();
    ASSERT_EQ(Client::status_of(r), 200) << r;
    for (const char* h : {"x-llmbridge-t0:", "x-llmbridge-gateway-us:", "x-llmbridge-upstream-us:"})
        EXPECT_NE(r.find(h), std::string::npos) << h << " missing:\n" << r;

    // t0 must be a plausible epoch-nanosecond wall clock, not a monotonic counter
    // (steady_clock has no epoch; a raw uptime value would be ~1e10, not ~1.7e18).
    const size_t p = r.find("x-llmbridge-t0: ") + 16;
    const long long t0 = std::stoll(r.substr(p, r.find("\r\n", p) - p));
    EXPECT_GT(t0, 1700000000000000000LL) << "t0 is not epoch nanoseconds: " << t0;
}

TEST_P(ProxyTiming, T0IsStrictlyIncreasingAcrossRequests)
{
    // The ordering property the tape for inference depends on. Must hold even if
    // the system clock is disciplined mid-run, which is why t0 is an anchored
    // monotonic value instead of a raw CLOCK_REALTIME read.
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, true);
    long long prev = 0;
    for (int i = 0; i < 5; ++i)
    {
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(openai_request("hi")));
        const std::string r = c.recv_response();
        ASSERT_EQ(Client::status_of(r), 200) << "request " << i;
        const size_t p = r.find("x-llmbridge-t0: ") + 16;
        const long long t0 = std::stoll(r.substr(p, r.find("\r\n", p) - p));
        EXPECT_GT(t0, prev) << "t0 not increasing at request " << i;
        prev = t0;
    }
}

TEST_P(ProxyTiming, BodyIsUnchangedWhenHeadersAreOn)
{
    // The headers must be additive: the JSON a client parses cannot differ.
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, true);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("hi")));
    const std::string with = body_of(c.recv_response());
    EXPECT_NE(with.find("\"content\":\"ok\""), std::string::npos) << with;
    EXPECT_EQ(with.find("x-llmbridge"), std::string::npos) << "timing leaked into the body";
}

TEST_P(ProxyTiming, StreamingEmitsTtfbNotTotal)
{
    // A stream cannot know total gateway time when headers go out, so it must
    // report TTFB instead, and must NOT claim a gateway-total it cannot have.
    _backend.set_response(sse_chunked_response(64));
    start(0, true, TranslateMode::Anthropic, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, true);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const std::string all = c.recv_all();
    EXPECT_NE(all.find("x-llmbridge-t0:"), std::string::npos) << all.substr(0, 400);
    EXPECT_NE(all.find("x-llmbridge-upstream-ttfb-us:"), std::string::npos) << all.substr(0, 400);
    EXPECT_EQ(all.find("x-llmbridge-upstream-us:"), std::string::npos) << all.substr(0, 400);
    EXPECT_NE(all.find("[DONE]"), std::string::npos);
}

TEST_P(ProxyTiming, SeqIsUniqueAndIncreasingUnderConcurrency)
{
    // The sequencer property: a TOTAL order that needs no clock. Driven from
    // several client threads at once, because the counter is shared across workers
    // and a non-atomic increment would hand two requests the same number, and the one
    // failure this must never have.
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, true);

    std::mutex m;
    std::vector<long long> seqs;
    std::vector<std::thread> ths;
    for (int th = 0; th < 4; ++th)
        ths.emplace_back([&] {
            for (int i = 0; i < 6; ++i)
            {
                Client c;
                if (!c.connect(_proxy_port)) return;
                if (!c.send(openai_request("hi"))) return;
                const std::string r = c.recv_response();
                const size_t p = r.find("x-llmbridge-seq: ");
                if (p == std::string::npos) continue;
                const long long s = std::stoll(r.substr(p + 17, r.find("\r\n", p) - (p + 17)));
                std::lock_guard<std::mutex> lk(m);
                seqs.push_back(s);
            }
        });
    for (auto& t2 : ths) t2.join();

    ASSERT_GE(seqs.size(), 20u);
    std::sort(seqs.begin(), seqs.end());
    EXPECT_EQ(std::adjacent_find(seqs.begin(), seqs.end()), seqs.end())
        << "duplicate sequence number: the counter is racing";
}

TEST_P(ProxyTiming, TokenCountsComeFromTheProviderNotEstimated)
{
    // Counts must match the upstream's own usage exactly; we never estimate.
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, true);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_request("hi")));
    const std::string r = c.recv_response();
    ASSERT_EQ(Client::status_of(r), 200) << r;
    ASSERT_NE(r.find("x-llmbridge-tokens-in:"), std::string::npos) << r;
    ASSERT_NE(r.find("x-llmbridge-tokens-out:"), std::string::npos) << r;

    // Header values must equal what the body reports: one source of truth.
    // Skip the key, then any spaces: a header has "key: 3", JSON has "key":3.
    // (An earlier version assumed a space in both and skipped past the digit.)
    const auto num_at = [](const std::string& s, const char* k) {
        size_t p = s.find(k);
        EXPECT_NE(p, std::string::npos) << k;
        p += std::string(k).size();
        while (p < s.size() && s[p] == ' ') ++p;
        return std::stoll(s.substr(p, 12));
    };
    const std::string b = body_of(r);
    const auto hdr = [&](const char* k) { return num_at(r, k); };
    const auto in_body = [&](const char* k) { return num_at(b, k); };
    EXPECT_EQ(hdr("x-llmbridge-tokens-in:"), in_body("\"prompt_tokens\":"));
    EXPECT_EQ(hdr("x-llmbridge-tokens-out:"), in_body("\"completion_tokens\":"));
}

TEST_P(ProxyTiming, StreamingHasNoTokenHeaders)
{
    // Deliberate absence: token totals and chunk counts are end-of-stream facts and
    // headers precede the body. Inventing them would be worse than omitting them.
    _backend.set_response(sse_chunked_response(64));
    start(0, true, TranslateMode::Anthropic, GetParam(), Gateway::kDefaultUpstreamIdleNs, 0, true);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const std::string all = c.recv_all();
    const std::string head = all.substr(0, all.find("\r\n\r\n"));
    EXPECT_EQ(head.find("x-llmbridge-tokens-"), std::string::npos) << head;
    EXPECT_NE(head.find("x-llmbridge-seq:"), std::string::npos) << head;
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyTiming,
                         ::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                         [](const testing::TestParamInfo<llmbridge::IoBackend>& i) {
                             return i.param == llmbridge::IoBackend::Epoll ? "epoll" : "uring";
                         });

// ── Tool calling through the gateway ────────────────────────────────────────
// The translator tests prove the shapes. These prove the shapes SURVIVE the
// gateway: framing, the rebuilt upstream request, auth-header injection, and both
// event-loop backends. A tool schema is the largest and most escape-heavy thing a
// client ever sends, so it is also a good stress on the request path.
class ProxyTools : public ProxyIT, public ::testing::WithParamInterface<llmbridge::IoBackend> {};

namespace
{
    std::string tool_request(const std::string& extra_hdrs = "")
    {
        const std::string body =
            R"({"model":"claude","max_tokens":64,)"
            R"("messages":[{"role":"user","content":"weather in Paris?"}],)"
            R"("tools":[{"type":"function","function":{"name":"get_weather",)"
            R"("description":"Get weather","parameters":{"type":"object",)"
            R"("properties":{"city":{"type":"string"}},"required":["city"]}}}],)"
            R"("tool_choice":"auto"})";
        return "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n" + extra_hdrs +
               "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }

    // Anthropic answering with a tool_use block.
    std::string anthropic_tool_use_body()
    {
        return R"({"id":"msg_1","model":"claude","stop_reason":"tool_use","content":[)"
               R"({"type":"tool_use","id":"toolu_1","name":"get_weather","input":{"city":"Paris"}}],)"
               R"("usage":{"input_tokens":10,"output_tokens":5}})";
    }
} // namespace

TEST_P(ProxyTools, ToolCallRoundTripsThroughTheGateway)
{
    _backend.set_response(http_ok(anthropic_tool_use_body()));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(tool_request()));
    const std::string r = c.recv_response();
    ASSERT_EQ(Client::status_of(r), 200) << r;
    const std::string b = body_of(r);
    EXPECT_NE(b.find(R"("finish_reason":"tool_calls")"), std::string::npos) << b;
    EXPECT_NE(b.find(R"("id":"toolu_1")"), std::string::npos) << b;
    EXPECT_NE(b.find(R"("name":"get_weather")"), std::string::npos) << b;
    EXPECT_NE(b.find(R"("content":null)"), std::string::npos) << b;
}

TEST_P(ProxyTools, UpstreamReceivesAnthropicToolShape)
{
    _backend.set_response(http_ok(anthropic_tool_use_body()));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(tool_request()));
    (void)c.recv_response();
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find(R"("input_schema")"), std::string::npos) << up;
    EXPECT_NE(up.find(R"("tool_choice":{"type":"auto"})"), std::string::npos) << up;
    EXPECT_EQ(up.find(R"("parameters")"), std::string::npos) << "OpenAI key leaked upstream";
    // The schema itself must arrive byte-for-byte.
    EXPECT_NE(up.find(R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"]})"),
              std::string::npos) << up;
}

TEST_P(ProxyTools, ToolsAndAuthHeadersCoexist)
{
    // Both features rebuild the upstream request. This is the test that would catch
    // one clobbering the other.
    _backend.set_response(http_ok(anthropic_tool_use_body()));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(tool_request("Authorization: Bearer sk-tools-1\r\n")));
    const std::string r = c.recv_response();
    ASSERT_EQ(Client::status_of(r), 200) << r;
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find("x-api-key: sk-tools-1\r\n"), std::string::npos) << up;
    EXPECT_NE(up.find("anthropic-version: 2023-06-01\r\n"), std::string::npos) << up;
    EXPECT_NE(up.find(R"("input_schema")"), std::string::npos) << up;
    EXPECT_NE(up.find("Host: 127.0.0.1:"), std::string::npos) << up;
    EXPECT_EQ(up.find("Authorization:"), std::string::npos) << up;
}

TEST_P(ProxyTools, ToolResultTurnForwardsCorrectly)
{
    // The second half of an agent loop: the client returns the tool result.
    _backend.set_response(http_ok(anthropic_resp_body("18C in Paris")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    const std::string body =
        R"({"model":"claude","max_tokens":64,"messages":[)"
        R"({"role":"user","content":"weather?"},)"
        R"({"role":"assistant","content":null,"tool_calls":[{"id":"t1","type":"function",)"
        R"("function":{"name":"get_weather","arguments":"{\"city\":\"Paris\"}"}}]},)"
        R"({"role":"tool","tool_call_id":"t1","content":"18C"}]})";
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send("POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body));
    const std::string r = c.recv_response();
    ASSERT_EQ(Client::status_of(r), 200) << r;
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find(R"("type":"tool_use")"), std::string::npos) << up;
    EXPECT_NE(up.find(R"("type":"tool_result")"), std::string::npos) << up;
    EXPECT_NE(up.find(R"("tool_use_id":"t1")"), std::string::npos) << up;
    EXPECT_NE(up.find(R"("input":{"city":"Paris"})"), std::string::npos)
        << "arguments string did not become an object upstream:\n" << up;
}

TEST_P(ProxyTools, StreamingRequestWithToolsStillStreams)
{
    // Tools in a STREAMING request must not break the SSE path. (Tool-call DELTAS
    // are not implemented yet; this asserts the stream still works when tools are
    // merely declared, which is the case that would silently regress.)
    _backend.set_response(sse_chunked_response(64));
    start(0, true, TranslateMode::Anthropic, GetParam());
    const std::string body =
        R"({"model":"claude","stream":true,"messages":[{"role":"user","content":"hi"}],)"
        R"("tools":[{"type":"function","function":{"name":"f","parameters":{"type":"object"}}}]})";
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send("POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body));
    const std::string all = c.recv_all();
    EXPECT_NE(all.find("text/event-stream"), std::string::npos) << all.substr(0, 300);
    EXPECT_NE(all.find("[DONE]"), std::string::npos);
    // and the upstream still saw both the stream flag and the tools
    const std::string up = _backend.last_request();
    EXPECT_NE(up.find(R"("stream":true)"), std::string::npos) << up;
    EXPECT_NE(up.find(R"("input_schema")"), std::string::npos) << up;
}

TEST_P(ProxyTools, LargeSchemaSurvivesFraming)
{
    // A big schema exercises the request path's buffering; it must arrive intact.
    std::string props;
    for (int i = 0; i < 60; ++i)
    {
        if (i) props += ',';
        props += "\"field_" + std::to_string(i) + "\":{\"type\":\"string\",\"description\":\"d" +
                 std::to_string(i) + "\"}";
    }
    const std::string schema = "{\"type\":\"object\",\"properties\":{" + props + "}}";
    const std::string body =
        R"({"model":"claude","max_tokens":8,"messages":[{"role":"user","content":"hi"}],)"
        R"("tools":[{"type":"function","function":{"name":"big","parameters":)" + schema + "}}]}";
    _backend.set_response(http_ok(anthropic_resp_body("ok")));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send("POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body));
    ASSERT_EQ(Client::status_of(c.recv_response()), 200);
    EXPECT_NE(_backend.last_request().find(schema), std::string::npos)
        << "large schema was altered in transit";
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyTools,
                         ::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                         [](const testing::TestParamInfo<llmbridge::IoBackend>& i) {
                             return i.param == llmbridge::IoBackend::Epoll ? "epoll" : "uring";
                         });

// ── Streamed tool calls through the gateway ─────────────────────────────────
// The translator tests prove the chunk shapes; these prove they survive the
// gateway pump: chunked decode, back-pressure buffers, and both event loops.
// Chunk sizes are varied because the tool events are LONGER than text events and
// so more likely to straddle a chunk boundary mid-JSON.
class ProxyToolStream
    : public ProxyIT,
      public ::testing::WithParamInterface<std::tuple<llmbridge::IoBackend, size_t>>
{
};

TEST_P(ProxyToolStream, ToolCallsStreamAndReassemble)
{
    const auto [backend, chunk] = GetParam();
    _backend.set_response(sse_tool_response(chunk));
    start(0, true, TranslateMode::Anthropic, backend);
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const std::string all = c.recv_all();

    EXPECT_NE(all.find("text/event-stream"), std::string::npos) << all.substr(0, 200);
    // Ordinals must be 0 and 1 even though Anthropic used blocks 1 and 2.
    EXPECT_NE(all.find(R"("index":0,"id":"toolu_A")"), std::string::npos) << all;
    EXPECT_NE(all.find(R"("index":1,"id":"toolu_B")"), std::string::npos) << all;
    EXPECT_EQ(all.find(R"("index":2,"id":)"), std::string::npos)
        << "Anthropic's block index leaked into tool_calls";
    // Arguments reassemble across chunk boundaries.
    std::string args;
    size_t p = 0;
    while ((p = all.find(R"("arguments":")", p)) != std::string::npos)
    {
        size_t s = p + 13, e = s;
        while (e < all.size() && all[e] != '"') e += (all[e] == '\\') ? 2 : 1;
        args += all.substr(s, e - s);
        p = e;
    }
    EXPECT_NE(args.find(R"({\"city\":\"Paris\"})"), std::string::npos) << "args: " << args;
    EXPECT_NE(all.find(R"("finish_reason":"tool_calls")"), std::string::npos) << all;
    EXPECT_NE(all.find("[DONE]"), std::string::npos) << all;
}

INSTANTIATE_TEST_SUITE_P(
    Backends, ProxyToolStream,
    ::testing::Combine(::testing::Values(llmbridge::IoBackend::Epoll, llmbridge::IoBackend::Uring),
                       ::testing::Values(size_t{7}, size_t{64}, size_t{4096})),
    [](const testing::TestParamInfo<std::tuple<llmbridge::IoBackend, size_t>>& i) {
        return std::string(std::get<0>(i.param) == llmbridge::IoBackend::Epoll ? "epoll" : "uring") +
               "_chunk" + std::to_string(std::get<1>(i.param));
    });

// ─── The policy seam (gateway/policy.hpp) ────────────────────────────────────
//
// Here instead of in a policy_test.cpp of its own, against this directory's
// "one executable per concern" rule, because the seam is only observable
// through a running proxy: the assertions that matter are "the client got a
// 401" and "the upstream saw nothing", and both need ProxyIT. Duplicating the
// harness to satisfy a naming rule would risk the two copies drifting.
namespace
{
    // Records what it was asked and answers however the test says.
    class RecordingPolicy final : public llmbridge::Policy
    {
    public:
        explicit RecordingPolicy(llmbridge::Decision d) : _d(d) {}

        llmbridge::Decision decide(const llmbridge::RequestFacts& f) noexcept override
        {
            ++calls;
            // Copy, never retain: `head` dies with the call, and the assertions run
            // after the loop thread is joined.
            seen_head.assign(f.head.data(), f.head.size());
            // Mixed case AND no colon: the two spellings find_header used to fail
            // silently on. A policy reads headers exactly like this.
            const std::string_view auth = llmbridge::net::http::find_header(f.head, "Authorization");
            seen_auth.assign(auth.data(), auth.size());
            const std::string_view spoofable = llmbridge::net::http::find_header(f.head, "x-tenant");
            seen_spoofable.assign(spoofable.data(), spoofable.size());
            seen_body_bytes = f.body_bytes;
            return _d;
        }

        std::atomic<int> calls{0};
        std::string seen_head, seen_auth, seen_spoofable;
        size_t seen_body_bytes = 0;

    private:
        llmbridge::Decision _d;
    };

    class ProxyPolicy : public ProxyIT,
                        public ::testing::WithParamInterface<llmbridge::IoBackend> {};
} // namespace

// The stock build: no policy, nothing consulted. This fails if "default deny" is
// ever read as "deny when absent", which would brick every OSS deployment.
TEST_P(ProxyPolicy, NoPolicyInstalledForwardsEverything)
{
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string resp = c.recv_response();
    EXPECT_NE(resp.find("200 OK"), std::string::npos) << resp;
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().requests, 1u);
    EXPECT_EQ(_gw->stats().policy_denied, 0u);
    EXPECT_EQ(_backend.requests_seen(), 1);
}

TEST_P(ProxyPolicy, AllowForwardsUnchanged)
{
    RecordingPolicy pol{llmbridge::Decision{.allow = true}};
    _policy = &pol;
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string resp = c.recv_response();
    EXPECT_NE(resp.find("200 OK"), std::string::npos) << resp;
    c.close();
    shutdown();
    EXPECT_EQ(pol.calls.load(), 1) << "the seam was not consulted";
    EXPECT_EQ(_gw->stats().policy_denied, 0u);
    EXPECT_EQ(_backend.requests_seen(), 1);
}

// The one that matters. A refusal must be terminal. Asserting only "the client
// got a 401" would pass even if the request had ALSO gone to the provider.
TEST_P(ProxyPolicy, DenyAnswersTheClientAndContactsNoUpstream)
{
    RecordingPolicy pol{llmbridge::Decision{.allow = false, .deny_status = 401, .reason = "no token"}};
    _policy = &pol;
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string resp = c.recv_response();
    EXPECT_NE(resp.find("401 Unauthorized"), std::string::npos) << resp;
    EXPECT_NE(resp.find("authentication_error"), std::string::npos) << resp;
    // The reason is for the operator's log. Telling a failed caller which rule
    // refused them is free reconnaissance.
    EXPECT_EQ(resp.find("no token"), std::string::npos) << "the deny reason reached the client";
    c.close();
    shutdown();
    EXPECT_EQ(pol.calls.load(), 1);
    EXPECT_EQ(_gw->stats().policy_denied, 1u);
    EXPECT_EQ(_gw->stats().requests, 0u) << "a refused request was counted as served";
    EXPECT_EQ(_backend.requests_seen(), 0) << "a denied request reached the upstream";
    EXPECT_TRUE(_backend.last_request().empty());
}

// The shape a bug produces: a forgotten branch, a default construction on an
// error path. It must refuse.
TEST_P(ProxyPolicy, ValueInitialisedDecisionRefuses)
{
    RecordingPolicy pol{llmbridge::Decision{}};
    _policy = &pol;
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string resp = c.recv_response();
    EXPECT_EQ(resp.find("200 OK"), std::string::npos) << "a zeroed Decision was forwarded: " << resp;
    EXPECT_NE(resp.find("401"), std::string::npos) << resp;
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().policy_denied, 1u);
    EXPECT_EQ(_backend.requests_seen(), 0);
}

// An unrenderable status must not become a misleading reply, and must not become
// an ALLOW either. This path used to emit "502 Bad Gateway" for an auth refusal.
TEST_P(ProxyPolicy, OutOfRangeDenyStatusStillRefuses)
{
    RecordingPolicy pol{llmbridge::Decision{.allow = false, .deny_status = 9000, .reason = "bad status"}};
    _policy = &pol;
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string resp = c.recv_response();
    EXPECT_NE(resp.find("403 Forbidden"), std::string::npos) << resp;
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().policy_denied, 1u);
    EXPECT_EQ(_backend.requests_seen(), 0);
}

// The facts have to be usable, or the seam is a boolean with extra steps.
TEST_P(ProxyPolicy, FactsCarryTheHeadersAndTheBodySize)
{
    RecordingPolicy pol{llmbridge::Decision{.allow = true}};
    _policy = &pol;
    start(0, true, TranslateMode::None, GetParam());
    const std::string body = "{\"hello\":\"world\"}";
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    // `x-tenant-spoof` is the forgery a bare-prefix match would accept as
    // `x-tenant`. There is no genuine `x-tenant` header here, so a policy asking
    // for one must come back empty.
    ASSERT_TRUE(c.send("POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n"
                       "Authorization: Bearer kb_live_TESTVALUE\r\n"
                       "x-tenant-spoof: attacker\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body));
    (void)c.recv_response();
    c.close();
    shutdown();
    ASSERT_EQ(pol.calls.load(), 1);
    // Case-insensitive lookup, as HTTP requires: the client sent "Authorization".
    EXPECT_EQ(pol.seen_auth, "Bearer kb_live_TESTVALUE");
    EXPECT_EQ(pol.seen_spoofable, "")
        << "x-tenant-spoof was accepted as x-tenant: a client can forge a trusted field";
    EXPECT_EQ(pol.seen_body_bytes, body.size());
    // Metadata only: the head ends at the blank line, so the body is unreachable.
    EXPECT_EQ(pol.seen_head.find("hello"), std::string::npos)
        << "the request body was visible to the policy";
    EXPECT_NE(pol.seen_head.rfind("\r\n\r\n"), std::string::npos) << pol.seen_head;
    EXPECT_EQ(pol.seen_head.size(), pol.seen_head.rfind("\r\n\r\n") + 4);
}

// ── Routing across several upstreams ─────────────────────────────────────────
//
// The gateway holds a table and the POLICY picks the index. llmbridge never chooses,
// because choosing needs measurements it does not collect; what it owns is that the
// choice is honoured exactly, on both backends, including which pool a connection
// goes back to.

namespace
{
    /// Answers with its own name, so a test can tell WHICH venue served a request.
    class NamedBackend
    {
      public:
        void start(std::string name)
        {
            _name = std::move(name);
            _b.set_response(http_ok(R"({"who":")" + _name + R"("})"));
            _b.start();
        }
        void stop() { _b.stop(); }
        uint16_t port() const { return _b.port(); }
        /// Answer once, then close: a provider dropping an idle pooled keep-alive.
        /// Only the first connection dies after one response; later ones stay
        /// alive and poolable, which is what makes a misfiled retry observable.
        void close_first_connection_only() { _b.set_close_after_first_n(1); }
        int seen() const { return _b.requests_seen(); }
        std::string last() { return _b.last_request(); }

      private:
        TestBackend _b;
        std::string _name;
    };

    /// Sends every request to a fixed index, so a test states the routing it expects.
    class PinnedPolicy final : public llmbridge::Policy
    {
      public:
        explicit PinnedPolicy(int idx) : _idx(idx) {}
        llmbridge::Decision decide(const llmbridge::RequestFacts&) noexcept override
        {
            return {.allow = true, .upstream_index = _idx.load(std::memory_order_relaxed)};
        }
        /// ATOMIC because a test calls this from the main thread while the loop
        /// thread is serving: a plain int here is a genuine data race and TSan says
        /// so. Relaxed is enough, since the test synchronises on the response it
        /// waits for and only the value itself crosses threads.
        void set(int idx) { _idx.store(idx, std::memory_order_relaxed); }

      private:
        std::atomic<int> _idx;
    };
} // namespace

class ProxyRoute : public ::testing::TestWithParam<llmbridge::IoBackend>
{
  protected:
    void start(std::vector<llmbridge::Upstream> table, llmbridge::Policy* pol,
               llmbridge::RequestSink* sink = nullptr,
               std::vector<std::string> capture = {})
    {
        _gw = std::make_unique<Gateway>(0, std::move(table), 0, GetParam(),
                                        Gateway::kDefaultUpstreamIdleNs, llmbridge::TlsConfig{},
                                        false, pol, std::vector<std::string>{});
        if (sink) _gw->set_request_sink(sink, std::move(capture));
        _port = _gw->bound_port();
        _th = std::thread([this] { _gw->run(); });
    }
    void shutdown()
    {
        if (_shut) return;
        _shut = true;
        if (_gw) _gw->request_stop();
        if (_th.joinable()) _th.join();
    }
    void TearDown() override { shutdown(); }

    std::unique_ptr<Gateway> _gw;
    std::thread _th;
    uint16_t _port = 0;
    bool _shut = false;
};

// THE FEATURE. Two venues, and the policy decides which one serves each request.
TEST_P(ProxyRoute, ThePolicyChoosesWhichVenueServes)
{
    NamedBackend a, b;
    a.start("alpha");
    b.start("bravo");
    PinnedPolicy pol(0);
    start({{"127.0.0.1", a.port(), false, "", TranslateMode::None, ""},
           {"127.0.0.1", b.port(), false, "", TranslateMode::None, ""}}, &pol);

    Client c1;
    ASSERT_TRUE(c1.connect(_port));
    ASSERT_TRUE(c1.send(make_request()));
    EXPECT_NE(c1.recv_response().find("alpha"), std::string::npos);
    c1.close();

    pol.set(1);
    Client c2;
    ASSERT_TRUE(c2.connect(_port));
    ASSERT_TRUE(c2.send(make_request()));
    EXPECT_NE(c2.recv_response().find("bravo"), std::string::npos) << "the second venue did not serve";
    c2.close();

    shutdown();
    EXPECT_EQ(a.seen(), 1);
    EXPECT_EQ(b.seen(), 1);
    a.stop();
    b.stop();
}

// THE ONE THAT PROTECTS CREDENTIALS. Pools are per venue, so a keep-alive connection
// to one provider must never be handed to a request bound for another: that would put
// the request, and its credential, on a socket to the wrong company. Drive each venue
// twice so both pools are warm and a cross-pool hand-out would show up.
TEST_P(ProxyRoute, APooledConnectionIsNeverHandedToAnotherVenue)
{
    NamedBackend a, b;
    a.start("alpha");
    b.start("bravo");
    PinnedPolicy pol(0);
    start({{"127.0.0.1", a.port(), false, "", TranslateMode::None, ""},
           {"127.0.0.1", b.port(), false, "", TranslateMode::None, ""}}, &pol);

    for (int round = 0; round < 3; ++round)
        for (int idx : {0, 1})
        {
            pol.set(idx);
            Client c;
            ASSERT_TRUE(c.connect(_port));
            ASSERT_TRUE(c.send(make_request()));
            const std::string want = idx == 0 ? "alpha" : "bravo";
            EXPECT_NE(c.recv_response().find(want), std::string::npos)
                << "round " << round << " index " << idx << " was served by the wrong venue";
            c.close();
        }
    shutdown();
    EXPECT_EQ(a.seen(), 3);
    EXPECT_EQ(b.seen(), 3);
    a.stop();
    b.stop();
}

// A policy that only authenticates leaves upstream_index at -1, and an index past the
// end of the table must not send the request somewhere arbitrary. Both mean "the first
// upstream", which is what makes the single-upstream gateway a special case of this one.
TEST_P(ProxyRoute, AnUnsetOrOutOfRangeIndexMeansTheFirstUpstream)
{
    NamedBackend a, b;
    a.start("alpha");
    b.start("bravo");
    for (int idx : {-1, 7, 99})
    {
        PinnedPolicy pol(idx);
        start({{"127.0.0.1", a.port(), false, "", TranslateMode::None, ""},
               {"127.0.0.1", b.port(), false, "", TranslateMode::None, ""}}, &pol);
        Client c;
        ASSERT_TRUE(c.connect(_port));
        ASSERT_TRUE(c.send(make_request()));
        EXPECT_NE(c.recv_response().find("alpha"), std::string::npos) << "index " << idx;
        c.close();
        shutdown();
        _shut = false;
        _gw.reset();
    }
    EXPECT_EQ(b.seen(), 0) << "an out-of-range index reached the second venue";
    a.stop();
    b.stop();
}

// A stale pooled connection is resent on a FRESH one, and that retry must reach the
// SAME venue. Rerouting it would put a request already translated for this dialect,
// and carrying this venue's credential, on a socket to a different company. Nothing
// covered this until a mutation that hard-coded the retry to venue 0 survived.
TEST_P(ProxyRoute, ARetryStaysOnTheSameVenue)
{
    NamedBackend a, b;
    a.start("alpha");
    b.start("bravo");
    b.close_first_connection_only(); // its FIRST pooled connection goes stale
    PinnedPolicy pol(1);
    start({{"127.0.0.1", a.port(), false, "", TranslateMode::None, ""},
           {"127.0.0.1", b.port(), false, "", TranslateMode::None, ""}}, &pol);

    // PIPELINED on one connection, as ProxyBackend.RetriesOnStalePooledConnection does:
    // the second request reuses the pooled upstream before the loop has noticed it
    // died, which is the only way to reach the retry path deterministically. Two
    // separate connections let the gateway evict the corpse first and never retry.
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request() + make_request()));
    EXPECT_NE(c.recv_response().find("bravo"), std::string::npos) << "response 1";
    EXPECT_NE(c.recv_response().find("bravo"), std::string::npos) << "response 2";
    c.close();

    // AND WHERE THE RETRIED CONNECTION LANDED. It is physically a socket to bravo, so
    // if it is released into alpha's pool the damage appears on the NEXT request to
    // alpha, which would be answered by bravo. That is cross-venue contamination, and
    // it is invisible to any assertion about the retry itself.
    pol.set(0);
    Client c2;
    ASSERT_TRUE(c2.connect(_port));
    ASSERT_TRUE(c2.send(make_request()));
    EXPECT_NE(c2.recv_response().find("alpha"), std::string::npos)
        << "a request to alpha was served by another venue's pooled connection";
    c2.close();
    shutdown();

    // Not vacuous: assert the retry actually happened, or this test proves nothing.
    // io_uring's armed recv may evict the dead connection first (a completion-order
    // race the existing retry test documents), so the counter is only guaranteed on
    // epoll; the venue assertion below holds on both.
    // Braced: EXPECT_GT expands to an if/else, so an unbraced body here is a
    // dangling-else that GCC refuses under -Werror.
    if (GetParam() == llmbridge::IoBackend::Epoll)
    {
        EXPECT_GT(_gw->stats().upstream_retries, 0u) << "the retry path was never reached";
    }
    EXPECT_EQ(a.seen(), 1) << "alpha served only the final request";
    EXPECT_GE(b.seen(), 2);
    a.stop();
    b.stop();
}

// REGRESSION: the single-upstream constructor must carry sni_host into the table.
// It once read tls.sni_host and std::move(tls)'d in the same argument list, and with
// GCC's evaluation order the move ran first, so the string arrived EMPTY and every
// single-upstream TLS gateway verified against no hostname. Build-agnostic on purpose:
// the field is copied whether or not TLS is compiled, so upstream_tls stays false here
// and the test needs no backend, no handshake, and no TLS build.
TEST_P(ProxyRoute, SingleUpstreamConstructorKeepsSniHost)
{
    llmbridge::TlsConfig tls;
    tls.upstream_tls = false;
    tls.sni_host = "regress.example";
    Gateway gw(0, "127.0.0.1", 9, 0, TranslateMode::None, GetParam(),
               Gateway::kDefaultUpstreamIdleNs, tls);
    EXPECT_EQ(gw.upstream_sni_host(0), "regress.example")
        << "the delegating constructor dropped sni_host (read-after-move)";
    // Out of range is empty, not a crash, matching the routing default.
    EXPECT_TRUE(gw.upstream_sni_host(5).empty());
}

// ── Failover: the policy reacts to a venue that did not answer ───────────────
//
// llmbridge supplies the mechanism only. It never decides that a venue is unhealthy,
// because health is measured and it measures nothing; the DEFAULT on_failure never
// retries, so a policy that ignores failures behaves exactly as before the hook.

namespace
{
    /// Sends everything to `first`, and on failure moves to the next venue in a list.
    /// The simplest possible failover, which is the point: all of it is the caller's.
    class FailoverPolicy final : public llmbridge::Policy
    {
      public:
        FailoverPolicy(int first, std::vector<int> order) : _first(first), _order(std::move(order)) {}

        llmbridge::Decision decide(const llmbridge::RequestFacts&) noexcept override
        {
            return {.allow = true, .upstream_index = _first};
        }
        llmbridge::Retry on_failure(const llmbridge::FailureFacts& f) noexcept override
        {
            failures.fetch_add(1, std::memory_order_relaxed);
            last_reason.store(f.reason, std::memory_order_relaxed);
            last_failed.store(f.upstream_index, std::memory_order_relaxed);
            if (f.attempt >= static_cast<int>(_order.size())) return {};
            return {.retry = true, .upstream_index = _order[static_cast<size_t>(f.attempt)]};
        }

        // Atomic: production shares ONE policy across every worker, so these are
        // written from several loop threads at once. TSan reports the alternative.
        std::atomic<int> failures{0};
        std::atomic<int> last_failed{-1};
        std::atomic<const char*> last_reason{""};

      private:
        int _first;
        std::vector<int> _order;
    };

    /// Never retries, which is the default a stock policy inherits.
    class NoFailoverPolicy final : public llmbridge::Policy
    {
      public:
        explicit NoFailoverPolicy(int idx) : _idx(idx) {}
        llmbridge::Decision decide(const llmbridge::RequestFacts&) noexcept override
        {
            return {.allow = true, .upstream_index = _idx};
        }
      private:
        int _idx;
    };
} // namespace

// THE FEATURE. Venue 0 is a closed port, so the connect fails; the policy names venue
// 1 and the client is served without ever seeing the failure.
TEST_P(ProxyRoute, AFailedVenueIsRetriedOnTheNextOne)
{
    NamedBackend good;
    good.start("bravo");
    const uint16_t dead = free_port(); // nothing listening: connect refused
    FailoverPolicy pol(0, {1});
    start({{"127.0.0.1", dead, false, "", TranslateMode::None, ""},
           {"127.0.0.1", good.port(), false, "", TranslateMode::None, ""}}, &pol);

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    const std::string resp = c.recv_response();
    EXPECT_NE(resp.find("200 OK"), std::string::npos) << resp;
    EXPECT_NE(resp.find("bravo"), std::string::npos) << "the healthy venue did not serve it";
    c.close();
    shutdown();

    EXPECT_EQ(pol.failures.load(), 1) << "the policy was not told about the failure";
    EXPECT_EQ(pol.last_failed.load(), 0) << "the policy was told the wrong venue failed";
    EXPECT_EQ(_gw->stats().upstream_failovers, 1u);
    EXPECT_EQ(good.seen(), 1);
    good.stop();
}

// THE DEFAULT. A policy that does not override on_failure must behave exactly as the
// gateway did before the hook existed: the client sees the error.
TEST_P(ProxyRoute, WithoutAPolicyOpinionTheClientSeesTheFailure)
{
    NamedBackend good;
    good.start("bravo");
    const uint16_t dead = free_port();
    NoFailoverPolicy pol(0);
    start({{"127.0.0.1", dead, false, "", TranslateMode::None, ""},
           {"127.0.0.1", good.port(), false, "", TranslateMode::None, ""}}, &pol);

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("502"), std::string::npos);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().upstream_failovers, 0u);
    EXPECT_EQ(good.seen(), 0) << "the healthy venue was used without the policy asking";
    good.stop();
}

// A chain must be BOUNDED. A policy that always names another venue would otherwise
// walk the table on every failure, turning one dead provider into a latency multiplier.
TEST_P(ProxyRoute, TheFailoverChainIsBounded)
{
    const uint16_t d1 = free_port(), d2 = free_port(), d3 = free_port(), d4 = free_port();
    FailoverPolicy pol(0, {1, 2, 3}); // every one of them is dead
    start({{"127.0.0.1", d1, false, "", TranslateMode::None, ""},
           {"127.0.0.1", d2, false, "", TranslateMode::None, ""},
           {"127.0.0.1", d3, false, "", TranslateMode::None, ""},
           {"127.0.0.1", d4, false, "", TranslateMode::None, ""}}, &pol);
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("502"), std::string::npos);
    c.close();
    shutdown();
    EXPECT_LE(_gw->stats().upstream_failovers, 2u) << "the chain was not bounded";
}

// Naming the venue that just failed is the one answer that cannot help, and is how a
// policy accidentally writes an infinite loop. Refused, and the client gets the error.
TEST_P(ProxyRoute, RetryingTheSameVenueIsRefused)
{
    const uint16_t dead = free_port();
    NamedBackend good;
    good.start("bravo");
    FailoverPolicy pol(0, {0}); // "retry venue 0", which is the one that failed
    start({{"127.0.0.1", dead, false, "", TranslateMode::None, ""},
           {"127.0.0.1", good.port(), false, "", TranslateMode::None, ""}}, &pol);
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("502"), std::string::npos);
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().upstream_failovers, 0u);
    good.stop();
}

// THE ONE THAT MAKES IT MORE THAN A RECONNECT. Failing over to a venue with a
// DIFFERENT dialect means the request must be REBUILT, not resent: the bytes queued
// for the first venue were translated for its API. Venue 0 is a dead Anthropic
// endpoint, venue 1 an OpenAI-compatible one, so the retry must arrive untranslated.
TEST_P(ProxyRoute, FailoverAcrossDialectsRebuildsTheRequest)
{
    TestBackend plain;
    plain.set_response(http_ok(R"({"id":"x","object":"chat.completion","choices":[]})"));
    plain.start();
    const uint16_t dead = free_port();
    FailoverPolicy pol(0, {1});
    start({{"127.0.0.1", dead, false, "", TranslateMode::Anthropic, ""},
           {"127.0.0.1", plain.port(), false, "", TranslateMode::None, ""}}, &pol);

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(openai_request("hi")));
    EXPECT_NE(c.recv_response().find("200 OK"), std::string::npos);
    c.close();
    shutdown();

    const std::string up = plain.last_request();
    // Passthrough: the client's own OpenAI request, NOT the /v1/messages rebuild that
    // was queued for the venue that failed.
    EXPECT_NE(up.find("POST /v1/chat/completions"), std::string::npos) << up;
    EXPECT_EQ(up.find("/v1/messages"), std::string::npos)
        << "the retry carried the FAILED venue's translation: " << up;
    EXPECT_EQ(_gw->stats().upstream_failovers, 1u);
    plain.stop();
}

// The DEFAULT on_failure must never retry. Pinned to venue 1 (dead) with venue 0
// healthy, so a default that returned "retry venue 0" would quietly succeed; the
// earlier version of this test pinned to venue 0 and could not tell the difference.
TEST_P(ProxyRoute, TheDefaultOnFailureNeverRetries)
{
    NamedBackend healthy;
    healthy.start("alpha");
    const uint16_t dead = free_port();
    NoFailoverPolicy pol(1);
    start({{"127.0.0.1", healthy.port(), false, "", TranslateMode::None, ""},
           {"127.0.0.1", dead, false, "", TranslateMode::None, ""}}, &pol);
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("502"), std::string::npos)
        << "the base-class on_failure retried when it must not";
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().upstream_failovers, 0u);
    EXPECT_EQ(healthy.seen(), 0) << "a healthy venue was used without the policy asking";
    healthy.stop();
}

// The policy must be told WHICH venue failed, or its health tracking attributes the
// outage to the wrong provider and ejects the innocent one.
TEST_P(ProxyRoute, ThePolicyIsToldWhichVenueFailed)
{
    NamedBackend healthy;
    healthy.start("alpha");
    const uint16_t dead = free_port();
    FailoverPolicy pol(1, {0}); // start on venue 1, which is dead
    start({{"127.0.0.1", healthy.port(), false, "", TranslateMode::None, ""},
           {"127.0.0.1", dead, false, "", TranslateMode::None, ""}}, &pol);
    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("alpha"), std::string::npos);
    c.close();
    shutdown();
    EXPECT_EQ(pol.last_failed.load(), 1) << "the policy was told the wrong venue failed";
    healthy.stop();
}

// Two requests on ONE keep-alive connection, both failing over. Without a reset
// between them the second reuses the FIRST request's saved bytes, so the provider is
// asked the first question twice and the client gets an answer to a question it did
// not ask. Distinct bodies are what makes that visible.
TEST_P(ProxyRoute, EachRequestGetsItsOwnFailoverBudgetAndBytes)
{
    TestBackend good;
    good.set_response(http_ok(R"({"ok":true})"));
    good.start();
    const uint16_t dead = free_port();
    FailoverPolicy pol(0, {1});
    start({{"127.0.0.1", dead, false, "", TranslateMode::None, ""},
           {"127.0.0.1", good.port(), false, "", TranslateMode::None, ""}}, &pol);

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request("FIRST-BODY")));
    EXPECT_NE(c.recv_response().find("200 OK"), std::string::npos) << "request 1";
    ASSERT_TRUE(c.send(make_request("SECOND-BODY")));
    EXPECT_NE(c.recv_response().find("200 OK"), std::string::npos) << "request 2";
    c.close();
    shutdown();

    // The SECOND request must have carried its own body upstream.
    const std::string up = good.last_request();
    EXPECT_NE(up.find("SECOND-BODY"), std::string::npos)
        << "the second failover resent the FIRST request: " << up;
    EXPECT_EQ(up.find("FIRST-BODY"), std::string::npos) << up;
    EXPECT_EQ(_gw->stats().upstream_failovers, 2u) << "each request should fail over once";
    good.stop();
}

// The reachable half of the "client saw bytes" guard. A response still draining to
// the client must not be followed by a re-sent request's response: the client would
// receive two answers on one connection and frame them as one.
TEST_P(ProxyRoute, AStreamingClientIsNeverFailedOver)
{
    // A streaming client never reaches the failover path at all: every call site
    // diverts it first. Asserted end to end so that if a future site forgets to,
    // this test says so before a customer sees a duplicated answer.
    TestBackend sse;
    sse.set_response("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     "Cache-Control: no-cache\r\n\r\n"
                     "event: message_start\ndata: {\"type\":\"message_start\","
                     "\"message\":{\"id\":\"m\",\"model\":\"x\",\"usage\":"
                     "{\"input_tokens\":1,\"output_tokens\":0}}}\n\n");
    sse.set_stall(2); // half a response, then hold the connection open forever
    sse.start();
    const uint16_t dead = free_port();
    FailoverPolicy pol(0, {1});
    _gw = std::make_unique<Gateway>(
        0, std::vector<llmbridge::Upstream>{
               {"127.0.0.1", sse.port(), false, "", TranslateMode::Anthropic, ""},
               {"127.0.0.1", dead, false, "", TranslateMode::None, ""}},
        0, GetParam(), 200'000'000LL /* 200 ms idle */, llmbridge::TlsConfig{}, false, &pol,
        std::vector<std::string>{});
    _port = _gw->bound_port();
    _th = std::thread([this] { _gw->run(); });

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(openai_request("hi", /*keep_alive=*/true) + ""));
    // Give the idle timeout time to fire while the stream is open.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().upstream_failovers, 0u)
        << "a client mid-stream was failed over, which duplicates its answer";
    sse.stop();
}

// STREAMING, BEFORE THE FIRST BYTE. A streaming request that cannot even reach its
// venue is an ordinary failure: nothing has been sent to the client, so it fails over
// and the client gets a complete stream from the healthy venue. This is the case that
// matters for a voice agent, where a provider blip before the first token is the
// difference between a pause and a dropped call.
TEST_P(ProxyRoute, AStreamingRequestFailsOverBeforeItsFirstByte)
{
    TestBackend sse;
    sse.set_response(sse_chunked_response(64));
    sse.start();
    const uint16_t dead = free_port();
    FailoverPolicy pol(0, {1});
    start({{"127.0.0.1", dead, false, "", TranslateMode::Anthropic, ""},
           {"127.0.0.1", sse.port(), false, "", TranslateMode::Anthropic, ""}}, &pol);

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const Streamed st = parse_streamed(c.recv_all());
    EXPECT_TRUE(st.sse_headers) << "no stream reached the client";
    EXPECT_EQ(st.content, "Hello, world") << "the stream did not complete after the failover";
    c.close();
    shutdown();
    EXPECT_EQ(_gw->stats().upstream_failovers, 1u);
    EXPECT_EQ(pol.failures.load(), 1);
    sse.stop();
}

// MULTITHREADING. Production runs one Gateway per worker, all sharing ONE policy, so
// the hook is called concurrently from several loop threads. The gateway adds no
// locking around the seam by design, which makes this the shape a data race would
// appear in; run it under TSan and the counters below must still add up exactly.
TEST_P(ProxyRoute, ConcurrentWorkersShareOnePolicyAndAllFailOver)
{
    constexpr int kWorkers = 4;
    constexpr int kPerWorker = 25;
    NamedBackend good;
    good.start("bravo");
    const uint16_t dead = free_port();
    FailoverPolicy pol(0, {1});

    std::vector<std::unique_ptr<Gateway>> gws;
    std::vector<std::thread> loops;
    std::vector<uint16_t> ports;
    for (int i = 0; i < kWorkers; ++i)
    {
        // Separate listeners, not SO_REUSEPORT: the point here is one POLICY
        // shared by several loop threads, and separate ports make which worker served
        // a request irrelevant instead of racy.
        gws.push_back(std::make_unique<Gateway>(
            uint16_t{0},
            std::vector<llmbridge::Upstream>{
                {"127.0.0.1", dead, false, "", TranslateMode::None, ""},
                {"127.0.0.1", good.port(), false, "", TranslateMode::None, ""}},
            int64_t{0}, GetParam(), Gateway::kDefaultUpstreamIdleNs, llmbridge::TlsConfig{},
            false, &pol, std::vector<std::string>{}));
        ports.push_back(gws.back()->bound_port());
    }
    for (auto& g : gws) loops.emplace_back([&g] { g->run(); });

    std::atomic<int> served{0};
    std::vector<std::thread> clients;
    for (int w = 0; w < kWorkers; ++w)
        clients.emplace_back([&, w] {
            for (int i = 0; i < kPerWorker; ++i)
            {
                Client c;
                if (!c.connect(ports[static_cast<size_t>(w)])) continue;
                if (!c.send(make_request())) continue;
                if (c.recv_response().find("bravo") != std::string::npos) served.fetch_add(1);
                c.close();
            }
        });
    for (auto& t : clients) t.join();
    for (auto& g : gws) g->request_stop();
    for (auto& t : loops) t.join();

    EXPECT_EQ(served.load(), kWorkers * kPerWorker) << "some requests were not failed over";
    EXPECT_EQ(pol.failures.load(), kWorkers * kPerWorker) << "the policy missed a failure";
    uint64_t total = 0;
    for (const auto& g : gws) total += g->stats().upstream_failovers;
    EXPECT_EQ(total, static_cast<uint64_t>(kWorkers * kPerWorker));
    EXPECT_EQ(good.seen(), kWorkers * kPerWorker);
    good.stop();
}

// ── Decision::tag -> FailureFacts::tag ───────────────────────────────────────

namespace
{
    class TaggingPolicy final : public llmbridge::Policy
    {
      public:
        static constexpr uint64_t kTagBase = 0xC0FFEE00u;

        TaggingPolicy(std::vector<int> venue_of_request, std::vector<int> retry_order)
            : _venues(std::move(venue_of_request)), _retry(std::move(retry_order))
        {
        }

        llmbridge::Decision decide(const llmbridge::RequestFacts&) noexcept override
        {
            const int n = _decides.fetch_add(1, std::memory_order_relaxed);
            const size_t i = std::min(static_cast<size_t>(n), _venues.size() - 1);
            return {.allow = true, .upstream_index = _venues[i], .tag = kTagBase + n};
        }
        llmbridge::Retry on_failure(const llmbridge::FailureFacts& f) noexcept override
        {
            const int n = _failures.fetch_add(1, std::memory_order_relaxed);
            if (n < static_cast<int>(kMaxSeen)) seen_tags[static_cast<size_t>(n)] = f.tag;
            if (f.attempt >= static_cast<int>(_retry.size())) return {};
            return {.retry = true, .upstream_index = _retry[static_cast<size_t>(f.attempt)]};
        }

        int failures() const noexcept { return _failures.load(std::memory_order_relaxed); }

        static constexpr size_t kMaxSeen = 8;
        std::array<std::atomic<uint64_t>, kMaxSeen> seen_tags{};

      private:
        std::vector<int> _venues, _retry;
        std::atomic<int> _decides{0};
        std::atomic<int> _failures{0};
    };

    /// Leaves Decision::tag at its default and records what the failure carries.
    class TagRecordingPolicy final : public llmbridge::Policy
    {
      public:
        TagRecordingPolicy(int first, int retry_on) : _first(first), _retry_on(retry_on) {}

        llmbridge::Decision decide(const llmbridge::RequestFacts&) noexcept override
        {
            return {.allow = true, .upstream_index = _first};
        }
        llmbridge::Retry on_failure(const llmbridge::FailureFacts& f) noexcept override
        {
            seen_tag.store(f.tag, std::memory_order_relaxed);
            return {.retry = true, .upstream_index = _retry_on};
        }

        std::atomic<uint64_t> seen_tag{~0ull}; ///< poisoned, so "never called" is visible

      private:
        int _first, _retry_on;
    };
} // namespace

// THE ROUND TRIP. decide() tags the request, the venue refuses the connect, and
// on_failure() must receive that exact value.
TEST_P(ProxyRoute, TheTagSetAtDecideArrivesAtTheFailure)
{
    NamedBackend good;
    good.start("bravo");
    const uint16_t dead = free_port();
    TaggingPolicy pol({0}, {1}); // venue 0 is dead; retry on venue 1
    start({{"127.0.0.1", dead, false, "", TranslateMode::None, ""},
           {"127.0.0.1", good.port(), false, "", TranslateMode::None, ""}}, &pol);

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("bravo"), std::string::npos);
    c.close();
    shutdown();

    ASSERT_EQ(pol.failures(), 1);
    EXPECT_EQ(pol.seen_tags[0].load(), TaggingPolicy::kTagBase + 0);
    good.stop();
}

// One request, several failures: attempt 0 and attempt 1 are the SAME decision, so
// they must carry the same tag. A tag that mutated across the chain would resolve
// the second failure against the wrong routing context.
TEST_P(ProxyRoute, TheTagIsStableAcrossTheFailoverChain)
{
    NamedBackend good;
    good.start("bravo");
    const uint16_t dead1 = free_port(), dead2 = free_port();
    TaggingPolicy pol({0}, {1, 2}); // dead -> dead -> healthy
    start({{"127.0.0.1", dead1, false, "", TranslateMode::None, ""},
           {"127.0.0.1", dead2, false, "", TranslateMode::None, ""},
           {"127.0.0.1", good.port(), false, "", TranslateMode::None, ""}}, &pol);

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("bravo"), std::string::npos);
    c.close();
    shutdown();

    ASSERT_EQ(pol.failures(), 2);
    EXPECT_EQ(pol.seen_tags[0].load(), TaggingPolicy::kTagBase + 0);
    EXPECT_EQ(pol.seen_tags[1].load(), TaggingPolicy::kTagBase + 0)
        << "attempt 1 carried a different tag from attempt 0 of the same request";
    good.stop();
}

// Two requests on ONE keep-alive connection. The second decision's tag must replace
// the first, or a failure on request 2 is resolved against request 1's context: the
// same staleness bug the per-request failover reset already guards for the bytes.
TEST_P(ProxyRoute, AKeepAliveConnectionCarriesEachRequestsOwnTag)
{
    NamedBackend good;
    good.start("bravo");
    const uint16_t dead = free_port();
    // Request 0 -> venue 1 (healthy, no failure). Request 1 -> venue 0 (dead).
    TaggingPolicy pol({1, 0}, {1});
    start({{"127.0.0.1", dead, false, "", TranslateMode::None, ""},
           {"127.0.0.1", good.port(), false, "", TranslateMode::None, ""}}, &pol);

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("bravo"), std::string::npos);
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("bravo"), std::string::npos);
    c.close();
    shutdown();

    ASSERT_EQ(pol.failures(), 1) << "only the second request's venue was dead";
    EXPECT_EQ(pol.seen_tags[0].load(), TaggingPolicy::kTagBase + 1)
        << "the failure carried the FIRST request's tag";
    good.stop();
}

// The default. FailoverPolicy never sets Decision::tag, so the failure must read 0:
// a stale or invented value here would send every existing policy a meaning it never
// wrote.
TEST_P(ProxyRoute, APolicyThatNeverTagsSeesZeroAtTheFailure)
{
    NamedBackend good;
    good.start("bravo");
    const uint16_t dead = free_port();
    TagRecordingPolicy pol(0, 1);
    start({{"127.0.0.1", dead, false, "", TranslateMode::None, ""},
           {"127.0.0.1", good.port(), false, "", TranslateMode::None, ""}}, &pol);

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("bravo"), std::string::npos);
    c.close();
    shutdown();

    EXPECT_EQ(pol.seen_tag.load(), 0u);
    good.stop();
}

// ── RequestSink: the completion seam ─────────────────────────────────────────
//
// llmbridge measures and hands over; meaning is the sink's business. The claims:
// every completion emits exactly one record, streams and gateway-generated errors
// included; captured headers are copied at framing (the buffer is reused by
// completion); and the stamps, venue, attempts and tag describe the request that
// actually ran.

namespace
{
    class RecordingSink final : public llmbridge::RequestSink
    {
      public:
        void on_request(const llmbridge::RequestRecord& r) noexcept override
        {
            std::lock_guard<std::mutex> lk(_mu);
            Copy c;
            c.r = r;
            for (size_t i = 0; i < llmbridge::kSinkCaptureMax; ++i)
                c.cap[i].assign(r.captured[i]); // the views die with the call
            _records.push_back(std::move(c));
        }
        struct Copy
        {
            llmbridge::RequestRecord r;
            std::string cap[llmbridge::kSinkCaptureMax];
        };
        std::vector<Copy> records()
        {
            std::lock_guard<std::mutex> lk(_mu);
            return _records;
        }

      private:
        std::mutex _mu; // records() is read from the test thread after shutdown
        std::vector<Copy> _records;
    };
} // namespace

TEST_P(ProxyRoute, TheSinkSeesACompletedRequestWithItsCapturedHeaders)
{
    NamedBackend b;
    b.start("alpha");
    RecordingSink sink;
    NoFailoverPolicy pol(0);
    start({{"127.0.0.1", b.port(), false, "", TranslateMode::None, ""}}, &pol, &sink,
          {"x-kottos-run", "x-kottos-step"});

    Client c;
    ASSERT_TRUE(c.connect(_port));
    std::string req = "POST /v1/chat/completions HTTP/1.1\r\nHost: h\r\n"
                      "x-kottos-run: run-42\r\nx-kottos-step: extract\r\n"
                      "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
    ASSERT_TRUE(c.send(req));
    EXPECT_NE(c.recv_response().find("alpha"), std::string::npos);
    c.close();
    shutdown();

    b.stop();
    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 1u);
    const llmbridge::RequestRecord& r = recs[0].r;
    EXPECT_GT(r.wall_t0_ns, 0);
    EXPECT_GT(r.ts_req_recvd, 0);
    EXPECT_GE(r.ts_req_built, r.ts_req_recvd);
    EXPECT_GE(r.ts_wire_ready, r.ts_req_built);
    EXPECT_GE(r.ts_up_sent, r.ts_wire_ready);
    EXPECT_GE(r.ts_up_recvd, r.ts_up_sent);
    EXPECT_GE(r.ts_done, r.ts_up_recvd);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.upstream_index, 0);
    EXPECT_EQ(r.attempts, 0);
    EXPECT_FALSE(r.streamed);
    EXPECT_FALSE(r.error_reply);
    EXPECT_FALSE(r.translated);
    EXPECT_EQ(r.backend, GetParam() == llmbridge::IoBackend::Uring ? 2 : 1);
    EXPECT_EQ(recs[0].cap[0], "run-42");
    EXPECT_EQ(recs[0].cap[1], "extract");
}

// The reply the gateway itself generates is a completion too: a tape that only
// records successes cannot answer "why was this run slow", because the refusals
// and the failures are the interesting rows.
TEST_P(ProxyRoute, TheSinkSeesGatewayGeneratedErrorReplies)
{
    NamedBackend b;
    b.start("alpha");
    RecordingSink sink;
    // Deny everything: the reply is ours, no provider is contacted.
    class DenyPolicy final : public llmbridge::Policy
    {
      public:
        llmbridge::Decision decide(const llmbridge::RequestFacts&) noexcept override
        {
            return {.allow = false, .deny_status = 429, .reason = "test"};
        }
    } pol;
    start({{"127.0.0.1", b.port(), false, "", TranslateMode::None, ""}}, &pol, &sink, {});

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("429"), std::string::npos);
    c.close();
    shutdown();

    b.stop();
    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].r.status, 429);
    EXPECT_TRUE(recs[0].r.error_reply);
    EXPECT_EQ(recs[0].r.ts_up_sent, 0) << "a refused request has no upstream stamps";
    EXPECT_EQ(recs[0].cap[0], "");
}

// After a failover the record must describe the venue that SERVED, the attempts it
// took, and the tag of the decision, or cost attribution lands on the dead venue.
TEST_P(ProxyRoute, TheSinkRecordsTheServingVenueAfterFailover)
{
    NamedBackend good;
    good.start("bravo");
    const uint16_t dead = free_port();
    RecordingSink sink;
    TaggingPolicy pol({0}, {1});
    start({{"127.0.0.1", dead, false, "", TranslateMode::None, ""},
           {"127.0.0.1", good.port(), false, "", TranslateMode::None, ""}}, &pol, &sink, {});

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request()));
    EXPECT_NE(c.recv_response().find("bravo"), std::string::npos);
    c.close();
    shutdown();

    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].r.upstream_index, 1) << "the record blames the dead venue";
    EXPECT_EQ(recs[0].r.attempts, 1);
    EXPECT_EQ(recs[0].r.tag, TaggingPolicy::kTagBase + 0);
    good.stop();
}

// Two requests on one keep-alive connection: one record each, with each request's
// OWN captured headers, because the capture is per request, not per connection.
TEST_P(ProxyRoute, AKeepAliveConnectionEmitsOneRecordPerRequest)
{
    NamedBackend b;
    b.start("alpha");
    RecordingSink sink;
    NoFailoverPolicy pol(0);
    start({{"127.0.0.1", b.port(), false, "", TranslateMode::None, ""}}, &pol, &sink,
          {"x-kottos-run"});

    Client c;
    ASSERT_TRUE(c.connect(_port));
    for (const char* run : {"run-a", "run-b"})
    {
        const std::string body = "{}";
        std::string req = "POST /v1/chat/completions HTTP/1.1\r\nHost: h\r\n"
                          "x-kottos-run: " + std::string(run) + "\r\n"
                          "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n" + body;
        ASSERT_TRUE(c.send(req));
        EXPECT_NE(c.recv_response().find("alpha"), std::string::npos);
    }
    c.close();
    shutdown();

    b.stop();
    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[0].cap[0], "run-a");
    EXPECT_EQ(recs[1].cap[0], "run-b");
    EXPECT_NE(recs[0].r.seq, recs[1].r.seq);
}

// A malformed request fails BEFORE framing, so it never reaches the per-request
// capture. Its 400's record must be empty, never the previous request's identity:
// billing a customer for a stranger's garbage is the bug this test pins.
TEST_P(ProxyRoute, AFramingErrorRecordDoesNotInheritThePreviousRequests)
{
    NamedBackend b;
    b.start("alpha");
    RecordingSink sink;
    NoFailoverPolicy pol(0);
    start({{"127.0.0.1", b.port(), false, "", TranslateMode::None, ""}}, &pol, &sink,
          {"x-kottos-run"});

    Client c;
    ASSERT_TRUE(c.connect(_port));
    std::string good = "POST /v1/chat/completions HTTP/1.1\r\nHost: h\r\n"
                       "x-kottos-run: run-good\r\n"
                       "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
    ASSERT_TRUE(c.send(good));
    EXPECT_NE(c.recv_response().find("alpha"), std::string::npos);
    ASSERT_TRUE(c.send("GET /x HTTP/1.1\r\nContent-Length : 0\r\n\r\n")); // malformed
    EXPECT_NE(c.recv_response().find("400"), std::string::npos);
    c.close();
    shutdown();
    b.stop();

    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[0].cap[0], "run-good");
    EXPECT_EQ(recs[1].r.status, 400);
    EXPECT_TRUE(recs[1].r.error_reply);
    EXPECT_EQ(recs[1].cap[0], "") << "the 400 inherited the previous request's run id";
    EXPECT_EQ(recs[1].r.tag, 0u) << "the 400 inherited the previous request's tag";
}

// A request that OMITS a captured header is the common case, and find_header returns
// a null view for it. Copying 0 bytes from a null pointer is undefined behaviour even
// though it "works": UBSan flags it, and no test sent such a request until this one.
TEST_P(ProxyRoute, ACaptureConfiguredButAbsentIsEmptyAndNotUndefined)
{
    NamedBackend b;
    b.start("alpha");
    RecordingSink sink;
    NoFailoverPolicy pol(0);
    start({{"127.0.0.1", b.port(), false, "", TranslateMode::None, ""}}, &pol, &sink,
          {"x-kottos-run", "x-kottos-step"});

    Client c;
    ASSERT_TRUE(c.connect(_port));
    ASSERT_TRUE(c.send(make_request())); // neither captured header is present
    EXPECT_NE(c.recv_response().find("alpha"), std::string::npos);
    c.close();
    shutdown();
    b.stop();

    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].cap[0], "");
    EXPECT_EQ(recs[0].cap[1], "");
    EXPECT_EQ(recs[0].r.status, 200);
}

// An over-long header value is truncated at the cap, never overrun and never
// carried whole: the sink's own consumer decides whether a truncated key is usable.
TEST_P(ProxyRoute, ACapturedHeaderIsBoundedAtTheCap)
{
    NamedBackend b;
    b.start("alpha");
    RecordingSink sink;
    NoFailoverPolicy pol(0);
    start({{"127.0.0.1", b.port(), false, "", TranslateMode::None, ""}}, &pol, &sink,
          {"x-kottos-run"});

    const std::string longv(200, 'r');
    Client c;
    ASSERT_TRUE(c.connect(_port));
    std::string req = "POST /v1/chat/completions HTTP/1.1\r\nHost: h\r\n"
                      "x-kottos-run: " + longv + "\r\n"
                      "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
    ASSERT_TRUE(c.send(req));
    EXPECT_NE(c.recv_response().find("alpha"), std::string::npos);
    c.close();
    shutdown();

    b.stop();
    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].cap[0], std::string(llmbridge::kSinkCaptureBytes, 'r'));
}

// A finished stream is a completion like any other: one record, streamed flag,
// the provider's own token counts, t4 at the response head, and ts_first_token at
// the first CONTENT token, which for a real stream lands at or after the head.
TEST_P(ProxyStream, TheSinkSeesAFinishedStreamWithTokenCounts)
{
    RecordingSink sink;
    _sink = &sink;
    // The tool stream, because it is the mock that carries usage (9 in, 14 out);
    // the plain text mock has none, and 0 would prove nothing.
    _backend.set_response(sse_tool_response(4096));
    start(0, true, TranslateMode::Anthropic, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi")));
    const Streamed s = parse_streamed(c.recv_all());
    ASSERT_TRUE(s.done);
    c.close();
    shutdown();

    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 1u);
    const llmbridge::RequestRecord& r = recs[0].r;
    EXPECT_TRUE(r.streamed);
    EXPECT_FALSE(r.truncated);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.translated);
    EXPECT_EQ(r.tokens_in, 9);
    EXPECT_EQ(r.tokens_out, 14);
    EXPECT_GE(r.ts_up_recvd, r.ts_up_sent);
    // The first content token is stamped, sits at or after the head (t4), and no
    // later than the flush. This is the real TTFT the tape's ttft_us reads.
    EXPECT_GT(r.ts_first_token, 0) << "a stream that produced content must stamp the first token";
    EXPECT_GE(r.ts_first_token, r.ts_up_recvd);
    EXPECT_GE(r.ts_done, r.ts_first_token);
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyRoute,
                         ::testing::Values(llmbridge::IoBackend::Epoll,
                                           llmbridge::IoBackend::Uring),
                         [](const testing::TestParamInfo<llmbridge::IoBackend>& i) {
                             return i.param == llmbridge::IoBackend::Epoll ? "epoll" : "uring";
                         });

INSTANTIATE_TEST_SUITE_P(Backends, ProxyPolicy,
                         ::testing::Values(llmbridge::IoBackend::Epoll,
                                           llmbridge::IoBackend::Uring));

// ── Byte-forward streaming: an OpenAI-compatible venue needs no translator ─────
//
// Until 2026-08-21 the streaming pump was entered for the Anthropic path ONLY, so a
// passthrough SSE response was framed as a whole body and handed over at the end.
// Measured against an upstream finishing at 1000 ms, the client's first byte arrived
// at 1003 ms instead of 200 ms. That is the default deployment shape and the exact
// workload the latency claim is sold against, so these tests pin both halves: the
// bytes arrive as they are produced, and they arrive unaltered.
namespace
{
    /// An OpenAI SSE stream: two content chunks, a usage chunk, then [DONE].
    std::string openai_sse_events()
    {
        return "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"content\":\"one\"}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"content\":\"two\"}}]}\n\n"
               "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":11,"
               "\"completion_tokens\":5,\"total_tokens\":16,"
               "\"prompt_tokens_details\":{\"cached_tokens\":7}}}\n\n"
               "data: [DONE]\n\n";
    }

    std::string openai_sse_response(size_t chunk)
    {
        return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
               "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n" +
               sse_chunk_encode(openai_sse_events(), chunk);
    }

    std::string openai_stream_request_with_usage()
    {
        return make_request("{\"model\":\"gpt-4o\",\"stream\":true,"
                            "\"stream_options\":{\"include_usage\":true},"
                            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");
    }
} // namespace

class ProxyForwardStream : public ProxyIT,
                           public ::testing::WithParamInterface<llmbridge::IoBackend>
{
};

TEST_P(ProxyForwardStream, TheProvidersEventsArriveUnaltered)
{
    // Byte-for-byte: a venue that already speaks the client's dialect must not have
    // its stream rewritten, including its own terminal [DONE].
    _backend.set_response(openai_sse_response(4096));
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request_with_usage()));
    const std::string raw = c.recv_all();

    const size_t hdr = raw.find("\r\n\r\n");
    ASSERT_NE(hdr, std::string::npos) << raw;
    EXPECT_NE(raw.find("Content-Type: text/event-stream"), std::string::npos) << raw;
    EXPECT_EQ(raw.substr(hdr + 4), openai_sse_events())
        << "the forwarded body is not the provider's bytes";
}

TEST_P(ProxyForwardStream, ItStreamsRatherThanBufferingToTheEnd)
{
    // The regression that started this, tested structurally so no clock decides
    // whether it passes. The upstream trickles its response in 8-byte writes, so the body is still
    // arriving long after its head has been framed. A streaming gateway answers the
    // client the moment it sees that head; the whole-body path emits nothing until it
    // has framed the entire response, so its first write would carry the [DONE] too.
    _backend.set_trickle(8);
    _backend.set_response(openai_sse_response(4096));
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request_with_usage()));

    const std::string first = c.recv_some();
    EXPECT_NE(first.find("text/event-stream"), std::string::npos) << first;
    EXPECT_EQ(first.find("[DONE]"), std::string::npos)
        << "the whole stream arrived in one read, so it was buffered: " << first;
    // And it does finish: a stream that starts early must still deliver everything.
    const std::string rest = c.recv_all();
    EXPECT_NE((first + rest).find("[DONE]"), std::string::npos);
}

TEST_P(ProxyForwardStream, TheSinkGetsTheProvidersTokenCountsIncludingCached)
{
    // The promise this exists to keep. Nothing parses a byte-forwarded stream, so
    // without the tail scan a streamed passthrough request reports no tokens at all,
    // and a cached-token figure is exactly what a design partner asked for.
    RecordingSink sink;
    _sink = &sink;
    _backend.set_response(openai_sse_response(4096));
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request_with_usage()));
    (void)c.recv_all();
    c.close();
    shutdown();

    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 1u);
    const llmbridge::RequestRecord& r = recs[0].r;
    EXPECT_TRUE(r.streamed);
    EXPECT_FALSE(r.translated);
    EXPECT_EQ(r.tokens_in, 11);
    EXPECT_EQ(r.tokens_out, 5);
    EXPECT_EQ(r.cached_tokens, 7);
    // TTFT is stamped on the first chunk carrying content, so the role-only opening
    // chunk does not claim the token arrived early.
    EXPECT_GT(r.ts_first_token, 0);
    EXPECT_GE(r.ts_first_token, r.ts_up_recvd);
}

TEST_P(ProxyForwardStream, WithoutIncludeUsageNothingIsInventedFromNothing)
{
    // No usage chunk means no counts. -1 is "not reported"; zero would be a claim.
    RecordingSink sink;
    _sink = &sink;
    _backend.set_response(openai_sse_response(4096));
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request("hi"))); // no stream_options
    (void)c.recv_all();
    c.close();
    shutdown();

    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].r.tokens_in, -1);
    EXPECT_EQ(recs[0].r.cached_tokens, -1);
}

TEST_P(ProxyForwardStream, AFullSizeProviderUsageChunkIsStillFound)
{
    // The retained tail is sized by what must FIT. A real OpenAI usage chunk carries
    // system_fingerprint, service_tier and both *_details blocks, and is followed by
    // data: [DONE], which puts the counts several hundred bytes from the end. This
    // pins that sizing against a realistic chunk instead of against arithmetic in a
    // comment.
    const std::string events =
        "data: {\"choices\":[{\"delta\":{\"content\":\"one\"}}]}\n\n"
        "data: {\"id\":\"chatcmpl-B7xQ2kZvLmNpQrStUvWxYzAbCdEf\","
        "\"object\":\"chat.completion.chunk\",\"created\":1755800000,"
        "\"model\":\"gpt-4o-2024-08-06\",\"service_tier\":\"default\","
        "\"system_fingerprint\":\"fp_a1b2c3d4e5\",\"choices\":[],"
        "\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":5,\"total_tokens\":16,"
        "\"prompt_tokens_details\":{\"cached_tokens\":7,\"audio_tokens\":0},"
        "\"completion_tokens_details\":{\"reasoning_tokens\":0,\"audio_tokens\":0,"
        "\"accepted_prediction_tokens\":0,\"rejected_prediction_tokens\":0}}}\n\n"
        "data: [DONE]\n\n";
    RecordingSink sink;
    _sink = &sink;
    _backend.set_response(
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n" +
        sse_chunk_encode(events, 4096));
    start(0, true, TranslateMode::None, GetParam());
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(openai_stream_request_with_usage()));
    (void)c.recv_all();
    c.close();
    shutdown();

    const auto recs = sink.records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].r.tokens_in, 11);
    EXPECT_EQ(recs[0].r.tokens_out, 5);
    EXPECT_EQ(recs[0].r.cached_tokens, 7);
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyForwardStream,
                         ::testing::Values(llmbridge::IoBackend::Epoll,
                                           llmbridge::IoBackend::Uring),
                         [](const auto& i) {
                             return i.param == llmbridge::IoBackend::Epoll ? "Epoll" : "Uring";
                         });

// ── Bedrock: signed, per request, over the bytes we actually send ──────────────
//
// The signature ARITHMETIC is proven in net_sigv4_test against AWS's published
// vectors. What is proven here is the wiring: that the model reaches the path, the
// body loses it, the credential is consumed and not forwarded, and both event loops
// behave identically.
class ProxyBedrock : public ProxyIT,
                     public ::testing::WithParamInterface<llmbridge::IoBackend>
{
  protected:
    static constexpr std::string_view kHost = "bedrock-runtime.us-east-1.amazonaws.com";

    void start_bedrock(const std::string& host = std::string(kHost))
    {
        _backend.start();
        _backend.set_response(http_ok(anthropic_resp_body("pong")));
        _upstreams.push_back(llmbridge::Upstream{.ip = "127.0.0.1",
                                                 .port = _backend.port(),
                                                 .sni_host = host,
                                                 .translate = TranslateMode::Bedrock});
        start(0, false, TranslateMode::None, GetParam());
    }

    /// An OpenAI request naming a versioned Bedrock model, with AWS credentials in
    /// the bearer slot the BYOK path already carries.
    static std::string request(std::string_view bearer)
    {
        const std::string body =
            R"({"model":"anthropic.claude-3-5-sonnet-20240620-v1:0",)"
            R"("messages":[{"role":"user","content":"ping"}]})";
        std::string r = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n";
        if (!bearer.empty())
        {
            r += "Authorization: Bearer ";
            r += bearer;
            r += "\r\n";
        }
        r += "Content-Type: application/json\r\nContent-Length: ";
        r += std::to_string(body.size());
        r += "\r\n\r\n";
        r += body;
        return r;
    }
};

// Signing needs OpenSSL, so the four tests that expect a SIGNED request only mean
// anything in a TLS build. The dependency-free build gets its own case below, which
// asserts the behaviour that matters there: refuse, never send unsigned.
#ifdef LLMBRIDGE_HAVE_TLS

TEST_P(ProxyBedrock, ModelMovesIntoThePathAndOutOfTheBody)
{
    start_bedrock();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(request("AKIDEXAMPLE:secret")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();

    // The colon is percent-encoded on the wire; the canonical form signed over it
    // carries %253A, which net_sigv4_test pins.
    EXPECT_EQ(up.find("POST /model/anthropic.claude-3-5-sonnet-20240620-v1%3A0/invoke"
                      " HTTP/1.1\r\n"),
              0u)
        << up;
    const std::string body = body_of(up);
    EXPECT_EQ(body.find("\"model\""), std::string::npos) << body;
    EXPECT_NE(body.find(R"("anthropic_version":"bedrock-2023-05-31")"), std::string::npos)
        << body;
}

TEST_P(ProxyBedrock, SignsWithTheDerivedRegionAndNeverForwardsTheSecret)
{
    start_bedrock();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(request("AKIDEXAMPLE:supersecretvalue")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();

    EXPECT_NE(up.find("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/"), std::string::npos) << up;
    // Region derived from the endpoint name, service pinned: `bedrock`, which does
    // NOT follow the `bedrock-runtime` hostname.
    EXPECT_NE(up.find("/us-east-1/bedrock/aws4_request"), std::string::npos) << up;
    EXPECT_NE(up.find("SignedHeaders=content-type;host;x-amz-date"), std::string::npos) << up;
    EXPECT_NE(up.find("\r\nx-amz-date: "), std::string::npos) << up;

    // The client's credential is CONSUMED. Neither the secret nor the bearer form may
    // appear anywhere in the bytes we send.
    EXPECT_EQ(up.find("supersecretvalue"), std::string::npos) << up;
    EXPECT_EQ(up.find("Bearer "), std::string::npos) << up;
    // Host names the venue, and the signature covers that same value. The test
    // backend is on an ephemeral port, so the header carries `host:port`, which is
    // exactly what host_header_for produces and therefore what got signed.
    EXPECT_NE(up.find(std::string("\r\nHost: ") + std::string(kHost)), std::string::npos)
        << up;
}

TEST_P(ProxyBedrock, ContentLengthMatchesTheRewrittenBody)
{
    // The body is rewritten, so a stale length is a framing desync on a POOLED
    // upstream: the tail of one request becomes the head of the next client's.
    start_bedrock();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(request("AKIDEXAMPLE:secret")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();

    llmbridge::net::http::Message m;
    ASSERT_EQ(llmbridge::net::http::parse_request(up, m),
              llmbridge::net::http::FrameStatus::Complete);
    EXPECT_EQ(m.body_len, up.size() - m.header_len) << up;
    EXPECT_EQ(m.body_len, body_of(up).size());
}

TEST_P(ProxyBedrock, SessionTokenIsSignedAndSentInItsOwnHeader)
{
    start_bedrock();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(request("AKIDEXAMPLE:secret:SESSIONTOKENVALUE")));
    (void)c.recv_response();
    const std::string up = _backend.last_request();

    EXPECT_NE(up.find("\r\nx-amz-security-token: SESSIONTOKENVALUE\r\n"), std::string::npos)
        << up;
    EXPECT_NE(up.find("SignedHeaders=content-type;host;x-amz-date;x-amz-security-token"),
              std::string::npos)
        << up;
}

#else  // no TLS compiled in

TEST_P(ProxyBedrock, WithoutTlsEveryRequestIsRefusedAndNothingIsSent)
{
    // A build with no OpenSSL cannot sign, and Bedrock is HTTPS-only anyway. What
    // must NOT happen is the request going out unsigned with the customer's AWS
    // secret on the wire for a call that was always going to be rejected.
    start_bedrock();
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(request("AKIDEXAMPLE:secret")));
    EXPECT_NE(c.recv_response().find("400"), std::string::npos);
    EXPECT_TRUE(_backend.last_request().empty()) << _backend.last_request();
}

#endif // LLMBRIDGE_HAVE_TLS

TEST_P(ProxyBedrock, RefusesRatherThanSendingUnsigned)
{
    // Each of these must produce a 400 with NOTHING reaching the upstream. An
    // unsigned request would be rejected by AWS anyway, but a request that leaves
    // here with a half-formed credential is a secret on the wire for no purpose.
    start_bedrock();
    for (const std::string_view bearer : {"", "AKIDEXAMPLE", "AKIDEXAMPLE:", ":secret"})
    {
        _backend.clear_last_request();
        Client c;
        ASSERT_TRUE(c.connect(_proxy_port));
        ASSERT_TRUE(c.send(request(bearer)));
        const std::string resp = c.recv_response();
        EXPECT_NE(resp.find("400"), std::string::npos) << "bearer=[" << bearer << "]";
        EXPECT_TRUE(_backend.last_request().empty())
            << "sent upstream for bearer=[" << bearer << "]: " << _backend.last_request();
        c.close();
    }
}

TEST_P(ProxyBedrock, AnEndpointWithNoRegionInItsNameRefusesEveryRequest)
{
    // Signing with a guessed region returns a 403 whose body says nothing, so an
    // underivable region is a refusal here instead of a mystery there.
    start_bedrock("bedrock.example.com");
    Client c;
    ASSERT_TRUE(c.connect(_proxy_port));
    ASSERT_TRUE(c.send(request("AKIDEXAMPLE:secret")));
    EXPECT_NE(c.recv_response().find("400"), std::string::npos);
    EXPECT_TRUE(_backend.last_request().empty()) << _backend.last_request();
}

INSTANTIATE_TEST_SUITE_P(Backends, ProxyBedrock,
                         ::testing::Values(llmbridge::IoBackend::Epoll,
                                           llmbridge::IoBackend::Uring),
                         [](const auto& i) {
                             return i.param == llmbridge::IoBackend::Epoll ? "Epoll" : "Uring";
                         });
