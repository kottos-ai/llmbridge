// Kottos gateway daemon.
//
//   kottos [--listen PORT] [--upstream IP:PORT] [--duration SECONDS]
//          [--warmup SECONDS] [--translate none|anthropic]
//
// One self-contained event-loop class (kottos::Gateway) does everything:
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
#include <string>
#include <thread>

#include "gateway/gateway.hpp"

namespace
{
    kottos::Gateway* g_gateway = nullptr;
    void on_signal(int) noexcept
    {
        if (g_gateway) g_gateway->request_stop();
    }
} // namespace

int main(int argc, char** argv)
{
    uint16_t listen_port = 8088;
    std::string upstream_ip = "127.0.0.1";
    uint16_t upstream_port = 9001;
    int duration = 0;
    double warmup = 0;
    kottos::TranslateMode translate = kottos::TranslateMode::None;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
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
        else if (a == "--translate")
        {
            if (const char* v = nextarg())
            {
                std::string mode(v);
                if (mode == "anthropic") translate = kottos::TranslateMode::Anthropic;
                else if (mode == "gemini") translate = kottos::TranslateMode::Gemini;
                else if (mode == "cohere") translate = kottos::TranslateMode::Cohere;
                else translate = kottos::TranslateMode::None;
            }
        }
        else if (a == "--help" || a == "-h")
        {
            std::printf("usage: %s [--listen PORT] [--upstream IP:PORT] "
                        "[--duration SECONDS] [--warmup SECONDS] "
                        "[--translate none|anthropic|gemini|cohere]\n", argv[0]);
            return 0;
        }
    }

    std::signal(SIGPIPE, SIG_IGN);

    kottos::Gateway gateway(listen_port, upstream_ip, upstream_port,
                            static_cast<int64_t>(warmup * 1e9), translate);
    g_gateway = &gateway;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::thread timer;
    if (duration > 0)
    {
        timer = std::thread([&gateway, duration] {
            std::this_thread::sleep_for(std::chrono::seconds(duration));
            gateway.request_stop();
        });
    }

    gateway.run();

    if (timer.joinable()) timer.join();

    const kottos::Stats& s = gateway.stats();
    std::fprintf(stderr, "\n=== Kottos gateway — added-latency profile ===\n");
    std::fprintf(stderr, "requests=%llu  errors=%llu  upstream_conns_opened=%llu\n",
                 (unsigned long long)s.requests, (unsigned long long)s.errors,
                 (unsigned long long)s.upstream_conns_opened);
    s.overhead.print(std::cerr, "added-total  ");
    s.req_path.print(std::cerr, "  request-path ");
    s.resp_path.print(std::cerr, "  response-path");
    return 0;
}
