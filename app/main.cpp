// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// llmbridge gateway daemon.
//
//   llmbridge [--listen PORT] [--upstream IP:PORT|HOST:PORT|http(s)://HOST[:PORT]]
//          [--duration SECONDS]
//          [--warmup SECONDS] [--translate none|anthropic|gemini|cohere]
//          [--upstream-timeout SECONDS]
//          [--io auto|epoll|uring]
//
// One self-contained event-loop class (llmbridge::Gateway) does everything:
// accept clients, frame requests, optionally translate the provider dialect,
// forward over a keep-alive upstream pool, translate the response back, reply.
// Defaults: listen :8088, upstream 127.0.0.1:9001. With --duration the daemon
// self-stops after N seconds and dumps the added-latency profile (for scripted
// benchmark runs); otherwise it runs until SIGINT/SIGTERM.

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gateway/gateway.hpp"
#include "net/upstream.hpp"

namespace
{
    std::vector<std::unique_ptr<llmbridge::Gateway>>* g_gateways = nullptr;
    void on_signal(int) noexcept
    {
        if (g_gateways)
            for (auto& g : *g_gateways) g->request_stop();
    }
} // namespace

// The real body. `main` below turns any escaping exception into a single
// readable line, because a startup failure that reaches the default handler
// prints "terminate called after throwing an instance of ..." and buries the
// message an operator actually needs. Every throw on this path is a setup
// failure with a specific cause: an unreadable certificate, a key with the
// wrong mode, a key that does not match, an expired certificate, a port already
// bound. Each of those deserves to be legible on the first read.
static int run(int argc, char** argv)
{
    uint16_t listen_port = 8088;
    std::string upstream_arg = "127.0.0.1:9001";
    int duration = 0;
    double warmup = 0;
    // Seconds of upstream silence before a request/stream is aborted (0 = off).
    double up_timeout = static_cast<double>(llmbridge::Gateway::kDefaultUpstreamIdleNs) / 1e9;
    int workers = 1;
    bool timing_headers = false;
    // Inbound TLS. ONE LISTENER, ONE MODE: --listen-tls makes the single listener
    // TLS-only, so "am I exposed in the clear?" is answerable from the command
    // line. There is deliberately no second plaintext port.
    bool listen_tls = false;
    std::string tls_cert, tls_key;
    llmbridge::TranslateMode translate = llmbridge::TranslateMode::None;
    llmbridge::IoBackend io = llmbridge::IoBackend::Auto;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        auto nextarg = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--listen")
        {
            if (const char* v = nextarg()) listen_port = static_cast<uint16_t>(std::atoi(v));
        }
        else if (a == "--upstream")
        {
            if (const char* v = nextarg()) upstream_arg = v;
        }
        else if (a == "--duration") { if (const char* v = nextarg()) duration = std::atoi(v); }
        else if (a == "--warmup")   { if (const char* v = nextarg()) warmup = std::atof(v); }
        else if (a == "--upstream-timeout") { if (const char* v = nextarg()) up_timeout = std::atof(v); }
        else if (a == "--workers")  { if (const char* v = nextarg()) workers = std::atoi(v); }
        else if (a == "--timing-headers") timing_headers = true;
        else if (a == "--listen-tls") listen_tls = true;
        else if (a == "--tls-cert") { if (const char* v = nextarg()) tls_cert = v; }
        else if (a == "--tls-key")  { if (const char* v = nextarg()) tls_key = v; }
        else if (a == "--translate")
        {
            if (const char* v = nextarg())
            {
                std::string mode(v);
                if (mode == "anthropic") translate = llmbridge::TranslateMode::Anthropic;
                else if (mode == "gemini") translate = llmbridge::TranslateMode::Gemini;
                else if (mode == "cohere") translate = llmbridge::TranslateMode::Cohere;
                else translate = llmbridge::TranslateMode::None;
            }
        }
        else if (a == "--io")
        {
            if (const char* v = nextarg())
            {
                std::string mode(v);
                if (mode == "epoll") io = llmbridge::IoBackend::Epoll;
                else if (mode == "uring") io = llmbridge::IoBackend::Uring;
                else io = llmbridge::IoBackend::Auto;
            }
        }
        else if (a == "--help" || a == "-h")
        {
            std::printf("usage: %s [--listen PORT] "
                        "[--upstream IP:PORT|HOST:PORT|http(s)://HOST[:PORT]] "
                        "[--duration SECONDS] [--warmup SECONDS] "
                        "[--translate none|anthropic|gemini|cohere] "
                        "[--upstream-timeout SECONDS] "
                        "[--listen-tls --tls-cert PATH --tls-key PATH] "
                        "[--io auto|epoll|uring] [--workers N] [--timing-headers]\n", argv[0]);
            return 0;
        }
    }
    if (workers < 1) workers = 1;

    if (listen_tls && (tls_cert.empty() || tls_key.empty()))
    {
        std::fprintf(stderr, "llmbridge: --listen-tls needs --tls-cert and --tls-key\n");
        return 2;
    }
    if (!listen_tls && (!tls_cert.empty() || !tls_key.empty()))
    {
        // A certificate given without --listen-tls means the operator believes the
        // listener is encrypted when it is not. Refuse instead of ignoring it.
        std::fprintf(stderr, "llmbridge: --tls-cert/--tls-key given without --listen-tls\n");
        return 2;
    }

    // Parse + resolve --upstream. Resolution happens ONCE, here, on the setup path:
    // the workers get a dotted-quad and never touch the resolver. (Re-resolution on
    // TTL expiry is future work: providers rotate IPs, but a long-lived gateway
    // pinning one A record is exactly what the pooled connections do anyway.)
    const llmbridge::net::UpstreamSpec up = llmbridge::net::parse_upstream(upstream_arg);
    if (!up.ok())
    {
        std::fprintf(stderr, "llmbridge: bad --upstream '%s': %s\n",
                     upstream_arg.c_str(), up.error.c_str());
        return 2;
    }
#ifndef LLMBRIDGE_HAVE_TLS
    if (up.tls)
    {
        std::fprintf(stderr, "llmbridge: https:// upstream requires a TLS build. "
                             "reconfigure with -DLLMBRIDGE_TLS=ON (needs OpenSSL). "
                             "Refusing to speak plaintext to a TLS port.\n");
        return 2;
    }
#endif
    std::string resolve_err;
    const std::vector<std::string> ips = llmbridge::net::resolve_host_ipv4(up.host, &resolve_err);
    if (ips.empty())
    {
        std::fprintf(stderr, "llmbridge: cannot resolve upstream host '%s': %s\n",
                     up.host.c_str(), resolve_err.c_str());
        return 2;
    }
    const std::string& upstream_ip = ips.front();
    const uint16_t upstream_port = up.port;
    if (ips.size() > 1)
        std::fprintf(stderr, "llmbridge: %s resolved to %zu addresses; using %s "
                             "(failover across the rest lands with the failover PR)\n",
                     up.host.c_str(), ips.size(), upstream_ip.c_str());

    // Credentials over plaintext: the gateway forwards the client's provider key
    // upstream, so a non-TLS upstream that is NOT loopback puts that key on the
    // wire in the clear. Loopback is exempt; that is the benchmark/mock setup and
    // the sidecar deployment, where there is no network to sniff.
    {
        const bool loopback = upstream_ip.rfind("127.", 0) == 0 || upstream_ip == "::1";
        if (!up.tls && !loopback)
            std::fprintf(stderr,
                         "llmbridge: WARNING: upstream %s:%u is plaintext HTTP and not "
                         "loopback. Any client credential forwarded to it travels "
                         "UNENCRYPTED. Use https:// for a real provider.\n",
                         up.host.c_str(), upstream_port);
    }

    std::signal(SIGPIPE, SIG_IGN);

    // N shared-nothing workers, each its own event loop binding the same port with
    // SO_REUSEPORT: the kernel load-balances connections across them. No locks on
    // the hot path; per-worker upstream pools and stats, merged at the end.
    const int64_t warmup_ns = static_cast<int64_t>(warmup * 1e9);
    const int64_t up_timeout_ns = static_cast<int64_t>(up_timeout * 1e9);
    std::vector<std::unique_ptr<llmbridge::Gateway>> gateways;
    for (int i = 0; i < workers; ++i)
    {
        llmbridge::TlsConfig tls;
        tls.upstream_tls = up.tls;
        tls.sni_host = up.host; // SNI + hostname verification: the PARSED host,
                                // never the resolved IP (verification needs the name)
        tls.client_tls = listen_tls;
        tls.cert_file = tls_cert;
        tls.key_file = tls_key;
        gateways.push_back(std::make_unique<llmbridge::Gateway>(
            listen_port, upstream_ip, upstream_port, warmup_ns, translate, io, up_timeout_ns, tls,
            timing_headers));
    }
    g_gateways = &gateways;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::thread timer;
    if (duration > 0)
    {
        timer = std::thread([&gateways, duration] {
            std::this_thread::sleep_for(std::chrono::seconds(duration));
            for (auto& g : gateways) g->request_stop();
        });
    }

    std::vector<std::thread> threads;
    for (auto& g : gateways) threads.emplace_back([gp = g.get()] { gp->run(); });
    for (auto& t : threads) t.join();
    if (timer.joinable()) timer.join();

    // Aggregate per-worker stats into one profile.
    llmbridge::Stats agg;
    for (const auto& g : gateways)
    {
        const llmbridge::Stats& s = g->stats();
        agg.requests += s.requests;
        agg.errors += s.errors;
        agg.upstream_timeouts += s.upstream_timeouts;
        agg.client_setup_timeouts += s.client_setup_timeouts;
        agg.stream_pauses += s.stream_pauses;
        agg.uring_enobufs += s.uring_enobufs;
        agg.upstream_conns_opened += s.upstream_conns_opened;
        agg.upstream_retries += s.upstream_retries;
        agg.upstream_reused += s.upstream_reused;
        agg.overhead.merge(s.overhead);
        agg.req_path.merge(s.req_path);
        agg.connect.merge(s.connect);
        agg.resp_path.merge(s.resp_path);
    }
    std::fprintf(stderr, "\n=== llmbridge gateway: added-latency profile (%d worker%s) ===\n",
                 workers, workers == 1 ? "" : "s");
    std::fprintf(stderr, "timeouts=%llu  client_setup_timeouts=%llu  stream_pauses=%llu  uring_enobufs=%llu\n",
                 (unsigned long long)agg.upstream_timeouts,
                 (unsigned long long)agg.client_setup_timeouts, (unsigned long long)agg.stream_pauses,
                 (unsigned long long)agg.uring_enobufs);
    std::fprintf(stderr, "requests=%llu  errors=%llu  upstream_conns_opened=%llu  reused=%llu  retries=%llu\n",
                 (unsigned long long)agg.requests, (unsigned long long)agg.errors,
                 (unsigned long long)agg.upstream_conns_opened,
                 (unsigned long long)agg.upstream_reused, (unsigned long long)agg.upstream_retries);
    agg.overhead.print(std::cerr, "added-total  ");
    agg.req_path.print(std::cerr, "  request-path ");
    agg.connect.print(std::cerr, "  connect(TLS) ");
    agg.resp_path.print(std::cerr, "  response-path");
    return 0;
}

int main(int argc, char** argv)
{
    try
    {
        return run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "llmbridge: %s\n", e.what());
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "llmbridge: unknown fatal error during startup\n");
        return 1;
    }
}
