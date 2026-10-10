// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// llmbridge gateway daemon: flags and an optional --config file (app/options.hpp, --help),
// N shared-nothing gateway workers, and the added-latency profile printed on exit.
// Defaults: listen :8088, upstream 127.0.0.1:9001. With --duration the daemon self-stops
// after that many seconds (for scripted benchmark runs); otherwise it runs until
// SIGINT/SIGTERM.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "options.hpp"
#include "gateway/gateway.hpp"
#include "net/log.hpp"
#include "net/upstream.hpp"

namespace
{
    /// One startup refusal, two destinations, and the exit code the caller returns.
    ///
    /// stderr is for the operator running this by hand; the log is for the journal
    /// when systemd started it, where stderr also lands but without a level or a
    /// timestamp to filter on. Both, deliberately, and through one function so a
    /// message cannot drift between them.
    ///
    /// `loc` defaults to the call site, and emits through the same path LB_ERROR
    /// does. An LB_ERROR inside this function would stamp every refusal in the
    /// binary with this one line number, which is worse than no line number: it
    /// looks like a location and is not one.
    int refuse(const std::string& msg,
               const std::source_location loc = std::source_location::current())
    {
        std::fprintf(stderr, "llmbridge: %s\n", msg.c_str());
        namespace log = llmbridge::net::log;
        if constexpr (static_cast<int>(log::Level::Error) >= LLMBRIDGE_LOG_COMPILE_LEVEL)
            if (log::enabled(log::Level::Error))
                log::emit(log::Level::Error, loc.file_name(), static_cast<int>(loc.line()),
                          msg.c_str());
        return 2;
    }

    /// A venue dialect by name. The option table has already refused any other word,
    /// so the OpenAI fallthrough is reached by "openai" only.
    llmbridge::UpstreamDialect dialect_from(const std::string& s)
    {
        return s == "anthropic" ? llmbridge::UpstreamDialect::Anthropic
               : s == "gemini"  ? llmbridge::UpstreamDialect::Gemini
               : s == "cohere"  ? llmbridge::UpstreamDialect::Cohere
               : s == "bedrock" ? llmbridge::UpstreamDialect::Bedrock
               : s == "azure"   ? llmbridge::UpstreamDialect::Azure
                                : llmbridge::UpstreamDialect::OpenAI;
    }

    // Atomic so a handler never sees a half-cleared pointer; cleared before the
    // gateways it points at are destroyed.
    using Gateways = std::vector<std::unique_ptr<llmbridge::Gateway>>;
    std::atomic<Gateways*> g_gateways{nullptr};
    void on_signal(int) noexcept
    {
        if (Gateways* gs = g_gateways.load())
            for (auto& g : *gs) g->request_stop();
    }

    // SIGUSR1 prints the profile without stopping anything. Until this existed the
    // only way to read `accept(TLS)` on a running gateway was to kill it.
    void on_dump_signal(int) noexcept
    {
        if (Gateways* gs = g_gateways.load())
            for (auto& g : *gs) g->request_stats_dump();
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
    llmbridge::app::Settings cfg;
    std::string err;
    switch (llmbridge::app::parse_cli(std::span<char* const>(argv + 1, argv + argc), cfg, err))
    {
    case llmbridge::app::Cli::Help:
        std::fputs(llmbridge::app::help_text(argv[0]).c_str(), stdout);
        return 0;
    case llmbridge::app::Cli::Refused:
        // Fail closed and name the setting: a half-applied config is how an operator
        // ends up believing a setting took effect when it did not.
        return refuse(err);
    case llmbridge::app::Cli::Run:
        break;
    }
    const uint16_t listen_port = static_cast<uint16_t>(cfg.listen_port);
    const bool listen_tls = cfg.listen_tls;
    const std::string& tls_cert = cfg.tls_cert;
    const std::string& tls_key = cfg.tls_key;
    const std::string& upstream_arg = cfg.upstream;
    const int workers = cfg.workers;

    llmbridge::net::log::register_thread("main", 0);
    {
        llmbridge::net::log::Level lv{};
        if (!llmbridge::net::log::level_from_name(cfg.log_level, lv))
            return refuse("unknown log level '" + cfg.log_level + "'");
        llmbridge::net::log::set_level(lv);
    }

    if (listen_tls && (tls_cert.empty() || tls_key.empty()))
    {
        return refuse("a TLS listener needs a certificate and a key (--tls-cert/--tls-key, "
                      "or listen.cert/listen.key in a config file)");
    }
    if (!listen_tls && (!tls_cert.empty() || !tls_key.empty()))
    {
        // A certificate given without --listen-tls means the operator believes the
        // listener is encrypted when it is not. Refuse instead of ignoring it.
        return refuse("a certificate/key was given without enabling the TLS listener "
                      "(--listen-tls, or listen.tls in a config file)");
    }
#ifndef LLMBRIDGE_HAVE_TLS
    if (listen_tls)
    {
        // The dangerous direction of the guard above, and the one that fails open:
        // without this the flag is accepted, the listener serves plaintext, and the
        // operator has every client credential on the wire believing otherwise. The
        // upstream leg refuses the mirror case a few lines down.
        return refuse("a TLS listener (--listen-tls, or listen.tls in a config file) requires "
                      "a TLS build. Reconfigure with -DLLMBRIDGE_TLS=ON (needs OpenSSL). "
                      "Refusing to serve plaintext on a listener asked to be TLS.");
    }
#endif

    // Parse + resolve --upstream. Resolution happens once, here, on the setup path:
    // the workers get a dotted-quad and never touch the resolver. (Re-resolution on
    // TTL expiry is future work: providers rotate IPs, but a long-lived gateway
    // pinning one A record is exactly what the pooled connections do anyway.)
    const llmbridge::net::UpstreamSpec up = llmbridge::net::parse_upstream(upstream_arg);
    if (!up.ok())
    {
        return refuse("bad --upstream '" + upstream_arg + "': " + up.error);
    }
#ifndef LLMBRIDGE_HAVE_TLS
    if (up.tls)
    {
        return refuse("https:// upstream requires a TLS build. Reconfigure with "
                      "-DLLMBRIDGE_TLS=ON (needs OpenSSL). Refusing to speak plaintext to a "
                      "TLS port.");
    }
#endif
    std::string resolve_err;
    const std::vector<std::string> ips = llmbridge::net::resolve_host_ipv4(up.host, &resolve_err);
    if (ips.empty())
    {
        return refuse("cannot resolve upstream host '" + up.host + "': " + resolve_err);
    }
    const std::string& upstream_ip = ips.front();
    const uint16_t upstream_port = up.port;
    if (ips.size() > 1)
        LB_INFO(up.host, " resolved to ", static_cast<int64_t>(ips.size()),
                " addresses; dialling ", upstream_ip,
                " first and the rest after a failed connect");

    // Credentials over plaintext: the gateway forwards the client's provider key
    // upstream, so a non-TLS upstream that is not loopback puts that key on the
    // wire in the clear. Loopback is exempt; that is the benchmark/mock setup and
    // the sidecar deployment, where there is no network to sniff.
    {
        const bool loopback = upstream_ip.rfind("127.", 0) == 0 || upstream_ip == "::1";
        if (!up.tls && !loopback)
            LB_WARN("upstream is plaintext HTTP and not loopback; any client credential "
                    "forwarded to it travels UNENCRYPTED, use https:// for a real provider. "
                    "host=", up.host, " port=", upstream_port);
    }

    // The table the workers index. Entry 0 is what the flags describe; the rest are
    // parsed and resolved identically, because a venue reached only from a file must
    // fail as loudly at startup as one named on the command line.
    std::vector<llmbridge::Upstream> upstream_table;
    upstream_table.push_back(
        llmbridge::Upstream{.ip = upstream_ip,
                            .port = upstream_port,
                            .tls = up.tls,
                            .sni_host = up.host,
                            .dialect = dialect_from(cfg.dialect),
                            .base_path = up.path,
                            .query = up.query,
                            .ips = ips,
                            .host = up.host});
    for (const auto& e : cfg.more_upstreams)
    {
        const llmbridge::net::UpstreamSpec s2 = llmbridge::net::parse_upstream(e.url);
        if (!s2.ok())
        {
            return refuse("bad upstream '" + e.url + "': " + s2.error);
        }
#ifndef LLMBRIDGE_HAVE_TLS
        if (s2.tls)
        {
            return refuse("https:// upstream requires a TLS build");
        }
#endif
        std::string e2;
        const std::vector<std::string> ips2 = llmbridge::net::resolve_host_ipv4(s2.host, &e2);
        if (ips2.empty())
        {
            return refuse("cannot resolve upstream host '" + s2.host + "': " + e2);
        }
        upstream_table.push_back(llmbridge::Upstream{.ip = ips2.front(),
                                                     .port = s2.port,
                                                     .tls = s2.tls,
                                                     .sni_host = s2.host,
                                                     .dialect = dialect_from(e.dialect),
                                                     .base_path = s2.path,
                                                     .query = s2.query,
                                                     .ips = ips2,
                                                     .host = s2.host});
    }

    std::signal(SIGPIPE, SIG_IGN);

    // N shared-nothing workers, each its own event loop binding the same port with
    // SO_REUSEPORT: the kernel load-balances connections across them. No locks on
    // the hot path; per-worker upstream pools and stats, merged at the end.
    const int64_t warmup_ns = static_cast<int64_t>(cfg.warmup_s * 1e9);
    const int64_t up_timeout_ns = static_cast<int64_t>(cfg.upstream_s * 1e9);
    const llmbridge::IoBackend io = cfg.io == "epoll"   ? llmbridge::IoBackend::Epoll
                                    : cfg.io == "uring" ? llmbridge::IoBackend::Uring
                                                        : llmbridge::IoBackend::Auto;
    Gateways gateways;
    for (int i = 0; i < workers; ++i)
    {
        llmbridge::TlsConfig tls;
        tls.upstream_tls = up.tls;
        tls.sni_host = up.host; // SNI + hostname verification: the parsed host,
                                // never the resolved IP (verification needs the name)
        tls.client_tls = listen_tls;
        tls.cert_file = tls_cert;
        tls.key_file = tls_key;
        auto gw = std::make_unique<llmbridge::Gateway>(
            listen_port, upstream_table, warmup_ns, io, up_timeout_ns, tls, cfg.timing_headers,
            nullptr, cfg.strip_headers);
        // Before run(): the loop thread reads it, so setting it later is a data race.
        gw->set_client_idle_ns(static_cast<int64_t>(cfg.client_idle_s * 1e9));
        gw->set_pool_idle_ns(static_cast<int64_t>(cfg.pool_idle_s * 1e9));
        gw->set_connect_ns(static_cast<int64_t>(cfg.connect_s * 1e9));
        gw->set_prefault_bytes(static_cast<size_t>(cfg.prefault_mb * (1 << 20)));
        gateways.push_back(std::move(gw));
    }
    g_gateways.store(&gateways);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGUSR1, on_dump_signal);

    // The stop condition: the first worker to return (a signal stops them all) or the
    // --duration deadline, whichever comes first. Waiting on it, not sleeping, is what
    // lets SIGINT end a --duration run at once, and a worker that dies takes the others
    // down instead of leaving them serving.
    std::mutex m;
    std::condition_variable cv;
    bool finished = false;
    std::vector<std::thread> threads;
    unsigned widx = 0;
    for (auto& g : gateways)
        threads.emplace_back([gp = g.get(), i = widx++, &m, &cv, &finished] {
            // Before anything the loop logs, so every line names its worker.
            llmbridge::net::log::register_thread("worker", i);
            gp->run();
            const std::lock_guard<std::mutex> lk(m);
            finished = true;
            cv.notify_all();
        });
    {
        std::unique_lock<std::mutex> lk(m);
        if (cfg.duration_s > 0)
            cv.wait_for(lk, std::chrono::duration<double>(cfg.duration_s), [&] { return finished; });
        else
            cv.wait(lk, [&] { return finished; });
    }
    for (auto& g : gateways) g->request_stop();
    for (auto& t : threads) t.join();
    // Only this thread is left, so a handler now runs here or not at all.
    g_gateways.store(nullptr);

    // Aggregate per-worker stats into one profile.
    llmbridge::Stats agg;
    for (const auto& g : gateways)
    {
        const llmbridge::Stats& s = g->stats();
        agg.requests += s.requests;
        agg.errors += s.errors;
        agg.upstream_timeouts += s.upstream_timeouts;
        agg.client_setup_timeouts += s.client_setup_timeouts;
        agg.client_idle_timeouts += s.client_idle_timeouts;
        agg.stream_pauses += s.stream_pauses;
        agg.uring_enobufs += s.uring_enobufs;
        agg.upstream_conns_opened += s.upstream_conns_opened;
        agg.upstream_retries += s.upstream_retries;
        agg.upstream_reused += s.upstream_reused;
        agg.upstream_unsent += s.upstream_unsent;
        agg.cold_builds += s.cold_builds;
        agg.warm_reuses += s.warm_reuses;
        agg.tls_out_grows += s.tls_out_grows;
        if (s.tls_buffered_peak > agg.tls_buffered_peak)
            agg.tls_buffered_peak = s.tls_buffered_peak;
        agg.overhead.merge(s.overhead);
        agg.req_path.merge(s.req_path);
        agg.connect.merge(s.connect);
        agg.accept_tls.merge(s.accept_tls);
        agg.resp_path.merge(s.resp_path);
        agg.first_token.merge(s.first_token);
    }
    std::fprintf(stderr, "\n=== llmbridge gateway: added-latency profile (%d worker%s) ===\n",
                 workers, workers == 1 ? "" : "s");
    std::fprintf(stderr, "timeouts=%llu  client_setup_timeouts=%llu  client_idle_timeouts=%llu  "
                 "stream_pauses=%llu  uring_enobufs=%llu  cold_builds=%llu  warm_reuses=%llu  tls_out_grows=%llu\n",
                 (unsigned long long)agg.upstream_timeouts,
                 (unsigned long long)agg.client_setup_timeouts,
                 (unsigned long long)agg.client_idle_timeouts, (unsigned long long)agg.stream_pauses,
                 (unsigned long long)agg.uring_enobufs, (unsigned long long)agg.cold_builds,
                 (unsigned long long)agg.warm_reuses, (unsigned long long)agg.tls_out_grows);
    std::fprintf(stderr,
                 "requests=%llu  errors=%llu  upstream_conns_opened=%llu  reused=%llu  retries=%llu  "
                 "unsent=%llu\n",
                 (unsigned long long)agg.requests, (unsigned long long)agg.errors,
                 (unsigned long long)agg.upstream_conns_opened,
                 (unsigned long long)agg.upstream_reused, (unsigned long long)agg.upstream_retries,
                 (unsigned long long)agg.upstream_unsent);
    agg.overhead.print(std::cerr, "added-total  ");
    agg.req_path.print(std::cerr, "  request-path ");
    agg.connect.print(std::cerr, "  connect(TLS) ");
    // Only when it has samples. A plaintext listener never terminates a handshake,
    // and a permanent "(no samples)" line for a feature that is off reads as a
    // missing measurement. This is the one handshake that is ours; see LATENCY.md 1.
    if (agg.accept_tls.total() > 0) agg.accept_tls.print(std::cerr, "accept(TLS)   ");
    agg.resp_path.print(std::cerr, "  response-path");
    // Printed only when it has samples, like accept(TLS): absent here means
    // inapplicable, not missing. Top level, since it is not part of added-total.
    if (agg.first_token.total() > 0) agg.first_token.print(std::cerr, "first-token  ");
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
        LB_ERROR("fatal during startup: ", e.what());
        return 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "llmbridge: unknown fatal error during startup\n");
        LB_ERROR("unknown fatal error during startup");
        return 1;
    }
}
