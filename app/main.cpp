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
//          [--warmup SECONDS] [--translate none|anthropic|gemini|cohere|bedrock|azure]
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
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "config.hpp"
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

    /// A venue dialect by name, or OpenAI for "openai" and its old spelling "none".
    /// Callers validate first: an unknown string must be refused, never defaulted.
    llmbridge::UpstreamDialect dialect_from(const std::string& s)
    {
        return s == "anthropic" ? llmbridge::UpstreamDialect::Anthropic
               : s == "gemini"  ? llmbridge::UpstreamDialect::Gemini
               : s == "cohere"  ? llmbridge::UpstreamDialect::Cohere
               : s == "bedrock" ? llmbridge::UpstreamDialect::Bedrock
               : s == "azure"   ? llmbridge::UpstreamDialect::Azure
                                : llmbridge::UpstreamDialect::OpenAI;
    }

    std::vector<std::unique_ptr<llmbridge::Gateway>>* g_gateways = nullptr;
    void on_signal(int) noexcept
    {
        if (g_gateways)
            for (auto& g : *g_gateways) g->request_stop();
    }

    // SIGUSR1 prints the profile without stopping anything. Until this existed the
    // only way to read `accept(TLS)` on a running gateway was to kill it.
    void on_dump_signal(int) noexcept
    {
        if (g_gateways)
            for (auto& g : *g_gateways) g->request_stats_dump();
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
    double client_idle = static_cast<double>(llmbridge::Gateway::kDefaultClientIdleNs) / 1e9;
    double pool_idle = static_cast<double>(llmbridge::Gateway::kDefaultPoolIdleNs) / 1e9;
    std::string log_level = "info";
    int workers = 1;
    bool timing_headers = false;
    // Inbound TLS. One listener, one mode: --listen-tls makes the single listener
    // TLS-only, so "am I exposed in the clear?" is answerable from the command
    // line. There is deliberately no second plaintext port.
    bool listen_tls = false;
    std::string tls_cert, tls_key;
    llmbridge::UpstreamDialect dialect = llmbridge::UpstreamDialect::OpenAI;
    llmbridge::IoBackend io = llmbridge::IoBackend::Auto;

    // --config is applied first and flags overwrite it, so the CLI always wins and a
    // one-off override needs no file edit. bench/*.sh drives this daemon with eight
    // flags and must keep working, so the file is additive, never a replacement.
    // --config is applied first and flags overwrite it, so the CLI always wins and a
    // one-off override needs no file edit. Precedence is not positional: a flag wins
    // whether it sits before or after --config. bench/*.sh drives this daemon with
    // eight flags and must keep working, so the file is additive, never a replacement.
    //
    // Argument validation runs before any I/O, so `--config a --config b` reports the
    // duplicate instead of whichever file happened to be unreadable first.
    const char* config_path = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) != "--config") continue;
        if (i + 1 >= argc)
        {
            return refuse("--config needs a path");
        }
        // Refuse a second --config instead of quietly using the first. Silently
        // discarding the operator's later file is the same fail-open shape the parser
        // refuses for unknown keys, and it is invisible from the outside.
        if (config_path)
        {
            return refuse("--config given twice (" + std::string(config_path) + " and " +
                          argv[i + 1] + "); pass it once");
        }
        config_path = argv[i + 1];
    }

    // Config-only, with no flag equivalent: a list does not fit a flag without
    // inventing a separator, which is the argument that put --config in first.
    std::vector<std::string> strip_headers;
    // Venues past the first, from a config file only.
    std::vector<llmbridge::app::ConfigFile::UpstreamEntry> extra_upstreams;

    if (config_path)
    {
        llmbridge::app::ConfigFile cfg;
        std::string cfg_err;
        if (!llmbridge::app::load_config(config_path, cfg, cfg_err))
        {
            // Fail closed and name the key. A config that half-applies is how an
            // operator ends up believing a setting took effect when it did not.
            return refuse(cfg_err);
        }
        if (cfg.has_listen_port) listen_port = cfg.listen_port;
        if (cfg.has_listen_tls) listen_tls = cfg.listen_tls;
        if (!cfg.tls_cert.empty()) tls_cert = cfg.tls_cert;
        if (!cfg.tls_key.empty()) tls_key = cfg.tls_key;
        strip_headers = cfg.strip_headers;
        // Entry 0 feeds the flags, so --upstream and --translate keep overriding a
        // one-upstream file exactly as before. Entries beyond the first can only come
        // from a file, since a list has no flag spelling.
        if (!cfg.upstreams.empty())
        {
            if (!cfg.upstreams[0].url.empty()) upstream_arg = cfg.upstreams[0].url;
            if (!cfg.upstreams[0].dialect.empty()) dialect = dialect_from(cfg.upstreams[0].dialect);
            extra_upstreams.assign(cfg.upstreams.begin() + 1, cfg.upstreams.end());
        }
        if (cfg.has_upstream_s) up_timeout = cfg.upstream_s;
        if (cfg.has_client_idle_s) client_idle = cfg.client_idle_s;
        if (cfg.has_pool_idle_s) pool_idle = cfg.pool_idle_s;
        if (!cfg.io.empty())
            io = cfg.io == "epoll"   ? llmbridge::IoBackend::Epoll
                 : cfg.io == "uring" ? llmbridge::IoBackend::Uring
                                     : llmbridge::IoBackend::Auto;
        if (!cfg.log_level.empty()) log_level = cfg.log_level;
        if (cfg.has_workers) workers = cfg.workers;
        if (cfg.has_timing_headers) timing_headers = cfg.timing_headers;
        if (cfg.has_duration_s) duration = static_cast<int>(cfg.duration_s);
        if (cfg.has_warmup_s) warmup = cfg.warmup_s;
    }

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        auto nextarg = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--config") { (void)nextarg(); continue; } // already applied above
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
        else if (a == "--client-idle")      { if (const char* v = nextarg()) client_idle = std::atof(v); }
        else if (a == "--pool-idle")        { if (const char* v = nextarg()) pool_idle = std::atof(v); }
        else if (a == "--log-level")        { if (const char* v = nextarg()) log_level = v; }
        else if (a == "--workers")  { if (const char* v = nextarg()) workers = std::atoi(v); }
        else if (a == "--timing-headers") timing_headers = true;
        else if (a == "--listen-tls") listen_tls = true;
        else if (a == "--tls-cert") { if (const char* v = nextarg()) tls_cert = v; }
        else if (a == "--tls-key")  { if (const char* v = nextarg()) tls_key = v; }
        else if (a == "--translate")
            return refuse("--translate is now --upstream-dialect, and names what the "
                          "venue speaks instead of an action we may not perform");
        else if (a == "--upstream-dialect")
        {
            const char* v = nextarg();
            if (v == nullptr) return refuse(std::string(a) + " needs a dialect");
            const std::string mode(v);
            // Validated, never defaulted: an unknown value used to become an
            // OpenAI venue silently, so a typo turned an Anthropic upstream into a
            // mistranslated request carrying a live credential.
            if (mode == "none")
                return refuse("upstream dialect 'none' is now 'openai': it names an "
                              "OpenAI-compatible venue, never an absence");
            if (mode != "openai" && mode != "anthropic" && mode != "gemini" &&
                mode != "cohere" && mode != "bedrock" && mode != "azure")
                return refuse("unknown upstream dialect '" + mode +
                              "'; use openai|anthropic|gemini|cohere|bedrock|azure");
            dialect = dialect_from(mode);
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
                        "[--upstream-dialect openai|anthropic|gemini|cohere|bedrock|azure] "
                        "[--upstream-timeout SECONDS] [--client-idle SECONDS] [--pool-idle SECONDS] "
                        "[--log-level trace|debug|info|warn|error|off] "
                        "[--listen-tls --tls-cert PATH --tls-key PATH] "
                        "[--io auto|epoll|uring] [--workers N] [--timing-headers] [--config FILE]\n", argv[0]);
            return 0;
        }
        else
        {
            std::string m = "unknown argument '" + std::string(a) + "'";
            if (const size_t eq = a.find('='); eq != std::string_view::npos)
                m += "; flags take a separate value, so write '" + std::string(a.substr(0, eq)) +
                     " " + std::string(a.substr(eq + 1)) + "'";
            return refuse(m + ". Run --help for the accepted flags.");
        }
    }
    llmbridge::net::log::register_thread("main", 0);
    {
        llmbridge::net::log::Level lv{};
        if (!llmbridge::net::log::level_from_name(log_level, lv))
        {
            return refuse("unknown --log-level '" + log_level +
                          "'; expected one of trace debug info warn error off");
        }
        llmbridge::net::log::set_level(lv);
    }

    if (workers < 1) workers = 1;

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
        LB_WARN(up.host, " resolved to ", static_cast<int64_t>(ips.size()),
                " addresses; using ", upstream_ip,
                " (list several venues under \"upstream\" to reach the rest)");

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
                            .dialect = dialect,
                            .base_path = up.path,
                            .query = up.query});
    for (const auto& e : extra_upstreams)
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
                                                     .query = s2.query});
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
        tls.sni_host = up.host; // SNI + hostname verification: the parsed host,
                                // never the resolved IP (verification needs the name)
        tls.client_tls = listen_tls;
        tls.cert_file = tls_cert;
        tls.key_file = tls_key;
        auto gw = std::make_unique<llmbridge::Gateway>(
            listen_port, upstream_table, warmup_ns, io, up_timeout_ns, tls, timing_headers,
            nullptr, strip_headers);
        // Before run(): the loop thread reads it, so setting it later is a data race.
        gw->set_client_idle_ns(static_cast<int64_t>(client_idle * 1e9));
        gw->set_pool_idle_ns(static_cast<int64_t>(pool_idle * 1e9));
        gateways.push_back(std::move(gw));
    }
    g_gateways = &gateways;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGUSR1, on_dump_signal);

    std::thread timer;
    if (duration > 0)
    {
        timer = std::thread([&gateways, duration] {
            std::this_thread::sleep_for(std::chrono::seconds(duration));
            for (auto& g : gateways) g->request_stop();
        });
    }

    std::vector<std::thread> threads;
    unsigned widx = 0;
    for (auto& g : gateways)
        threads.emplace_back([gp = g.get(), i = widx++] {
            // Before anything the loop logs, so every line names its worker.
            llmbridge::net::log::register_thread("worker", i);
            gp->run();
        });
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
        agg.client_idle_timeouts += s.client_idle_timeouts;
        agg.stream_pauses += s.stream_pauses;
        agg.uring_enobufs += s.uring_enobufs;
        agg.upstream_conns_opened += s.upstream_conns_opened;
        agg.upstream_retries += s.upstream_retries;
        agg.upstream_reused += s.upstream_reused;
        agg.upstream_unsent += s.upstream_unsent;
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
                 "stream_pauses=%llu  uring_enobufs=%llu\n",
                 (unsigned long long)agg.upstream_timeouts,
                 (unsigned long long)agg.client_setup_timeouts,
                 (unsigned long long)agg.client_idle_timeouts, (unsigned long long)agg.stream_pauses,
                 (unsigned long long)agg.uring_enobufs);
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
