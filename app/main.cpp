// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// llmbridge gateway daemon.
//
//   llmbridge [--listen PORT] [--upstream IP:PORT] [--duration SECONDS]
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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gateway/gateway.hpp"

namespace
{
    std::vector<std::unique_ptr<llmbridge::Gateway>>* g_gateways = nullptr;
    void on_signal(int) noexcept
    {
        if (g_gateways)
            for (auto& g : *g_gateways) g->request_stop();
    }
} // namespace

int main(int argc, char** argv)
{
    uint16_t listen_port = 8088;
    std::string upstream_ip = "127.0.0.1";
    uint16_t upstream_port = 9001;
    int duration = 0;
    double warmup = 0;
    // Seconds of upstream silence before a request/stream is aborted (0 = off).
    double up_timeout = static_cast<double>(llmbridge::Gateway::kDefaultUpstreamIdleNs) / 1e9;
    int workers = 1;
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
            if (const char* v = nextarg())
            {
                const char* colon = std::strrchr(v, ':');
                if (colon)
                {
                    upstream_ip.assign(v, colon - v);
                    upstream_port = static_cast<uint16_t>(std::atoi(colon + 1));
                }
            }
        }
        else if (a == "--duration") { if (const char* v = nextarg()) duration = std::atoi(v); }
        else if (a == "--warmup")   { if (const char* v = nextarg()) warmup = std::atof(v); }
        else if (a == "--upstream-timeout") { if (const char* v = nextarg()) up_timeout = std::atof(v); }
        else if (a == "--workers")  { if (const char* v = nextarg()) workers = std::atoi(v); }
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
            std::printf("usage: %s [--listen PORT] [--upstream IP:PORT] "
                        "[--duration SECONDS] [--warmup SECONDS] "
                        "[--translate none|anthropic|gemini|cohere] "
                        "[--upstream-timeout SECONDS] "
                        "[--io auto|epoll|uring] [--workers N]\n", argv[0]);
            return 0;
        }
    }
    if (workers < 1) workers = 1;

    std::signal(SIGPIPE, SIG_IGN);

    // N shared-nothing workers, each its own event loop binding the same port with
    // SO_REUSEPORT — the kernel load-balances connections across them. No locks on
    // the hot path; per-worker upstream pools and stats, merged at the end.
    const int64_t warmup_ns = static_cast<int64_t>(warmup * 1e9);
    const int64_t up_timeout_ns = static_cast<int64_t>(up_timeout * 1e9);
    std::vector<std::unique_ptr<llmbridge::Gateway>> gateways;
    for (int i = 0; i < workers; ++i)
        gateways.push_back(std::make_unique<llmbridge::Gateway>(
            listen_port, upstream_ip, upstream_port, warmup_ns, translate, io, up_timeout_ns));
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
        agg.upstream_conns_opened += s.upstream_conns_opened;
        agg.upstream_retries += s.upstream_retries;
        agg.overhead.merge(s.overhead);
        agg.req_path.merge(s.req_path);
        agg.resp_path.merge(s.resp_path);
    }
    std::fprintf(stderr, "\n=== llmbridge gateway — added-latency profile (%d worker%s) ===\n",
                 workers, workers == 1 ? "" : "s");
    std::fprintf(stderr, "requests=%llu  errors=%llu  upstream_conns_opened=%llu  retries=%llu\n",
                 (unsigned long long)agg.requests, (unsigned long long)agg.errors,
                 (unsigned long long)agg.upstream_conns_opened, (unsigned long long)agg.upstream_retries);
    agg.overhead.print(std::cerr, "added-total  ");
    agg.req_path.print(std::cerr, "  request-path ");
    agg.resp_path.print(std::cerr, "  response-path");
    return 0;
}
