// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "gateway/gateway.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include "net/socket_util.hpp"
#include "net/uring.hpp" // self-guarded by LLMBRIDGE_HAVE_URING

namespace llmbridge
{
    namespace
    {
        constexpr size_t kInitialBuf = 4096;
        constexpr int kMaxEvents = 1024;
        constexpr int kPollTickMs = 200; // so request_stop() is observed promptly

        // Build a minimal HTTP/1.1 message (start line + JSON body) for a
        // translated request/response. The benchmark backend ignores path and
        // most headers; a real Anthropic target would add x-api-key /
        // anthropic-version here (same cost class).
        std::string build_http(std::string_view start_line, std::string_view body)
        {
            std::string out;
            out.reserve(start_line.size() + body.size() + 96);
            out.append(start_line);
            out.append("\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: ");
            out.append(std::to_string(body.size()));
            out.append("\r\n\r\n");
            out.append(body);
            return out;
        }

        // Translate an OpenAI request body to the upstream dialect, also yielding
        // the upstream start line. Empty return = malformed body. Shared by both
        // event-loop backends.
        std::string xlate_req(TranslateMode mode, std::string_view body, std::string_view& start_line)
        {
            switch (mode)
            {
                case TranslateMode::Anthropic:
                    start_line = "POST /v1/messages HTTP/1.1";
                    return provider::openai_to_anthropic_request(body);
                case TranslateMode::Gemini:
                    start_line = "POST /v1beta/models/gemini:generateContent HTTP/1.1";
                    return provider::openai_to_gemini_request(body);
                case TranslateMode::Cohere:
                    start_line = "POST /v2/chat HTTP/1.1";
                    return provider::openai_to_cohere_request(body);
                case TranslateMode::None:
                    return {};
            }
            return {};
        }

        // A minimal HTTP error response (Connection: close) so a client sees a real
        // status code instead of a bare TCP reset. 400 = malformed client request;
        // 502 = upstream failure. Body uses the OpenAI error-envelope shape.
        std::string build_error(int code)
        {
            const char* line = code == 400   ? "HTTP/1.1 400 Bad Request"
                               : code == 504 ? "HTTP/1.1 504 Gateway Timeout"
                                             : "HTTP/1.1 502 Bad Gateway";
            const char* type = code == 400   ? "invalid_request_error"
                               : code == 504 ? "timeout_error"
                                             : "upstream_error";
            const char* msg = code == 400   ? "malformed request"
                              : code == 504 ? "upstream timed out"
                                            : "bad gateway: upstream failure";
            std::string body = std::string("{\"error\":{\"message\":\"") + msg + "\",\"type\":\"" + type + "\"}}";
            std::string out;
            out.reserve(body.size() + 128);
            out.append(line);
            out.append("\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ");
            out.append(std::to_string(body.size()));
            out.append("\r\n\r\n");
            out.append(body);
            return out;
        }

        // Client-facing SSE response head. The stream is close-delimited: the body
        // ends when we close the socket, so no client-side chunk framing is needed.
        constexpr std::string_view kSseHead =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n";

        // Build a response that PRESERVES the upstream status code. Used to relay a
        // provider's own failure (429 rate limit, 529 overloaded, 400 context
        // length, 401 auth) to the client instead of flattening it to a gateway
        // 502 — the client needs the real code to decide whether to back off/retry.
        std::string build_http_status(int status, std::string_view reason, std::string_view body)
        {
            std::string out = "HTTP/1.1 " + std::to_string(status) + " ";
            out.append(reason);
            out.append("\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: ");
            out.append(std::to_string(body.size()));
            out.append("\r\n\r\n");
            out.append(body);
            return out;
        }

        // A short reason phrase for the codes providers actually return.
        const char* reason_for(int status)
        {
            switch (status)
            {
                case 400: return "Bad Request";
                case 401: return "Unauthorized";
                case 403: return "Forbidden";
                case 404: return "Not Found";
                case 408: return "Request Timeout";
                case 413: return "Payload Too Large";
                case 429: return "Too Many Requests";
                case 500: return "Internal Server Error";
                case 502: return "Bad Gateway";
                case 503: return "Service Unavailable";
                case 504: return "Gateway Timeout";
                case 529: return "Overloaded";
                default: return status < 500 ? "Client Error" : "Server Error";
            }
        }

        // Outcome of one streaming translate step (shared by both backends).
        enum class StreamStep
        {
            Ok,      // bytes translated (maybe none); stream continues
            Ended,   // upstream signalled end; terminal [DONE] emitted
            Corrupt, // malformed chunked framing — drop WITHOUT a fake clean [DONE]
            Failed   // translator refused (cap tripped / protocol error) — drop
        };

        // The dialect/transport transform shared by the epoll and io_uring pumps:
        // chunk-decode -> SSE-translate -> detect end. Lives in ONE place so a fix
        // (e.g. honouring the translator's cap) can't land on one backend only; the
        // backends keep their own idiomatic DELIVERY (flush+pause vs kick+cap).
        // `in` is the upstream's raw buffer (consumed); `out` receives client SSE.
        StreamStep stream_step(Connection* client, std::string& in, std::string& out, bool at_eof)
        {
            std::string sse_in;
            if (client->stream_chunked)
            {
                const bool ok = client->chunkdec.feed(in, sse_in);
                in.clear();
                if (!ok) return StreamStep::Corrupt; // truncate honestly: no fake [DONE]
            }
            else
            {
                sse_in.swap(in);
                in.clear();
            }

            // Honour the translator's own failure (its DoS caps are sticky): a
            // hostile/broken upstream must tear the stream down, not silently
            // produce nothing while we keep reading it forever.
            if (!sse_in.empty() && !client->sse->feed(sse_in, out)) return StreamStep::Failed;

            const bool ended = (client->stream_chunked && client->chunkdec.done()) || at_eof;
            if (ended && !client->stream_ended)
            {
                if (!client->sse->finish(out)) return StreamStep::Failed;
                client->stream_ended = true;
                return StreamStep::Ended;
            }
            return client->stream_ended ? StreamStep::Ended : StreamStep::Ok;
        }

        // Translate an upstream response body back to the OpenAI shape. Empty = bad.
        std::string xlate_resp(TranslateMode mode, std::string_view body)
        {
            switch (mode)
            {
                case TranslateMode::Anthropic: return provider::anthropic_to_openai_response(body);
                case TranslateMode::Gemini: return provider::gemini_to_openai_response(body);
                case TranslateMode::Cohere: return provider::cohere_to_openai_response(body);
                case TranslateMode::None: return {};
            }
            return {};
        }
    } // namespace

    Gateway::Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                     int64_t warmup_ns, TranslateMode translate, IoBackend io,
                     int64_t upstream_idle_ns)
        : _listen_port(listen_port), _upstream_ip(std::move(upstream_ip)),
          _upstream_port(upstream_port), _warmup_ns(warmup_ns), _translate(translate), _io(io),
          _upstream_idle_ns(upstream_idle_ns)
    {
        // Linux has no SO_NOSIGPIPE; ignore SIGPIPE process-wide so a write to a
        // peer-closed socket returns EPIPE instead of killing us. Idempotent, so
        // safe to set here (covers the daemon and the test harness alike).
        std::signal(SIGPIPE, SIG_IGN);
        _epfd = ::epoll_create1(0);
        if (_epfd < 0) throw std::runtime_error("epoll_create1() failed");
        _listen_fd = net::make_listener(_listen_port);
        if (_listen_fd < 0) throw std::runtime_error("failed to bind listen port");
        _listen_conn = new Connection();
        _listen_conn->fd = _listen_fd;
        ep_add_read(_listen_conn);
        std::fprintf(stderr, "llmbridge: listening :%u -> upstream %s:%u%s\n",
                     _listen_port, _upstream_ip.c_str(), _upstream_port,
                     _translate == TranslateMode::Anthropic ? " (translate: anthropic)" : "");

        // Resolve the event-loop backend: io_uring for Uring/Auto when the kernel
        // supports it, else epoll. Uring requested but unavailable -> epoll.
#ifdef LLMBRIDGE_HAVE_URING
        const bool uring_ok = net::uring::available();
        if (_io == IoBackend::Uring || _io == IoBackend::Auto) _uring_active = uring_ok;
        if (_io == IoBackend::Uring && !uring_ok)
            std::fprintf(stderr, "llmbridge: --io=uring requested but io_uring unavailable; using epoll\n");
#endif
        const char* want = _io == IoBackend::Uring ? "uring" : _io == IoBackend::Epoll ? "epoll" : "auto";
        std::fprintf(stderr, "llmbridge: io=%s — %s loop\n", want, _uring_active ? "io_uring" : "epoll");
    }

    Gateway::~Gateway()
    {
        for (auto& [id, c] : _clients)
        {
            // An in-flight (acquired, not pooled) upstream is reachable only via
            // peer — free it too, or it leaks when we stop mid-request. (The
            // io_uring loop already nulls these during its drain.)
            if (Connection* u = c->peer) { if (u->fd >= 0) ::close(u->fd); delete u; }
            if (c->fd >= 0) ::close(c->fd);
            delete c;
        }
        for (Connection* u : _idle_upstreams) { if (u->fd >= 0) ::close(u->fd); delete u; }
        for (Connection* d : _doomed) delete d;
        if (_listen_fd >= 0) ::close(_listen_fd);
        if (_epfd >= 0) ::close(_epfd);
        delete _listen_conn;
    }

    uint16_t Gateway::bound_port() const noexcept
    {
        if (_listen_fd < 0) return 0;
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) return 0;
        return ntohs(addr.sin_port);
    }

    // epoll is level-triggered here (no EPOLLET): every readable handler first
    // drains the socket into rbuf, so EPOLLIN won't re-fire on unread bytes, and
    // EPOLLIN stays armed for the connection's whole life. Write interest is
    // toggled via EPOLL_CTL_MOD on top of that always-on EPOLLIN.
    void Gateway::ep_add_read(Connection* c) noexcept
    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_ADD, c->fd, &ev);
    }

    void Gateway::ep_arm_write(Connection* c) noexcept
    {
        if (c->write_armed) return;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->write_armed = true;
    }

    void Gateway::ep_disarm_write(Connection* c) noexcept
    {
        if (!c->write_armed) return;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->write_armed = false;
    }

    // Backpressure: pause/resume reading a connection. Used to stop pulling
    // upstream SSE bytes while the client's write buffer is draining, so a slow
    // client can't make us buffer an unbounded stream. (EPOLLHUP/EPOLLERR still
    // fire while paused, so an upstream close is never missed.)
    void Gateway::ep_pause_read(Connection* c) noexcept
    {
        if (c->read_paused) return;
        epoll_event ev{};
        ev.events = c->write_armed ? static_cast<uint32_t>(EPOLLOUT) : 0u;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->read_paused = true;
        ++_stats.stream_pauses; // observability: proves backpressure actually engaged
    }

    void Gateway::ep_resume_read(Connection* c) noexcept
    {
        if (!c->read_paused) return;
        epoll_event ev{};
        ev.events = static_cast<uint32_t>(EPOLLIN) | (c->write_armed ? static_cast<uint32_t>(EPOLLOUT) : 0u);
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->read_paused = false;
    }

    bool Gateway::drain_read(Connection* c) noexcept
    {
        char tmp[16384];
        for (;;)
        {
            ssize_t n = ::read(c->fd, tmp, sizeof(tmp));
            if (n > 0)
            {
                c->rbuf.append(tmp, static_cast<size_t>(n));
                if (static_cast<size_t>(n) < sizeof(tmp)) return true;
                continue;
            }
            if (n == 0) return false;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;
        }
    }

    bool Gateway::pump_write(Connection* c, bool* done) noexcept
    {
        *done = false;
        while (c->woff < c->wbuf.size())
        {
            ssize_t n = ::write(c->fd, c->wbuf.data() + c->woff, c->wbuf.size() - c->woff);
            if (n > 0) { c->woff += static_cast<size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        // Leave wbuf intact on completion: the request stays available for a
        // stale-connection resend until the response is read; callers clear it.
        *done = true;
        return true;
    }

    Connection* Gateway::acquire_upstream() noexcept
    {
        if (!_idle_upstreams.empty())
        {
            Connection* u = _idle_upstreams.back();
            _idle_upstreams.pop_back();
            u->from_pool = true; // reused -> a pre-response failure is retry-eligible
            u->retried = false;  // fresh request: one retry available again
            return u;
        }
        int fd = net::start_connect(_upstream_ip.c_str(), _upstream_port);
        if (fd < 0) return nullptr;
        Connection* u = new Connection();
        u->fd = fd;
        u->is_client = false;
        u->from_pool = false;
        u->rbuf.reserve(kInitialBuf);
        ep_add_read(u);
        ep_arm_write(u); // learn when the non-blocking connect completes
        ++_stats.upstream_conns_opened;
        return u;
    }

    bool Gateway::retry_upstream(Connection* u) noexcept
    {
        // Stale pooled connection: reused from the keep-alive pool, not yet retried,
        // and failed before sending any response — the provider almost certainly
        // dropped it idle without processing. Resend the request once on a fresh
        // connection rather than failing the client. (Same rule as the io_uring path.)
        if (!u->from_pool || u->retried || !u->rbuf.empty()) return false;
        Connection* client = u->peer;
        if (!client) return false;
        int fd = net::start_connect(_upstream_ip.c_str(), _upstream_port);
        if (fd < 0) return false;

        Connection* uf = new Connection();
        uf->fd = fd;
        uf->is_client = false;
        uf->from_pool = false;
        uf->retried = true; // this request's one allowed retry is now spent
        uf->wbuf = std::move(u->wbuf);
        uf->woff = 0;
        uf->rbuf.reserve(kInitialBuf);
        ep_add_read(uf);
        ep_arm_write(uf); // learn when connect completes, then send the request
        ++_stats.upstream_conns_opened;
        ++_stats.upstream_retries;

        u->peer = nullptr;
        close_upstream(u); // discard the dead connection
        client->peer = uf;
        uf->peer = client;
        return true;
    }

    void Gateway::release_upstream(Connection* u) noexcept
    {
        u->peer = nullptr;
        u->rbuf.clear();
        u->wbuf.clear();
        u->woff = 0;
        u->msg = http::Message{};
        ep_disarm_write(u);
        _idle_upstreams.push_back(u);
    }

    // close_* defer the free to the end of the epoll batch: an earlier event in
    // the same batch can close a conn that a later event still references via
    // data.ptr (e.g. a peer aborted while its own fd is also ready), so freeing
    // inline would dangle that pointer. Events for doomed conns are skipped in
    // run().
    void Gateway::close_client(Connection* c) noexcept
    {
        if (c->doomed) return;
        if (c->id) _clients.erase(c->id);
        if (c->fd >= 0) { ::close(c->fd); c->fd = -1; }
        c->doomed = true;
        _doomed.push_back(c);
    }

    void Gateway::close_upstream(Connection* u) noexcept
    {
        if (u->doomed) return;
        for (auto it = _idle_upstreams.begin(); it != _idle_upstreams.end(); ++it)
            if (*it == u) { _idle_upstreams.erase(it); break; }
        if (u->fd >= 0) { ::close(u->fd); u->fd = -1; }
        u->doomed = true;
        _doomed.push_back(u);
    }

    void Gateway::abort_pair(Connection* client) noexcept
    {
        Connection* u = client->peer;
        if (u) { u->peer = nullptr; close_upstream(u); }
        close_client(client);
        ++_stats.errors;
    }

    void Gateway::error_respond(Connection* client, int code) noexcept
    {
        if (client->doomed) return;
        // We're replying to the client ourselves — drop any in-flight upstream.
        if (Connection* u = client->peer) { client->peer = nullptr; u->peer = nullptr; close_upstream(u); }
        client->wbuf = build_error(code);
        client->woff = 0;
        client->close_after_resp = true; // finish_client_response closes once it flushes
        ++_stats.errors;
        respond(client);
    }

    void Gateway::on_accept() noexcept
    {
        for (;;)
        {
            int fd = ::accept(_listen_fd, nullptr, nullptr);
            if (fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                return;
            }
            net::set_nonblocking(fd);
            net::set_nodelay(fd);
            net::set_nosigpipe(fd);
            Connection* c = new Connection();
            c->fd = fd;
            c->is_client = true;
            c->id = _next_client_id++;
            c->rbuf.reserve(kInitialBuf);
            _clients[c->id] = c;
            ep_add_read(c);
        }
    }

    void Gateway::on_client_readable(Connection* c) noexcept
    {
        if (!drain_read(c))
        {
            // EOF/error: mid-request close is a real abort; idle close is normal.
            const bool in_flight = c->peer != nullptr || !c->wbuf.empty();
            if (in_flight) abort_pair(c);
            else close_client(c);
            return;
        }
        // One request in flight at a time per client.
        if (c->peer != nullptr || !c->wbuf.empty()) return;
        if (c->rbuf.empty()) return;

        // Stamp just before framing so the (completing) HTTP parse is counted in
        // the request-path overhead. On a partial read parse returns NeedMore and
        // we discard t0 and return, so inter-packet network wait is never charged
        // to the gateway.
        const int64_t t0 = now_ns();
        http::Message m;
        auto st = http::parse(c->rbuf, m);
        if (st == http::ParseStatus::NeedMore) return;
        if (st == http::ParseStatus::Error) { error_respond(c, 400); return; }

        c->msg = m;
        c->ts_req_recvd = t0;
        forward(c);
    }

    void Gateway::forward(Connection* c) noexcept
    {
        // Build the bytes to send upstream (translate first, before acquiring an
        // upstream, so a bad body can't leak a pooled connection).
        std::string upstream_bytes;
        if (_translate != TranslateMode::None)
        {
            std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
            std::string_view start_line;
            std::string tbody = xlate_req(_translate, body, start_line);
            if (tbody.empty()) { error_respond(c, 400); return; }
            // Remember whether the client asked for a final usage chunk — the
            // request bytes are consumed below, but the stream needs it later.
            c->wants_usage = provider::openai_wants_stream_usage(body);
            upstream_bytes = build_http(start_line, tbody);
        }
        else
        {
            upstream_bytes.assign(c->rbuf.data(), c->msg.total_len);
        }

        Connection* u = acquire_upstream();
        if (!u) { error_respond(c, 502); return; }

        u->wbuf = std::move(upstream_bytes);
        u->woff = 0;
        c->rbuf.erase(0, c->msg.total_len);
        c->peer = u;
        u->peer = c;
        c->ts_up_activity = now_ns(); // idle-timeout baseline for this request

        // Optimistic send: if the pooled upstream is already connected (the common
        // case), write immediately and only arm EPOLLOUT if the socket buffer is
        // full. Arming unconditionally costs two epoll_ctl calls + an extra wakeup
        // per request; the client-response leg already writes this way.
        if (u->connected)
        {
            bool done = false;
            if (!pump_write(u, &done)) { if (!retry_upstream(u)) error_respond(c, 502); return; }
            if (done) c->ts_up_sent = now_ns(); // request fully sent (end of request path)
            else ep_arm_write(u);               // socket full; finish on writability
        }
        else
        {
            ep_arm_write(u); // connect pending; on_upstream_writable sends once it completes
        }
    }

    void Gateway::on_upstream_writable(Connection* u) noexcept
    {
        if (!u->connected)
        {
            int err = net::connect_result(u->fd);
            if (err != 0)
            {
                Connection* client = u->peer;
                u->peer = nullptr;
                close_upstream(u);
                if (client) { client->peer = nullptr; error_respond(client, 502); }
                else ++_stats.errors;
                return;
            }
            u->connected = true;
        }
        bool done = false;
        if (!pump_write(u, &done))
        {
            if (u->peer) { if (!retry_upstream(u)) error_respond(u->peer, 502); }
            else close_upstream(u);
            return;
        }
        if (!done) { ep_arm_write(u); return; }
        ep_disarm_write(u);
        if (u->peer) u->peer->ts_up_sent = now_ns(); // end of request-path work
    }

    void Gateway::on_upstream_readable(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!drain_read(u))
        {
            if (client && client->streaming) { stream_on_upstream_eof(u); return; }
            if (client == nullptr) close_upstream(u); // idle pooled conn dropped (eviction)
            else if (!retry_upstream(u)) error_respond(client, 502);
            return;
        }
        if (client == nullptr) return; // stray bytes on an idle pooled conn
        client->ts_up_activity = now_ns(); // upstream made progress

        // Mid-stream: pump the newly-arrived body bytes and return.
        if (client->streaming) { stream_pump(u); return; }

        // First response bytes: for the Anthropic translate path, peek the head to
        // decide whole-body vs streaming (text/event-stream). Other modes and
        // non-streaming responses fall through to the whole-body path unchanged.
        if (_translate == TranslateMode::Anthropic)
        {
            http::ResponseHead h;
            const auto hs = http::parse_response_head(u->rbuf, h);
            if (hs == http::HeadStatus::NeedMore) return;
            if (hs == http::HeadStatus::Error) { error_respond(client, 502); return; }
            // Only a 200 carries a real event stream. A provider error (429 rate
            // limit, 529 overloaded, 400 context length, 401 auth) must reach the
            // client with ITS status — relayed below once the body is framed —
            // never laundered into a 200 stream.
            if (h.event_stream && h.status == 200) { begin_stream(u, h); return; }
        }

        // Stamp just before framing so the response HTTP parse is counted in the
        // response-path overhead, without charging inter-packet network wait.
        const int64_t t0 = now_ns();
        http::Message m;
        auto st = http::parse(u->rbuf, m);
        if (st == http::ParseStatus::NeedMore) return;
        if (st == http::ParseStatus::Error) { error_respond(client, 502); return; }

        client->ts_up_recvd = t0; // end of upstream wait (stamped pre-framing)

        if (_translate != TranslateMode::None)
        {
            std::string_view body(u->rbuf.data() + m.header_len, m.body_len);
            // Relay a provider failure with ITS OWN status + message (rate limit,
            // overloaded GPU, context length, auth) — translating a non-200 body as
            // if it were a completion would fail and mask it as a generic 502.
            http::ResponseHead h;
            const bool have_head = http::parse_response_head(u->rbuf, h) == http::HeadStatus::Ok;
            if (have_head && h.status != 0 && h.status != 200)
            {
                client->wbuf = build_http_status(
                    h.status, reason_for(h.status),
                    provider::upstream_error_to_openai(body, "upstream_error"));
                client->woff = 0;
                client->peer = nullptr;
                if (m.keep_alive) release_upstream(u); else close_upstream(u);
                ++_stats.errors;
                respond(client);
                return;
            }
            std::string tbody = xlate_resp(_translate, body);
            if (tbody.empty())
            {
                client->peer = nullptr;
                release_upstream(u); // framing was valid; the upstream conn is reusable
                error_respond(client, 502);
                return;
            }
            client->wbuf = build_http("HTTP/1.1 200 OK", tbody);
        }
        else
        {
            client->wbuf.assign(u->rbuf.data(), m.total_len);
        }
        client->woff = 0;

        // Response fully read -> upstream is free. Pool it only if it will stay
        // open (response keep-alive, and for passthrough the client didn't ask to
        // close); otherwise it's about to close, so drop it rather than reuse a
        // stale connection.
        const bool pool_upstream =
            m.keep_alive && (_translate != TranslateMode::None || client->msg.keep_alive);
        client->peer = nullptr;
        if (pool_upstream) release_upstream(u);
        else close_upstream(u);
        respond(client);
    }

    void Gateway::respond(Connection* c) noexcept
    {
        bool done = false;
        if (!pump_write(c, &done)) { close_client(c); ++_stats.errors; return; }
        if (!done) { ep_arm_write(c); return; } // socket full; finish on writability
        finish_client_response(c);
    }

    void Gateway::on_client_writable(Connection* c) noexcept
    {
        if (c->streaming) { stream_flush(c); return; } // pump path has its own drain logic
        bool done = false;
        if (!pump_write(c, &done)) { abort_pair(c); return; }
        if (!done) { ep_arm_write(c); return; }
        finish_client_response(c);
    }

    void Gateway::finish_client_response(Connection* c) noexcept
    {
        // Error replies (close_after_resp) are counted in _stats.errors, not the
        // latency histograms — their timing stamps are unset and would be garbage.
        if (!c->close_after_resp)
        {
            const int64_t ts_resp_sent = now_ns();
            if (ts_resp_sent - _t_start >= _warmup_ns)
            {
                const int64_t req_ns = c->ts_up_sent - c->ts_req_recvd;
                const int64_t resp_ns = ts_resp_sent - c->ts_up_recvd;
                if (req_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(req_ns));
                if (resp_ns >= 0) _stats.resp_path.record(static_cast<uint64_t>(resp_ns));
                if (req_ns >= 0 && resp_ns >= 0)
                    _stats.overhead.record(static_cast<uint64_t>(req_ns + resp_ns));
                ++_stats.requests;
            }
        }
        ep_disarm_write(c);
        c->wbuf.clear(); // response fully sent; pump_write no longer clears it for us
        c->woff = 0;
        const bool close_now = c->close_after_resp || !c->msg.keep_alive;
        c->msg = http::Message{};
        if (close_now) { close_client(c); return; }
        on_client_readable(c); // service any pipelined next request already in rbuf
    }

    // ── Streaming pump (epoll, Anthropic->OpenAI SSE) ───────────────────────
    // Enter streaming: the upstream response is text/event-stream. Send the
    // client SSE headers (close-delimited) and translate the body as it arrives.
    void Gateway::begin_stream(Connection* u, const http::ResponseHead& h) noexcept
    {
        Connection* client = u->peer;
        client->streaming = true;
        client->up_head_done = true;
        client->stream_chunked = h.chunked;
        client->sse = std::make_unique<provider::AnthropicToOpenAiSse>(-1, client->wants_usage);
        client->wbuf.assign(kSseHead);
        client->woff = 0;

        u->rbuf.erase(0, h.header_len); // consume the head; the rest is body
        stream_pump(u);                 // translate any initial body + flush headers
    }

    // Decode + translate the upstream body bytes now sitting in u->rbuf, appending
    // OpenAI SSE to the client's write buffer, then flush.
    void Gateway::stream_pump(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { close_upstream(u); return; } // lost peer mid-stream

        const StreamStep st = stream_step(client, u->rbuf, client->wbuf, /*at_eof=*/false);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            // Truncate honestly: flush what we already translated, then close
            // WITHOUT a terminal [DONE] so the client sees an aborted stream
            // rather than a fabricated clean finish.
            client->stream_ended = true;
            client->close_after_resp = true;
            ++_stats.errors;
            stream_flush(client);
            return;
        }
        stream_flush(client);
    }

    // Upstream closed the connection: translate whatever remains, emit the terminal
    // [DONE] if we haven't, and finalize.
    void Gateway::stream_on_upstream_eof(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { close_upstream(u); return; }

        const StreamStep st = stream_step(client, u->rbuf, client->wbuf, /*at_eof=*/true);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            client->stream_ended = true;
            client->close_after_resp = true;
            ++_stats.errors;
        }
        stream_flush(client);
    }

    // Write buffered SSE to the client. If it doesn't all go, finish on writability
    // and pause upstream reads (backpressure). On full flush, resume upstream — or
    // finalize if the stream has ended.
    void Gateway::stream_flush(Connection* client) noexcept
    {
        bool done = false;
        if (!pump_write(client, &done)) { abort_pair(client); return; } // client gone
        if (!done)
        {
            ep_arm_write(client);
            if (client->peer && !client->peer->doomed) ep_pause_read(client->peer);
            return;
        }
        client->wbuf.clear();
        client->woff = 0;
        ep_disarm_write(client);
        if (client->stream_ended) { finalize_stream(client); return; }
        if (client->peer && !client->peer->doomed) ep_resume_read(client->peer);
    }

    // Stream complete: drop the upstream (a just-streamed conn isn't pooled),
    // count the request, and close the client (which delimits the SSE body).
    void Gateway::finalize_stream(Connection* client) noexcept
    {
        if (Connection* u = client->peer) { client->peer = nullptr; u->peer = nullptr; close_upstream(u); }
        // Only a stream that terminated cleanly counts as a served request; an
        // aborted one (close_after_resp) was already counted in _stats.errors.
        if (!client->close_after_resp) ++_stats.requests; // latency histograms N/A
        close_client(client);
    }

    // Abort requests whose upstream has gone silent. Runs on the loop's existing
    // periodic tick, so an idle gateway costs one cheap scan per tick. A client
    // that hasn't been answered yet gets a real 504; a live stream (headers already
    // sent) is closed WITHOUT a terminal [DONE], so the client sees a truncated
    // stream rather than a fabricated clean finish.
    void Gateway::sweep_idle(bool uring) noexcept
    {
        if (_upstream_idle_ns <= 0) return;
        const int64_t now = now_ns();
        if (now - _last_sweep_ns < 50'000'000LL) return; // at most ~20 sweeps/sec
        _last_sweep_ns = now;

        // Collect first: the teardown below erases from _clients.
        std::vector<Connection*> stale;
        for (auto& [id, c] : _clients)
        {
            if (c->doomed) continue;
            const bool in_flight = c->peer != nullptr || c->streaming;
            if (!in_flight || c->ts_up_activity == 0) continue;
            if (now - c->ts_up_activity > _upstream_idle_ns) stale.push_back(c);
        }
        for (Connection* c : stale)
        {
            ++_stats.upstream_timeouts;
            const bool streaming = c->streaming;
            if (streaming)
            {
                // Response headers are already out; truncate honestly (no [DONE]).
                c->stream_ended = true;
                c->close_after_resp = true;
                ++_stats.errors;
            }
#ifdef LLMBRIDGE_HAVE_URING
            if (uring)
            {
                if (streaming) u_abort_pair(c);
                else u_error_respond(c, 504);
                continue;
            }
#else
            (void)uring;
#endif
            if (streaming) abort_pair(c);
            else error_respond(c, 504);
        }
    }

    int Gateway::run()
    {
#ifdef LLMBRIDGE_HAVE_URING
        if (_uring_active) return run_uring();
#endif
        return run_epoll();
    }

    int Gateway::run_epoll()
    {
        _t_start = now_ns();
        epoll_event events[kMaxEvents];

        while (!_stop)
        {
            // kPollTickMs timeout so request_stop() is observed within a tick.
            int n = ::epoll_wait(_epfd, events, kMaxEvents, kPollTickMs);
            if (n < 0) { if (errno == EINTR) continue; break; }
            for (int i = 0; i < n; ++i)
            {
                Connection* c = static_cast<Connection*>(events[i].data.ptr);
                if (c == _listen_conn) { on_accept(); continue; }
                if (c->doomed) continue; // freed earlier this batch
                // Unlike kqueue, epoll coalesces a fd's readiness into one entry,
                // so a single event can carry both EPOLLIN and EPOLLOUT. Error/
                // hangup conditions fold into the readable path (drain_read then
                // reports the EOF/error). Re-check doomed between the two halves.
                const uint32_t e = events[i].events;
                const bool readable = e & (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP);
                const bool writable = e & EPOLLOUT;
                if (c->is_client)
                {
                    if (readable) on_client_readable(c);
                    if (writable && !c->doomed) on_client_writable(c);
                }
                else
                {
                    if (writable) on_upstream_writable(c);
                    if (readable && !c->doomed) on_upstream_readable(c);
                }
            }
            sweep_idle(/*uring=*/false); // abort requests whose upstream went silent
            for (Connection* d : _doomed) delete d;
            _doomed.clear();
        }
        return 0;
    }

#ifdef LLMBRIDGE_HAVE_URING
    // ════════════════════════════════════════════════════════════════════════
    // io_uring backend (Phase 1) — a completion-driven mirror of the epoll loop.
    // Each request advances a small per-connection state machine: every CQE says
    // "this op finished with N bytes," we act, and submit the next op. A conn is
    // freed only when its `inflight` SQEs have all completed (no use-after-free on
    // a completion that lands after we close).
    // ════════════════════════════════════════════════════════════════════════
    namespace
    {
        enum UOp : uint64_t { UAccept = 0, URecv = 1, USend = 2, UConnect = 3, UTimer = 4, UCancel = 5 };
        constexpr uint64_t kTagMask = 7;
        inline uint64_t make_ud(Connection* c, UOp op) { return reinterpret_cast<uintptr_t>(c) | op; }
        inline Connection* ud_conn(uint64_t d) { return reinterpret_cast<Connection*>(d & ~kTagMask); }
        inline UOp ud_op(uint64_t d) { return static_cast<UOp>(d & kTagMask); }

        constexpr unsigned kRingDepth = 4096;
        // Provided-buffer pool for multishot recv: plenty so a connection always has
        // a buffer to land in (we recycle each immediately after copying it out).
        constexpr unsigned kBufGroup = 1;
        constexpr unsigned kBufCount = 4096; // power of two
        constexpr unsigned kBufSize = 4096;

        // Cap on buffered SSE output for one stream. SSE is model-rate-limited, so
        // this only trips for a pathologically slow client — rather than buffer
        // without bound we drop that stream. (The epoll pump instead pauses reads;
        // for a model-rate stream the cap is equivalent in practice.)
        constexpr size_t kStreamBufCap = 8 << 20; // 8 MiB
    } // namespace

    bool Gateway::u_next_sqe(io_uring_sqe** out) noexcept
    {
        io_uring_sqe* s = _ring.get_sqe();
        if (!s) { _ring.submit(); s = _ring.get_sqe(); } // SQ full: flush, retry once
        *out = s;
        return s != nullptr;
    }

    void Gateway::u_submit_accept() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!u_next_sqe(&s)) return;
        s->opcode = IORING_OP_ACCEPT;
        s->fd = _listen_fd;
        s->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
        s->ioprio = IORING_ACCEPT_MULTISHOT; // one SQE, a completion per accepted fd
        s->user_data = make_ud(_listen_conn, UAccept);
    }

    void Gateway::u_submit_timer() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!u_next_sqe(&s)) return;
        s->opcode = IORING_OP_TIMEOUT;
        s->addr = reinterpret_cast<uint64_t>(&_uring_ts);
        s->len = 1;
        s->user_data = UTimer; // conn = nullptr
    }

    bool Gateway::u_arm_recv(Connection* c) noexcept
    {
        // Multishot recv drawing from the provided-buffer pool: one submission keeps
        // delivering a completion per data arrival (each naming the buffer it used),
        // so we never re-submit a recv per read. Armed once per connection; only
        // re-armed if the kernel ends the multishot (e.g. pool exhaustion).
        io_uring_sqe* s = nullptr;
        if (!u_next_sqe(&s)) { u_close(c); return false; }
        s->opcode = IORING_OP_RECV;
        s->fd = c->fd;
        s->addr = 0;
        s->len = 0;
        s->flags |= IOSQE_BUFFER_SELECT;
        s->buf_group = kBufGroup;
        s->ioprio |= IORING_RECV_MULTISHOT;
        s->user_data = make_ud(c, URecv);
        ++c->inflight;
        ++_uring_inflight;
        return true;
    }

    bool Gateway::u_submit_send(Connection* c) noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!u_next_sqe(&s)) { u_close(c); return false; }
        s->opcode = IORING_OP_SEND;
        s->fd = c->fd;
        s->addr = reinterpret_cast<uint64_t>(c->wbuf.data() + c->woff);
        s->len = static_cast<unsigned>(c->wbuf.size() - c->woff);
        s->user_data = make_ud(c, USend);
        ++c->inflight;
        ++_uring_inflight;
        return true;
    }

    bool Gateway::u_submit_connect(Connection* u) noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!u_next_sqe(&s)) { u_abort_pair(u->peer); return false; }
        s->opcode = IORING_OP_CONNECT;
        s->fd = u->fd;
        s->addr = reinterpret_cast<uint64_t>(&_upstream_addr);
        s->off = sizeof(_upstream_addr); // connect addrlen rides in `off`
        s->user_data = make_ud(u, UConnect);
        ++u->inflight;
        ++_uring_inflight;
        return true;
    }

    void Gateway::u_submit_cancel(int fd) noexcept
    {
        // Cancel every in-flight op on `fd` (notably the armed multishot recv, which
        // shutdown() does NOT terminate). The cancelled ops complete with -ECANCELED,
        // releasing their inflight slots so the conn can be freed / the drain finishes.
        io_uring_sqe* s = nullptr;
        if (!u_next_sqe(&s)) return;
        s->opcode = IORING_OP_ASYNC_CANCEL;
        s->fd = fd;
        s->cancel_flags = IORING_ASYNC_CANCEL_FD;
        s->user_data = UCancel; // sentinel: not inflight-counted, completion ignored
    }

    Connection* Gateway::u_acquire_upstream() noexcept
    {
        if (!_idle_upstreams.empty())
        {
            Connection* u = _idle_upstreams.back();
            _idle_upstreams.pop_back();
            u->from_pool = true; // reused -> a pre-response failure is retry-eligible
            u->retried = false;  // fresh request: one retry available again
            return u;
        }
        const int fd = net::make_client_socket();
        if (fd < 0) return nullptr;
        Connection* u = new Connection();
        u->fd = fd;
        u->is_client = false;
        u->connected = false;
        u->from_pool = false;
        u->rbuf.reserve(kInitialBuf);
        ++_stats.upstream_conns_opened;
        return u;
    }

    bool Gateway::u_retry_upstream(Connection* u) noexcept
    {
        // Only safe to resend when the upstream was a pooled reuse, hasn't been
        // retried, and gave us ZERO response bytes — i.e. the provider closed the
        // idle keep-alive connection without processing the request. (Industry
        // convention: retry an idempotent-or-idle-reused request that failed before
        // any response; don't retry once a partial response has been seen.)
        if (!u->from_pool || u->retried || !u->rbuf.empty()) return false;
        Connection* client = u->peer;
        if (!client) return false;
        const int fd = net::make_client_socket();
        if (fd < 0) return false;

        Connection* uf = new Connection();
        uf->fd = fd;
        uf->is_client = false;
        uf->connected = false;
        uf->from_pool = false;
        uf->retried = true; // this request's one allowed retry is now spent
        uf->wbuf = std::move(u->wbuf); // the request bytes, kept intact for resend
        uf->woff = 0;
        uf->rbuf.reserve(kInitialBuf);
        ++_stats.upstream_conns_opened;

        u->peer = nullptr;
        u_close(u); // discard the dead pooled connection
        client->peer = uf;
        uf->peer = client;
        ++_stats.upstream_retries;
        u_submit_connect(uf); // connect fresh, then send on completion
        return true;
    }

    void Gateway::u_release_upstream(Connection* u) noexcept
    {
        u->peer = nullptr;
        u->rbuf.clear();
        u->wbuf.clear();
        u->woff = 0;
        u->msg = http::Message{};
        _idle_upstreams.push_back(u);
    }

    void Gateway::u_close(Connection* c) noexcept
    {
        if (c->doomed) return;
        if (c->is_client && c->id) _clients.erase(c->id);
        for (auto it = _idle_upstreams.begin(); it != _idle_upstreams.end(); ++it)
            if (*it == c) { _idle_upstreams.erase(it); break; }
        // Force any in-flight op on this fd to complete so its inflight count drains
        // (the fd is closed at free time). shutdown alone does NOT end an armed
        // multishot recv, so also cancel everything on the fd.
        if (c->fd >= 0) { ::shutdown(c->fd, SHUT_RDWR); u_submit_cancel(c->fd); }
        c->doomed = true;
        _doomed.push_back(c);
        u_maybe_free(c);
    }

    void Gateway::u_abort_pair(Connection* client) noexcept
    {
        if (!client) return;
        if (Connection* u = client->peer) { u->peer = nullptr; u_close(u); }
        u_close(client);
        ++_stats.errors;
    }

    void Gateway::u_error_respond(Connection* client, int code) noexcept
    {
        if (!client || client->doomed) return;
        if (Connection* u = client->peer) { client->peer = nullptr; u->peer = nullptr; u_close(u); }
        client->wbuf = build_error(code);
        client->woff = 0;
        client->close_after_resp = true; // u_finish_client closes once the reply flushes
        ++_stats.errors;
        u_submit_send(client);
    }

    void Gateway::u_maybe_free(Connection* c) noexcept
    {
        if (!c->doomed || c->inflight > 0) return; // a completion still references it
        if (c->fd >= 0) { ::close(c->fd); c->fd = -1; }
        for (auto it = _doomed.begin(); it != _doomed.end(); ++it)
            if (*it == c) { _doomed.erase(it); break; }
        delete c;
    }

    void Gateway::u_on_cqe(uint64_t user_data, int res, uint32_t flags) noexcept
    {
        const UOp op = ud_op(user_data);
        if (op == UTimer)
        {
            if (!_draining && !_stop) { sweep_idle(/*uring=*/true); u_submit_timer(); }
            return;
        }
        if (op == UCancel) return; // control op (cancel-by-fd); not inflight-counted
        if (op == UAccept) { u_on_accept(res, flags); return; } // not inflight-counted

        Connection* c = ud_conn(user_data);
        // A multishot recv stays armed only while it keeps delivering data (F_MORE
        // set AND res > 0). Its terminal completion (EOF/error, res <= 0) can still
        // carry F_MORE, so it must release the inflight slot — otherwise the drain
        // never reaches zero. Only the consuming (terminal) completion decrements.
        const bool armed = (op == URecv) && (flags & IORING_CQE_F_MORE) && res > 0;
        if (!armed) { --c->inflight; --_uring_inflight; }

        if (_draining || c->doomed)
        {
            if (op == URecv && (flags & IORING_CQE_F_BUFFER))
                _bufring.recycle(flags >> IORING_CQE_BUFFER_SHIFT); // return the provided buffer
            if (!armed) u_maybe_free(c);
            return;
        }
        switch (op)
        {
            case URecv: u_on_recv(c, res, flags); break;
            case USend: u_on_send(c, res); break;
            case UConnect: u_on_connect(c, res); break;
            default: break;
        }
    }

    void Gateway::u_on_accept(int res, uint32_t flags) noexcept
    {
        if (_stop || _draining)
        {
            if (res >= 0) ::close(res); // shutting down: don't take new work
            return;
        }
        if (!(flags & IORING_CQE_F_MORE)) u_submit_accept(); // multishot ended -> re-arm
        if (res < 0) return;                                 // transient accept error
        const int fd = res;
        net::set_nodelay(fd);
        Connection* c = new Connection();
        c->fd = fd;
        c->is_client = true;
        c->id = _next_client_id++;
        c->rbuf.reserve(kInitialBuf);
        _clients[c->id] = c;
        u_arm_recv(c); // multishot recv stays armed for the connection's life
    }

    void Gateway::u_on_recv(Connection* c, int res, uint32_t flags) noexcept
    {
        const bool armed = flags & IORING_CQE_F_MORE;

        if (res <= 0) // multishot ended: EOF (0) or error (<0)
        {
            if (flags & IORING_CQE_F_BUFFER) _bufring.recycle(flags >> IORING_CQE_BUFFER_SHIFT);
            if (c->is_client) { if (c->peer) u_abort_pair(c); else u_close(c); }
            else if (c->peer && c->peer->streaming) u_stream_on_eof(c); // stream end, not a failure
            else if (!u_retry_upstream(c)) { if (c->peer) u_error_respond(c->peer, 502); else u_close(c); }
            return;
        }

        // Copy the bytes out of the kernel-selected buffer, then return it to the pool.
        if (flags & IORING_CQE_F_BUFFER)
        {
            const unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
            c->rbuf.append(_bufring.data(bid), static_cast<size_t>(res));
            _bufring.recycle(bid);
        }
        if (!armed) u_arm_recv(c); // kernel ended the multishot (pool pressure) -> re-arm

        if (c->is_client)
        {
            u_try_forward_buffered(c); // forward a framed request iff the client is idle
        }
        else
        {
            // Upstream bytes are the response to the in-flight request. Stray data on
            // an idle pooled upstream (no peer) means it's unusable — drop it.
            if (!c->peer) { u_close(c); return; }
            c->peer->ts_up_activity = now_ns(); // upstream made progress

            // Mid-stream: pump the newly-arrived body bytes and return.
            if (c->peer->streaming) { u_stream_pump(c); return; }

            // First response bytes: peek the head (parse_response_head tolerates
            // chunked, unlike parse()); a text/event-stream response enters the
            // streaming pump, everything else the whole-body path below.
            if (_translate == TranslateMode::Anthropic)
            {
                http::ResponseHead h;
                const auto hs = http::parse_response_head(c->rbuf, h);
                if (hs == http::HeadStatus::NeedMore) return; // wait for the full head
                if (hs == http::HeadStatus::Error) { u_error_respond(c->peer, 502); return; }
                // Only a 200 is a real stream; a provider error is relayed with its
                // own status by u_on_response below (never laundered into a 200).
                if (h.event_stream && h.status == 200) { u_begin_stream(c, h); return; }
            }

            http::Message m;
            const auto st = http::parse(c->rbuf, m);
            if (st == http::ParseStatus::NeedMore) return; // armed recv delivers the rest
            if (st == http::ParseStatus::Error) { u_error_respond(c->peer, 502); return; }
            c->peer->ts_up_recvd = now_ns();
            u_on_response(c, m);
        }
    }

    void Gateway::u_try_forward_buffered(Connection* c) noexcept
    {
        // Forward the next framed request only when the client is idle — no request
        // in flight (peer) and no response still draining to it (wbuf).
        if (c->peer != nullptr || !c->wbuf.empty() || c->rbuf.empty()) return;
        http::Message m;
        const auto st = http::parse(c->rbuf, m);
        if (st == http::ParseStatus::NeedMore) return; // the armed recv will deliver more
        if (st == http::ParseStatus::Error) { u_error_respond(c, 400); return; }
        c->msg = m;
        c->ts_req_recvd = now_ns();
        u_forward(c);
    }

    void Gateway::u_forward(Connection* c) noexcept
    {
        std::string upstream_bytes;
        if (_translate != TranslateMode::None)
        {
            std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
            std::string_view start_line;
            std::string tbody = xlate_req(_translate, body, start_line);
            if (tbody.empty()) { u_error_respond(c, 400); return; }
            c->wants_usage = provider::openai_wants_stream_usage(body); // see epoll mirror
            upstream_bytes = build_http(start_line, tbody);
        }
        else
        {
            upstream_bytes.assign(c->rbuf.data(), c->msg.total_len);
        }

        Connection* u = u_acquire_upstream();
        if (!u) { u_error_respond(c, 502); return; }

        u->wbuf = std::move(upstream_bytes);
        u->woff = 0;
        c->rbuf.erase(0, c->msg.total_len);
        c->peer = u;
        u->peer = c;
        c->ts_up_activity = now_ns(); // idle-timeout baseline for this request

        if (u->connected) u_submit_send(u);
        else u_submit_connect(u); // connect first; on completion we send the request
    }

    void Gateway::u_on_connect(Connection* u, int res) noexcept
    {
        if (res < 0)
        {
            Connection* cl = u->peer;
            u->peer = nullptr;
            u_close(u);
            if (cl) { cl->peer = nullptr; u_error_respond(cl, 502); }
            else ++_stats.errors;
            return;
        }
        u->connected = true;
        u_arm_recv(u);    // arm the multishot recv for this upstream's life
        u_submit_send(u); // then send the request
    }

    void Gateway::u_on_response(Connection* u, const http::Message& m) noexcept
    {
        Connection* client = u->peer;
        if (_translate != TranslateMode::None)
        {
            std::string_view body(u->rbuf.data() + m.header_len, m.body_len);
            // Relay a provider failure with ITS OWN status + message (see the epoll
            // mirror): a 429/529/400 must not be flattened into a generic 502.
            http::ResponseHead h;
            const bool have_head = http::parse_response_head(u->rbuf, h) == http::HeadStatus::Ok;
            if (have_head && h.status != 0 && h.status != 200)
            {
                client->wbuf = build_http_status(
                    h.status, reason_for(h.status),
                    provider::upstream_error_to_openai(body, "upstream_error"));
                client->woff = 0;
                client->peer = nullptr;
                if (m.keep_alive) u_release_upstream(u); else u_close(u);
                ++_stats.errors;
                u_submit_send(client);
                return;
            }
            std::string tbody = xlate_resp(_translate, body);
            if (tbody.empty())
            {
                client->peer = nullptr;
                u_release_upstream(u); // framing was valid; the upstream conn is reusable
                u_error_respond(client, 502);
                return;
            }
            client->wbuf = build_http("HTTP/1.1 200 OK", tbody);
        }
        else
        {
            client->wbuf.assign(u->rbuf.data(), m.total_len);
        }
        // Pool the upstream only if it will stay open: the response must say
        // keep-alive AND (for passthrough, where the client's Connection header was
        // forwarded verbatim) the client must not have asked to close. Otherwise the
        // upstream is about to close on us — drop it instead of reusing a corpse.
        const bool pool_upstream =
            m.keep_alive && (_translate != TranslateMode::None || client->msg.keep_alive);
        client->woff = 0;
        client->peer = nullptr;
        if (pool_upstream) u_release_upstream(u);
        else u_close(u);
        u_submit_send(client);
    }

    void Gateway::u_on_send(Connection* c, int res) noexcept
    {
        if (res <= 0)
        {
            if (c->is_client) { if (c->peer) u_abort_pair(c); else u_close(c); }
            else if (!u_retry_upstream(c)) { if (c->peer) u_error_respond(c->peer, 502); else u_close(c); }
            return;
        }
        c->woff += static_cast<size_t>(res);
        if (c->woff < c->wbuf.size()) { u_submit_send(c); return; } // partial -> send remainder

        if (!c->is_client)
        {
            // Request fully sent. The response arrives via the already-armed multishot
            // recv. KEEP wbuf so we can resend on a stale-connection failure.
            if (c->peer) c->peer->ts_up_sent = now_ns();
        }
        else if (c->streaming)
        {
            // This SSE buffer is fully out. Free the send slot and either send the
            // next pending bytes or finalize if the stream has ended.
            c->wbuf.clear();
            c->woff = 0;
            c->send_inflight = false;
            u_stream_kick(c);
        }
        else
        {
            c->wbuf.clear();
            c->woff = 0;
            u_finish_client(c);
        }
    }

    void Gateway::u_finish_client(Connection* c) noexcept
    {
        // Error replies (close_after_resp) are counted as errors, not in the latency
        // histograms — their timing stamps are unset.
        if (!c->close_after_resp)
        {
            const int64_t ts = now_ns();
            if (ts - _t_start >= _warmup_ns)
            {
                const int64_t req_ns = c->ts_up_sent - c->ts_req_recvd;
                const int64_t resp_ns = ts - c->ts_up_recvd;
                if (req_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(req_ns));
                if (resp_ns >= 0) _stats.resp_path.record(static_cast<uint64_t>(resp_ns));
                if (req_ns >= 0 && resp_ns >= 0)
                    _stats.overhead.record(static_cast<uint64_t>(req_ns + resp_ns));
                ++_stats.requests;
            }
        }
        const bool close_now = c->close_after_resp || !c->msg.keep_alive;
        c->msg = http::Message{};
        if (close_now) { u_close(c); return; }
        // The client's multishot recv is still armed; a pipelined next request may
        // already sit in rbuf — forward it, else the armed recv delivers more.
        u_try_forward_buffered(c);
    }

    // ── io_uring streaming pump (Anthropic->OpenAI SSE) ─────────────────────
    // Enter streaming: send the client SSE headers, then translate the body as it
    // arrives. Output accumulates in wpending; u_stream_kick moves it into wbuf
    // (kept immutable during an in-flight SEND) one send at a time.
    void Gateway::u_begin_stream(Connection* u, const http::ResponseHead& h) noexcept
    {
        Connection* client = u->peer;
        client->streaming = true;
        client->up_head_done = true;
        client->stream_chunked = h.chunked;
        client->sse = std::make_unique<provider::AnthropicToOpenAiSse>(-1, client->wants_usage);
        client->wpending.assign(kSseHead);
        u->rbuf.erase(0, h.header_len); // consume the head; the rest is body
        u_stream_pump(u);
    }

    void Gateway::u_stream_pump(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { u_close(u); return; }

        const StreamStep st = stream_step(client, u->rbuf, client->wpending, /*at_eof=*/false);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            // Truncate honestly: no fabricated [DONE]. Flush what we have, then the
            // finalize path closes the client (an aborted SSE body).
            client->stream_ended = true;
            client->close_after_resp = true;
            ++_stats.errors;
            u_stream_kick(client);
            return;
        }
        if (client->wpending.size() + client->wbuf.size() > kStreamBufCap) { u_abort_pair(client); return; }
        u_stream_kick(client);
    }

    void Gateway::u_stream_on_eof(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { u_close(u); return; }
        const StreamStep st = stream_step(client, u->rbuf, client->wpending, /*at_eof=*/true);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            client->stream_ended = true;
            client->close_after_resp = true;
            ++_stats.errors;
        }
        u_stream_kick(client);
    }

    // Serialize sends: only one SEND SQE outstanding (concurrent sends on a fd would
    // interleave). wbuf is (re)filled from wpending ONLY when idle, so its bytes stay
    // put while the kernel reads them for an in-flight SEND — no realloc-under-kernel.
    void Gateway::u_stream_kick(Connection* client) noexcept
    {
        if (client->send_inflight) return; // a send is already draining wbuf
        if (client->wpending.empty())
        {
            if (client->stream_ended) u_finalize_stream(client); // nothing left + ended
            return;
        }
        client->wbuf = std::move(client->wpending);
        client->wpending.clear();
        client->woff = 0;
        client->send_inflight = true;
        u_submit_send(client); // (closes the client on SQE exhaustion; nothing more to do)
    }

    void Gateway::u_finalize_stream(Connection* client) noexcept
    {
        if (Connection* u = client->peer) { client->peer = nullptr; u->peer = nullptr; u_close(u); }
        // Only a cleanly-terminated stream counts as served (see the epoll mirror).
        if (!client->close_after_resp) ++_stats.requests; // latency histograms N/A
        u_close(client);
    }

    int Gateway::run_uring()
    {
        _t_start = now_ns();

        unsigned flags = 0;
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_DEFER_TASKRUN)
        flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
#endif
        if (!_ring.init(kRingDepth, flags) && !_ring.init(kRingDepth, 0))
        {
            std::fprintf(stderr, "llmbridge: io_uring init failed; using epoll\n");
            return run_epoll();
        }
        if (!_bufring.init(_ring, kBufGroup, kBufCount, kBufSize))
        {
            std::fprintf(stderr, "llmbridge: io_uring provided-buffer ring unavailable; using epoll\n");
            return run_epoll();
        }
        if (!net::resolve_ipv4(_upstream_ip.c_str(), _upstream_port, _upstream_addr))
            return 1;

        _uring_ts.tv_sec = kPollTickMs / 1000;
        _uring_ts.tv_nsec = static_cast<long long>(kPollTickMs % 1000) * 1000000LL;

        u_submit_accept();
        u_submit_timer();

        auto reap = [this] {
            _ring.for_each_cqe([this](const io_uring_cqe* cqe) {
                u_on_cqe(cqe->user_data, cqe->res, cqe->flags);
            });
        };

        while (!_stop)
        {
            const int r = _ring.submit_and_wait(1);
            if (r < 0 && r != -EINTR && r != -ETIME) break;
            reap();
        }

        // Graceful drain: stop taking new work, force every live fd's in-flight ops
        // to complete, and reap until nothing is outstanding — so no kernel op
        // writes into a buffer we're about to free. Acquired upstreams are reachable
        // only via client->peer, so shut those down too.
        _draining = true;
        auto stop_conn = [this](Connection* c) {
            if (c->fd >= 0) { ::shutdown(c->fd, SHUT_RDWR); u_submit_cancel(c->fd); }
        };
        for (auto& [id, c] : _clients)
        {
            stop_conn(c);
            if (c->peer) stop_conn(c->peer); // acquired upstreams reachable only via peer
        }
        for (Connection* u : _idle_upstreams) stop_conn(u);
        // doomed conns were already shut down + cancelled by u_close().

        while (_uring_inflight > 0)
        {
            const int r = _ring.submit_and_wait(1);
            if (r < 0 && r != -EINTR && r != -ETIME) break;
            reap();
        }

        // Free acquired (in-flight) upstreams now drained but tracked only via peer;
        // the rest (_clients, _idle_upstreams, _doomed, listen_conn) are freed by
        // ~Gateway.
        for (auto& [id, c] : _clients)
            if (Connection* u = c->peer) { c->peer = nullptr; if (u->fd >= 0) ::close(u->fd); delete u; }

        return 0;
    }
#endif // LLMBRIDGE_HAVE_URING
} // namespace llmbridge
