// Kottos benchmark load generator.
//
// Open-loop, constant-arrival-rate HTTP/1.1 driver — the wrk2 methodology,
// purpose-built because wrk2 isn't on the box and because owning the loop lets
// us correct coordinated omission exactly. Requests "arrive" on a fixed
// schedule (1/RPS apart); each request's measured latency runs from its
// *scheduled* arrival time, not from when a connection happened to be free. So
// if the target stalls, the backlog's latency is counted from when those
// requests should have gone out — the tail can't hide behind a closed loop.
//
// Single epoll thread drives a fixed pool of persistent keep-alive connections
// (sized to cover peak concurrency = RPS x latency). The same non-blocking
// primitives the proxy uses (net/socket_util.hpp, net/http.hpp) and the same
// Kottos Histogram/Clock back the measurement, so loadgen and proxy report
// latency on identical machinery.
//
// epoll_pwait2 gives the loop a nanosecond-resolution timeout, which the
// open-loop scheduler needs to hold a tight sub-millisecond arrival cadence at
// high RPS (plain epoll_wait only takes whole milliseconds).
//
//   loadgen --target IP:PORT --rps N --duration SEC [--conns C]
//           [--warmup SEC] [--body-bytes N]

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

#include "gateway/metrics.hpp" // kottos::Histogram + now_ns()
#include "net/http.hpp"
#include "net/socket_util.hpp"

using kottos::http::Message;
using kottos::http::ParseStatus;

namespace
{
    enum class State : uint8_t
    {
        Connecting,
        Idle,
        Awaiting
    };

    struct Connection
    {
        int fd = -1;
        State state = State::Connecting;
        bool write_armed = false;
        std::string rbuf;
        size_t woff = 0;          // bytes of the request already written
        int64_t scheduled_ns = 0; // CO-corrected: when this request *should* have gone out
    };

    struct Args
    {
        std::string ip = "127.0.0.1";
        uint16_t port = 8088;
        double rps = 1000;
        int duration = 10;
        int conns = 0;   // 0 => auto
        double warmup = 2.0;
        int body_bytes = 64;
    };

    Args parse_args(int argc, char** argv)
    {
        Args a;
        for (int i = 1; i < argc; ++i)
        {
            std::string s = argv[i];
            auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
            if (s == "--target")
            {
                if (const char* v = next())
                {
                    const char* c = std::strrchr(v, ':');
                    if (c) { a.ip.assign(v, c - v); a.port = (uint16_t)std::atoi(c + 1); }
                }
            }
            else if (s == "--rps") { if (auto v = next()) a.rps = std::atof(v); }
            else if (s == "--duration") { if (auto v = next()) a.duration = std::atoi(v); }
            else if (s == "--conns") { if (auto v = next()) a.conns = std::atoi(v); }
            else if (s == "--warmup") { if (auto v = next()) a.warmup = std::atof(v); }
            else if (s == "--body-bytes") { if (auto v = next()) a.body_bytes = std::atoi(v); }
            else if (s == "--help" || s == "-h")
            {
                std::printf("usage: %s --target IP:PORT --rps N --duration SEC "
                            "[--conns C] [--warmup SEC] [--body-bytes N]\n", argv[0]);
                std::exit(0);
            }
        }
        return a;
    }

    // Build the fixed request once. Tiny chat-completion POST with a JSON body
    // padded to body_bytes so we exercise a realistic small request size.
    std::string build_request(const Args& a)
    {
        std::string pad((size_t)std::max(0, a.body_bytes), 'x');
        std::string body = "{\"model\":\"mock-1\",\"messages\":[{\"role\":\"user\","
                           "\"content\":\"" + pad + "\"}]}";
        std::string req =
            "POST /v1/chat/completions HTTP/1.1\r\n"
            "Host: " + a.ip + "\r\n"
            "Content-Type: application/json\r\n"
            "Connection: keep-alive\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "\r\n" + body;
        return req;
    }
} // namespace

int main(int argc, char** argv)
{
    Args args = parse_args(argc, argv);
    const std::string REQ = build_request(args);
    const double interval_ns = 1e9 / args.rps;

    std::signal(SIGPIPE, SIG_IGN);

    // Auto-size the connection pool to cover peak in-flight requests. We don't
    // know the target's latency a priori, so assume up to ~250 ms and add 50%
    // headroom; capped so a typo doesn't open a million sockets.
    int conns = args.conns;
    if (conns <= 0)
    {
        double est_concurrency = args.rps * 0.25;
        conns = (int)(est_concurrency * 1.5) + 16;
        if (conns > 20000) conns = 20000;
    }

    int epfd = ::epoll_create1(0);
    if (epfd < 0) { std::perror("epoll_create1"); return 1; }

    // 10 µs buckets x 65536 => 0..655 ms range, covering a 200 ms backend + tail.
    kottos::Histogram hist(10'000, 65536);

    std::vector<Connection*> pool;
    pool.reserve(conns);
    std::vector<Connection*> idle; // connected + ready to send

    // epoll level-triggered: EPOLLIN stays armed for the conn's life; toggle
    // EPOLLOUT via EPOLL_CTL_MOD for connect-completion / partial-write flushing.
    auto add_read = [&](Connection* c) {
        epoll_event ev{}; ev.events = EPOLLIN; ev.data.ptr = c;
        ::epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev);
    };
    auto arm_write = [&](Connection* c) {
        if (c->write_armed) return;
        epoll_event ev{}; ev.events = EPOLLIN | EPOLLOUT; ev.data.ptr = c;
        ::epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev); c->write_armed = true;
    };
    auto disarm_write = [&](Connection* c) {
        if (!c->write_armed) return;
        epoll_event ev{}; ev.events = EPOLLIN; ev.data.ptr = c;
        ::epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev); c->write_armed = false;
    };

    // Open all connections up front (non-blocking connect).
    for (int i = 0; i < conns; ++i)
    {
        int fd = kottos::net::start_connect(args.ip.c_str(), args.port);
        if (fd < 0) { std::perror("connect"); continue; }
        Connection* c = new Connection();
        c->fd = fd;
        c->state = State::Connecting;
        c->rbuf.reserve(1024);
        add_read(c);
        arm_write(c); // wait for connect completion
        pool.push_back(c);
    }
    std::fprintf(stderr, "loadgen: %s:%u  rps=%.0f  conns=%d  duration=%ds  warmup=%.1fs\n",
                 args.ip.c_str(), args.port, args.rps, (int)pool.size(), args.duration, args.warmup);

    auto send_on = [&](Connection* c) {
        // Write the (tiny) request; handle the rare partial write via woff.
        c->woff = 0;
        while (c->woff < REQ.size())
        {
            ssize_t n = ::write(c->fd, REQ.data() + c->woff, REQ.size() - c->woff);
            if (n > 0) { c->woff += (size_t)n; continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { arm_write(c); return; }
            if (n < 0 && errno == EINTR) continue;
            // write error — drop this conn from rotation
            c->state = State::Idle; // will be retried as idle; simplest
            return;
        }
        c->state = State::Awaiting;
    };

    const int64_t t_start = kottos::now_ns();
    const int64_t t_warmup_end = t_start + (int64_t)(args.warmup * 1e9);
    const int64_t t_end = t_start + (int64_t)((args.warmup + args.duration) * 1e9);
    int64_t next_arrival = t_start;
    std::deque<int64_t> backlog; // scheduled arrival times not yet dispatched

    uint64_t sent = 0, completed = 0, errors = 0, dropped_warmup = 0;
    uint64_t max_backlog = 0;

    epoll_event events[2048];
    while (true)
    {
        int64_t now = kottos::now_ns();
        if (now >= t_end) break;

        // 1) Generate arrivals up to `now` on the fixed schedule.
        while (next_arrival <= now)
        {
            backlog.push_back(next_arrival);
            next_arrival += (int64_t)interval_ns;
        }
        if (backlog.size() > max_backlog) max_backlog = backlog.size();

        // 2) Dispatch backlog onto idle connections.
        while (!backlog.empty() && !idle.empty())
        {
            Connection* c = idle.back();
            idle.pop_back();
            c->scheduled_ns = backlog.front();
            backlog.pop_front();
            c->rbuf.clear();
            send_on(c);
            ++sent;
        }

        // 3) Wait for I/O, but no longer than until the next arrival.
        int64_t wait_ns = next_arrival - now;
        if (wait_ns < 0) wait_ns = 0;
        if (wait_ns > 1'000'000) wait_ns = 1'000'000; // cap 1 ms so schedule stays tight
        struct timespec ts;
        ts.tv_sec = wait_ns / 1'000'000'000;
        ts.tv_nsec = wait_ns % 1'000'000'000;

        int n = ::epoll_pwait2(epfd, events, 2048, &ts, nullptr);
        if (n < 0) { if (errno == EINTR) continue; std::perror("epoll_pwait2"); break; }

        for (int i = 0; i < n; ++i)
        {
            Connection* c = static_cast<Connection*>(events[i].data.ptr);
            const uint32_t e = events[i].events;

            // Writable: connect completion, or finishing a partial request write.
            if (e & EPOLLOUT)
            {
                if (c->state == State::Connecting)
                {
                    int err = kottos::net::connect_result(c->fd);
                    disarm_write(c);
                    if (err != 0) { ++errors; c->state = State::Idle; idle.push_back(c); continue; }
                    c->state = State::Idle;
                    idle.push_back(c);
                    continue;
                }
                // finishing a partial request write
                while (c->woff < REQ.size())
                {
                    ssize_t w = ::write(c->fd, REQ.data() + c->woff, REQ.size() - c->woff);
                    if (w > 0) { c->woff += (size_t)w; continue; }
                    break;
                }
                if (c->woff >= REQ.size()) { disarm_write(c); c->state = State::Awaiting; }
            }

            if (!(e & (EPOLLIN | EPOLLHUP | EPOLLERR))) continue;

            // Readable: read the response.
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
            if (dead) { ++errors; ::close(c->fd); c->fd = -1; continue; }

            Message m;
            ParseStatus st = kottos::http::parse(c->rbuf, m);
            if (st == ParseStatus::NeedMore) continue;
            if (st == ParseStatus::Error) { ++errors; ::close(c->fd); c->fd = -1; continue; }

            // Full response. Record CO-corrected latency, recycle the conn.
            int64_t lat = kottos::now_ns() - c->scheduled_ns;
            if (kottos::now_ns() >= t_warmup_end)
            {
                if (lat >= 0) hist.record((uint64_t)lat);
                ++completed;
            }
            else ++dropped_warmup;

            c->rbuf.erase(0, m.total_len);
            c->state = State::Idle;
            idle.push_back(c);
        }
    }

    double secs = args.duration;
    double achieved = completed / secs;
    std::fprintf(stderr, "\n=== loadgen results (target %s:%u, rps=%.0f) ===\n",
                 args.ip.c_str(), args.port, args.rps);
    std::fprintf(stderr,
                 "sent=%llu completed=%llu errors=%llu warmup_dropped=%llu "
                 "achieved=%.0f req/s  max_backlog=%llu\n",
                 (unsigned long long)sent, (unsigned long long)completed,
                 (unsigned long long)errors, (unsigned long long)dropped_warmup,
                 achieved, (unsigned long long)max_backlog);
    hist.print(std::cerr, "client-observed e2e ");

    // Machine-readable line for the runner script to aggregate.
    std::fprintf(stdout,
                 "RESULT rps=%.0f achieved=%.0f completed=%llu errors=%llu "
                 "p50_us=%llu p99_us=%llu p999_us=%llu max_us=%llu\n",
                 args.rps, achieved, (unsigned long long)completed,
                 (unsigned long long)errors,
                 (unsigned long long)(hist.percentile(0.50) / 1000),
                 (unsigned long long)(hist.percentile(0.99) / 1000),
                 (unsigned long long)(hist.percentile(0.999) / 1000),
                 (unsigned long long)(hist.max() / 1000));
    return 0;
}
