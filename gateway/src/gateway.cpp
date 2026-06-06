#include "gateway/gateway.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include "net/socket_util.hpp"

namespace kottos
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
    } // namespace

    Gateway::Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                     int64_t warmup_ns, TranslateMode translate)
        : _listen_port(listen_port), _upstream_ip(std::move(upstream_ip)),
          _upstream_port(upstream_port), _warmup_ns(warmup_ns), _translate(translate)
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
        std::fprintf(stderr, "kottos: listening :%u -> upstream %s:%u%s\n",
                     _listen_port, _upstream_ip.c_str(), _upstream_port,
                     _translate == TranslateMode::Anthropic ? " (translate: anthropic)" : "");
    }

    Gateway::~Gateway()
    {
        for (auto& [id, c] : _clients) { if (c->fd >= 0) ::close(c->fd); delete c; }
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

    Connection* Gateway::client_by_id(uint64_t id) noexcept
    {
        auto it = _clients.find(id);
        return it == _clients.end() ? nullptr : it->second;
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
        *done = true;
        c->wbuf.clear();
        c->woff = 0;
        return true;
    }

    Connection* Gateway::acquire_upstream() noexcept
    {
        if (!_idle_upstreams.empty())
        {
            Connection* u = _idle_upstreams.back();
            _idle_upstreams.pop_back();
            return u;
        }
        int fd = net::start_connect(_upstream_ip.c_str(), _upstream_port);
        if (fd < 0) return nullptr;
        Connection* u = new Connection();
        u->fd = fd;
        u->is_client = false;
        u->rbuf.reserve(kInitialBuf);
        ep_add_read(u);
        ep_arm_write(u); // learn when the non-blocking connect completes
        ++_stats.upstream_conns_opened;
        return u;
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

        http::Message m;
        auto st = http::parse(c->rbuf, m);
        if (st == http::ParseStatus::NeedMore) return;
        if (st == http::ParseStatus::Error) { close_client(c); ++_stats.errors; return; }

        c->msg = m;
        c->ts_req_recvd = now_ns();
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
            std::string tbody;
            std::string_view start_line;
            switch (_translate)
            {
                case TranslateMode::Anthropic:
                    tbody = provider::openai_to_anthropic_request(body);
                    start_line = "POST /v1/messages HTTP/1.1";
                    break;
                case TranslateMode::Gemini:
                    tbody = provider::openai_to_gemini_request(body);
                    start_line = "POST /v1beta/models/gemini:generateContent HTTP/1.1";
                    break;
                case TranslateMode::Cohere:
                    tbody = provider::openai_to_cohere_request(body);
                    start_line = "POST /v2/chat HTTP/1.1";
                    break;
                case TranslateMode::None:
                    break; // unreachable (guarded above)
            }
            if (tbody.empty()) { close_client(c); ++_stats.errors; return; }
            upstream_bytes = build_http(start_line, tbody);
        }
        else
        {
            upstream_bytes.assign(c->rbuf.data(), c->msg.total_len);
        }

        Connection* u = acquire_upstream();
        if (!u) { abort_pair(c); return; }

        u->wbuf = std::move(upstream_bytes);
        u->woff = 0;
        c->rbuf.erase(0, c->msg.total_len);
        c->peer = u;
        u->peer = c;

        if (u->connected) ep_arm_write(u);
        // else: on_upstream_writable sends once the connect completes.
    }

    void Gateway::on_upstream_writable(Connection* u) noexcept
    {
        if (!u->connected)
        {
            int err = net::connect_result(u->fd);
            if (err != 0)
            {
                Connection* client = u->peer;
                if (client) { client->peer = nullptr; close_client(client); }
                close_upstream(u);
                ++_stats.errors;
                return;
            }
            u->connected = true;
        }
        bool done = false;
        if (!pump_write(u, &done)) { if (u->peer) abort_pair(u->peer); else close_upstream(u); return; }
        if (!done) { ep_arm_write(u); return; }
        ep_disarm_write(u);
        if (u->peer) u->peer->ts_up_sent = now_ns(); // end of request-path work
    }

    void Gateway::on_upstream_readable(Connection* u) noexcept
    {
        if (!drain_read(u))
        {
            if (u->peer == nullptr) close_upstream(u); // idle pooled conn dropped
            else abort_pair(u->peer);
            return;
        }
        if (u->peer == nullptr) return; // stray bytes on an idle pooled conn

        http::Message m;
        auto st = http::parse(u->rbuf, m);
        if (st == http::ParseStatus::NeedMore) return;
        if (st == http::ParseStatus::Error) { abort_pair(u->peer); return; }

        Connection* client = u->peer;
        client->ts_up_recvd = now_ns(); // end of upstream wait

        if (_translate != TranslateMode::None)
        {
            std::string_view body(u->rbuf.data() + m.header_len, m.body_len);
            std::string tbody;
            switch (_translate)
            {
                case TranslateMode::Anthropic: tbody = provider::anthropic_to_openai_response(body); break;
                case TranslateMode::Gemini:    tbody = provider::gemini_to_openai_response(body); break;
                case TranslateMode::Cohere:    tbody = provider::cohere_to_openai_response(body); break;
                case TranslateMode::None:      break; // unreachable (guarded above)
            }
            if (tbody.empty())
            {
                client->peer = nullptr;
                release_upstream(u);
                close_client(client);
                ++_stats.errors;
                return;
            }
            client->wbuf = build_http("HTTP/1.1 200 OK", tbody);
        }
        else
        {
            client->wbuf.assign(u->rbuf.data(), m.total_len);
        }
        client->woff = 0;

        // Response fully read -> upstream is free; pool it and write to client.
        client->peer = nullptr;
        release_upstream(u);
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
        bool done = false;
        if (!pump_write(c, &done)) { abort_pair(c); return; }
        if (!done) { ep_arm_write(c); return; }
        finish_client_response(c);
    }

    void Gateway::finish_client_response(Connection* c) noexcept
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
        ep_disarm_write(c);
        const bool keep_alive = c->msg.keep_alive;
        c->msg = http::Message{};
        if (!keep_alive) { close_client(c); return; }
        on_client_readable(c); // service any pipelined next request already in rbuf
    }

    int Gateway::run()
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
            for (Connection* d : _doomed) delete d;
            _doomed.clear();
        }
        return 0;
    }
} // namespace kottos
