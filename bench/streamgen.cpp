// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// streamgen: the streaming (SSE) load generator, the Phase-B counterpart to
// loadgen.cpp. Deliberately a separate binary: loadgen is open-loop and paced by
// RPS (one short request per slot), while a streaming benchmark is closed-loop and
// paced by concurrency (N long-lived streams, each delivering many chunks). It
// also keeps loadgen byte-for-byte untouched, so the already-published
// non-streaming numbers stay exactly reproducible.
//
// ── What it measures, and why it can't be fooled ─────────────────────────────
// The mock provider stamps its own CLOCK_MONOTONIC emission time inside each
// token ("t=<ns> "), and that stamp rides the payload through whatever gateway is
// under test. So per chunk we compute:
//
//     added latency = arrival_at_client - emission_at_mock
//
// Both processes share CLOCK_MONOTONIC (same host), so this needs no clock
// synchronisation, and no assumption about when a token *should*
// have arrived. That sidesteps coordinated omission entirely: a stalled gateway
// cannot hide by simply delivering fewer chunks, because every chunk that does
// arrive carries the true age of its own data.
//
// Reported: TTFT (request sent -> first chunk), per-chunk added latency
// (p50/p99/p99.9/max), inter-token gap at the client, and completion counts.
//
//   streamgen --port 8088 [--streams 64] [--duration 15] [--warmup 3]
//             [--path /v1/chat/completions] [--header "K: V"] [--csv out.csv]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/metrics.hpp" // llmbridge::Histogram + now_ns()

// Optional TLS arm, so the inbound-TLS cost can be measured at all. Without it
// there is no client in this tree that speaks TLS to the gateway, and the only
// well-formed comparison (same gateway, --listen-tls on against off) cannot run.
// Verification is not disableable here either; pass the CA with --ca.
#ifdef LLMBRIDGE_HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace
{
    using llmbridge::Histogram;
    using llmbridge::now_ns;

    struct Conn
    {
        int fd = -1;
#ifdef LLMBRIDGE_HAVE_TLS
        SSL* ssl = nullptr;
        bool hs_done = false;
#endif
        bool connected = false;
        bool req_sent = false;
        bool head_done = false;   // response headers consumed
        bool first_chunk = false; // TTFT recorded
        std::string rbuf;
        std::string wbuf;
        size_t woff = 0;
        int64_t ts_sent = 0; // when the request went out (for TTFT)
        int64_t last_chunk_ns = 0;
        long chunks = 0;
    };

    // Pull the mock's emission stamp out of a payload: "t=<digits>". Searching the
    // raw bytes (instead of JSON-parsing) means the same parser works for the
    // OpenAI chunk shape (delta.content), the raw Anthropic shape (delta.text),
    // and anything else a gateway might wrap it in.
    bool extract_stamp(std::string_view data, int64_t& out)
    {
        const size_t p = data.find("t=");
        if (p == std::string_view::npos) return false;
        const char* b = data.data() + p + 2;
        const char* e = data.data() + data.size();
        int64_t v = 0;
        auto [ptr, ec] = std::from_chars(b, e, v);
        if (ec != std::errc{} || ptr == b) return false;
        out = v;
        return true;
    }

    int connect_nb(const char* ip, uint16_t port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) return -1;
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        ::inet_pton(AF_INET, ip, &a.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 && errno != EINPROGRESS)
        {
            ::close(fd);
            return -1;
        }
        return fd;
    }

#ifdef LLMBRIDGE_HAVE_TLS
    SSL_CTX* g_ctx = nullptr;

    /// -1 = fatal, 0 = would block, 1 = handshake complete. `want_write` tells the
    /// caller which epoll event to arm, which is the whole reason this is not a
    /// blocking SSL_connect.
    int tls_handshake(Conn* c, bool& want_write)
    {
        want_write = false;
        const int r = SSL_connect(c->ssl);
        if (r == 1) { c->hs_done = true; return 1; }
        const int e = SSL_get_error(c->ssl, r);
        if (e == SSL_ERROR_WANT_WRITE) { want_write = true; return 0; }
        if (e == SSL_ERROR_WANT_READ) return 0;
        return -1;
    }
#endif

    /// read()/write() or their TLS equivalents. Returns the same conventions as the
    /// syscalls (>0 bytes, 0 = peer closed, <0 with EAGAIN = would block) so the
    /// event loop below is identical on both transports.
    ssize_t conn_read(Conn* c, char* p, size_t n)
    {
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->ssl)
        {
            const int r = SSL_read(c->ssl, p, static_cast<int>(n));
            if (r > 0) return r;
            const int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { errno = EAGAIN; return -1; }
            return 0; // ZERO_RETURN or fatal: treat as closed, as the caller does
        }
#endif
        return ::read(c->fd, p, n);
    }

    ssize_t conn_write(Conn* c, const char* p, size_t n)
    {
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->ssl)
        {
            const int w = SSL_write(c->ssl, p, static_cast<int>(n));
            if (w > 0) return w;
            const int e = SSL_get_error(c->ssl, w);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { errno = EAGAIN; return -1; }
            errno = EIO;
            return -1;
        }
#endif
        return ::write(c->fd, p, n);
    }
} // namespace

int main(int argc, char** argv)
{
    const char* host = "127.0.0.1";
    uint16_t port = 8088;
    int streams = 64;
    double duration = 15.0, warmup = 3.0;
    std::string path = "/v1/chat/completions";
    std::string extra_header;
    std::string model = "mock-1";
    const char* csv = nullptr;
    const char* label = "run";
    bool tls = false;
    const char* ca_file = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--host") { if (const char* v = next()) host = v; }
        else if (a == "--port") { if (const char* v = next()) port = static_cast<uint16_t>(std::atoi(v)); }
        else if (a == "--streams") { if (const char* v = next()) streams = std::atoi(v); }
        else if (a == "--duration") { if (const char* v = next()) duration = std::atof(v); }
        else if (a == "--warmup") { if (const char* v = next()) warmup = std::atof(v); }
        else if (a == "--path") { if (const char* v = next()) path = v; }
        else if (a == "--model") { if (const char* v = next()) model = v; }
        else if (a == "--header") { if (const char* v = next()) extra_header = v; }
        else if (a == "--csv") { if (const char* v = next()) csv = v; }
        else if (a == "--label") { if (const char* v = next()) label = v; }
        else if (a == "--tls") { tls = true; }
        else if (a == "--ca") { if (const char* v = next()) ca_file = v; }
        else if (a == "--help" || a == "-h")
        {
            std::printf("usage: %s [--host IP] [--port N] [--streams N] [--duration S]\n"
                        "          [--warmup S] [--path P] [--model M] [--header \"K: V\"]\n"
                        "          [--csv FILE] [--label NAME] [--tls --ca FILE]\n", argv[0]);
            return 0;
        }
    }

    // One streaming request body, reused by every target: the mock triggers on
    // "stream":true, the gateway translates it to Anthropic, and LiteLLM accepts
    // the same OpenAI shape, so all three paths do equal work.
    const std::string body =
        "{\"model\":\"" + model + "\",\"stream\":true,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: bench\r\n";
    if (!extra_header.empty()) req += extra_header + "\r\n";
    req += "Content-Type: application/json\r\nAccept: text/event-stream\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

#ifdef LLMBRIDGE_HAVE_TLS
    if (tls)
    {
        if (!ca_file)
        {
            // No unverified mode, deliberately: a benchmark client that skips
            // verification is one somebody copies into production.
            std::fprintf(stderr, "streamgen: --tls needs --ca FILE\n");
            return 2;
        }
        g_ctx = SSL_CTX_new(TLS_client_method());
        if (!g_ctx) { std::fprintf(stderr, "streamgen: SSL_CTX_new failed\n"); return 1; }
        SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_load_verify_locations(g_ctx, ca_file, nullptr) != 1)
        {
            std::fprintf(stderr, "streamgen: cannot load CA %s\n", ca_file);
            return 1;
        }
    }
#else
    if (tls)
    {
        std::fprintf(stderr, "streamgen: --tls needs a TLS build (-DLLMBRIDGE_TLS=ON)\n");
        return 2;
    }
    (void)ca_file;
#endif

    const int epfd = ::epoll_create1(0);
    if (epfd < 0) { std::perror("epoll_create1"); return 1; }

    std::vector<Conn*> conns;
    conns.reserve(static_cast<size_t>(streams));

    auto arm = [&](Conn* c, uint32_t ev) {
        epoll_event e{};
        e.events = ev;
        e.data.ptr = c;
        ::epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &e);
    };

    auto start_stream = [&](Conn* c) {
        if (c->fd >= 0) { ::epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, nullptr); ::close(c->fd); }
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->ssl) { SSL_free(c->ssl); c->ssl = nullptr; }
#endif
        *c = Conn{};
        c->fd = connect_nb(host, port);
        if (c->fd < 0) return false;
#ifdef LLMBRIDGE_HAVE_TLS
        if (g_ctx)
        {
            c->ssl = SSL_new(g_ctx);
            if (!c->ssl) return false;
            SSL_set_fd(c->ssl, c->fd);
            SSL_set_tlsext_host_name(c->ssl, host);   // SNI
            SSL_set1_host(c->ssl, host);              // and hostname verification
            SSL_set_connect_state(c->ssl);
        }
#endif
        c->wbuf = req;
        epoll_event e{};
        e.events = EPOLLOUT;
        e.data.ptr = c;
        ::epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &e);
        return true;
    };

    for (int i = 0; i < streams; ++i)
    {
        Conn* c = new Conn();
        conns.push_back(c);
        if (!start_stream(c)) { std::fprintf(stderr, "connect failed\n"); return 1; }
    }

    // Histogram range matters here. The default (20ns x 131072 = 2.62ms ceiling) is
    // sized for sub-millisecond proxy overhead; streaming under load produces
    // multi-SECOND outliers, and percentile() returns _max once the target falls in
    // the overflow region, so an overflowing histogram silently reports
    // "p50 == p99 == max" that looks like a percentile but is just the maximum.
    // 1us buckets over 2s for latency/gap; 100us over 20s for TTFT (a saturated
    // Python gateway can take >10s to first token).
    Histogram h_chunk(1000, 2'000'000);   // 1us buckets, 2s range
    Histogram h_gap(1000, 2'000'000);     // 1us buckets, 2s range
    Histogram h_ttft(100'000, 200'000);   // 100us buckets, 20s range
    long completed = 0, total_chunks = 0, measured_chunks = 0, failures = 0;
    const int64_t t0 = now_ns();
    const int64_t t_warm = t0 + static_cast<int64_t>(warmup * 1e9);
    const int64_t t_end = t0 + static_cast<int64_t>(duration * 1e9);

    std::vector<epoll_event> events(static_cast<size_t>(streams) + 8);
    while (now_ns() < t_end)
    {
        const int n = ::epoll_wait(epfd, events.data(), static_cast<int>(events.size()), 50);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; ++i)
        {
            Conn* c = static_cast<Conn*>(events[i].data.ptr);
            const uint32_t ev = events[i].events;

            if (ev & EPOLLOUT)
            {
                if (!c->connected)
                {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    ::getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err != 0) { ++failures; start_stream(c); continue; }
                    c->connected = true;
                }
#ifdef LLMBRIDGE_HAVE_TLS
                if (c->ssl && !c->hs_done)
                {
                    bool want_write = false;
                    const int hs = tls_handshake(c, want_write);
                    if (hs < 0) { ++failures; start_stream(c); continue; }
                    if (hs == 0) { arm(c, want_write ? EPOLLOUT : EPOLLIN); continue; }
                }
#endif
                while (c->woff < c->wbuf.size())
                {
                    const ssize_t w =
                        conn_write(c, c->wbuf.data() + c->woff, c->wbuf.size() - c->woff);
                    if (w > 0) { c->woff += static_cast<size_t>(w); continue; }
                    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    ++failures;
                    start_stream(c);
                    goto next_event;
                }
                if (c->woff >= c->wbuf.size() && !c->req_sent)
                {
                    c->req_sent = true;
                    c->ts_sent = now_ns();
                    arm(c, EPOLLIN);
                }
            }

            if (ev & (EPOLLIN | EPOLLHUP | EPOLLERR))
            {
#ifdef LLMBRIDGE_HAVE_TLS
                // The handshake can be waiting on a read, which is why it is driven
                // from both branches. Resuming it only on EPOLLOUT deadlocks after
                // the ClientHello has gone out.
                if (c->ssl && !c->hs_done)
                {
                    bool want_write = false;
                    const int hs = tls_handshake(c, want_write);
                    if (hs < 0) { ++failures; start_stream(c); continue; }
                    if (hs == 0) { arm(c, want_write ? EPOLLOUT : EPOLLIN); continue; }
                    arm(c, EPOLLOUT); // handshake done: go send the request
                    continue;
                }
#endif
                char tmp[16384];
                bool closed = false;
                for (;;)
                {
                    const ssize_t r = conn_read(c, tmp, sizeof(tmp));
                    if (r > 0) { c->rbuf.append(tmp, static_cast<size_t>(r)); continue; }
                    if (r == 0) { closed = true; break; }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    closed = true;
                    break;
                }

                const int64_t arrival = now_ns();
                const bool counting = arrival >= t_warm;

                // Skip response headers once.
                if (!c->head_done)
                {
                    const size_t hb = c->rbuf.find("\r\n\r\n");
                    if (hb != std::string::npos) { c->rbuf.erase(0, hb + 4); c->head_done = true; }
                }

                // Consume whole SSE lines; each "data:" payload that carries a
                // stamp is one measured chunk.
                if (c->head_done)
                {
                    size_t pos = 0;
                    for (;;)
                    {
                        const size_t nl = c->rbuf.find('\n', pos);
                        if (nl == std::string::npos) break;
                        std::string_view line(c->rbuf.data() + pos, nl - pos);
                        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                        pos = nl + 1;
                        if (line.rfind("data:", 0) != 0) continue;

                        int64_t emitted = 0;
                        if (extract_stamp(line, emitted))
                        {
                            ++c->chunks;
                            ++total_chunks;
                            if (counting)
                            {
                                ++measured_chunks; // rate must exclude warmup chunks
                                const int64_t added = arrival - emitted;
                                if (added >= 0) h_chunk.record(static_cast<uint64_t>(added));
                                if (c->last_chunk_ns != 0)
                                {
                                    const int64_t gap = arrival - c->last_chunk_ns;
                                    if (gap >= 0) h_gap.record(static_cast<uint64_t>(gap));
                                }
                            }
                            c->last_chunk_ns = arrival;
                            if (!c->first_chunk)
                            {
                                c->first_chunk = true;
                                if (counting && c->ts_sent)
                                    h_ttft.record(static_cast<uint64_t>(arrival - c->ts_sent));
                            }
                        }
                        // End of stream. Both terminators must be recognised: a
                        // gateway under test emits OpenAI's "[DONE]" sentinel, while
                        // the DIRECT-to-mock baseline sees Anthropic's raw
                        // "message_stop" event. Miss either and that path never
                        // recycles its connections, so it silently under-measures.
                        else if (line.find("[DONE]") != std::string_view::npos ||
                                 line.find("message_stop") != std::string_view::npos)
                        {
                            ++completed;
                            closed = true; // stream finished; recycle the connection
                        }
                    }
                    c->rbuf.erase(0, pos);
                }

                if (closed) start_stream(c); // keep `streams` streams in flight
            }
        next_event:;
        }
    }

    for (Conn* c : conns) { if (c->fd >= 0) ::close(c->fd); delete c; }
    ::close(epfd);

    const double secs = duration - warmup;
    std::printf("streams=%d  duration=%.0fs (warmup %.0fs)\n", streams, duration, warmup);
    // chunk_rate counts only post-warmup chunks over the post-warmup window;
    // dividing all-chunks by the measured window inflated it by duration/(duration-warmup).
    std::printf("chunks=%ld  measured=%ld  completed_streams=%ld  failures=%ld  chunk_rate=%.0f/s\n",
                total_chunks, measured_chunks, completed, failures,
                secs > 0 ? measured_chunks / secs : 0.0);
    // Overflow must never be silent: if it happens the percentiles above are
    // really just the max, and the run needs a wider histogram to be reportable.
    if (h_chunk.overflow_count() || h_ttft.overflow_count() || h_gap.overflow_count())
        std::printf("WARNING: histogram overflow (chunk=%llu ttft=%llu gap=%llu):"
                    " percentiles beyond the tracked range are NOT reliable\n",
                    (unsigned long long)h_chunk.overflow_count(),
                    (unsigned long long)h_ttft.overflow_count(),
                    (unsigned long long)h_gap.overflow_count());
    std::printf("per-chunk added latency us: p50=%llu p99=%llu p99.9=%llu max=%llu\n",
                (unsigned long long)(h_chunk.percentile(0.50) / 1000),
                (unsigned long long)(h_chunk.percentile(0.99) / 1000),
                (unsigned long long)(h_chunk.percentile(0.999) / 1000),
                (unsigned long long)(h_chunk.max() / 1000));
    std::printf("ttft ms: p50=%.2f p99=%.2f\n",
                h_ttft.percentile(0.50) / 1e6, h_ttft.percentile(0.99) / 1e6);
    std::printf("inter-token gap ms: p50=%.2f p99=%.2f\n",
                h_gap.percentile(0.50) / 1e6, h_gap.percentile(0.99) / 1e6);

    if (csv)
    {
        FILE* f = std::fopen(csv, "a");
        if (f)
        {
            // label,streams,chunks,completed,chunk_p50_us,chunk_p99_us,chunk_p999_us,
            // chunk_max_us,ttft_p50_ms,ttft_p99_ms,gap_p50_ms,gap_p99_ms
            std::fprintf(f, "%s,%d,%ld,%ld,%llu,%llu,%llu,%llu,%.3f,%.3f,%.3f,%.3f\n",
                         label, streams, total_chunks, completed,
                         (unsigned long long)(h_chunk.percentile(0.50) / 1000),
                         (unsigned long long)(h_chunk.percentile(0.99) / 1000),
                         (unsigned long long)(h_chunk.percentile(0.999) / 1000),
                         (unsigned long long)(h_chunk.max() / 1000),
                         h_ttft.percentile(0.50) / 1e6, h_ttft.percentile(0.99) / 1e6,
                         h_gap.percentile(0.50) / 1e6, h_gap.percentile(0.99) / 1e6);
            std::fclose(f);
        }
    }
    return 0;
}
