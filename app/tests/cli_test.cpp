// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The flags share the config file's table, so they share its refusals. Every case
// below used to start a gateway configured differently from what the operator typed.

#include "options.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

using llmbridge::app::Cli;
using llmbridge::app::Settings;

namespace
{
    Cli parse(std::initializer_list<const char*> args, Settings& s, std::string& err)
    {
        std::vector<char*> v;
        for (const char* a : args) v.push_back(const_cast<char*>(a));
        return llmbridge::app::parse_cli(v, s, err);
    }

    constexpr const char* kGoldenHelp = R"(usage: llmbridge [--config FILE] [flags]
Flags override the file wherever they appear. In parentheses: the file key.
  --config FILE               JSON settings; see app/llmbridge.example.json
  --listen PORT               listening port, 0..65535 (listen.port)
  --listen-tls                serve TLS only; needs a certificate and a key (listen.tls)
  --tls-cert PATH             certificate chain, PEM (listen.cert)
  --tls-key PATH              private key, PEM, owner-only (listen.key)
  --upstream URL              IP:PORT, HOST:PORT or http(s)://HOST[:PORT][/BASE] (upstream.url)
  --upstream-dialect NAME     what the venue speaks: openai|anthropic|gemini|cohere|bedrock|azure (upstream.dialect)
  --upstream-timeout SECONDS  upstream silence before a request is aborted, 0 = off (timeouts.upstream_s)
  --client-idle SECONDS       idle client lifetime, 0 = off (timeouts.client_idle_s)
  --pool-idle SECONDS         idle pooled upstream lifetime, 0 = off (timeouts.pool_idle_s)
  --connect-timeout SECONDS   upstream connect and TLS handshake limit, 0 = off (timeouts.connect_s)
  --io NAME                   event loop: auto|epoll|uring (runtime.io)
  --log-level LEVEL           log level: trace|debug|info|warn|error|off (runtime.log_level)
  --workers N                 event loops sharing the port, 1..4096 (runtime.workers)
  --timing-headers            add x-llmbridge-* timing headers (runtime.timing_headers)
  --duration SECONDS          stop after this long and print the profile, 0 = until signalled (runtime.duration_s)
  --warmup SECONDS            leave the first seconds out of the histograms (runtime.warmup_s)
  --prefault-mb MB            prefault every request buffer to this size (runtime.prefault_mb)
File only:
  upstream.strip_headers      header names dropped from every upstream request
  -h, --help                  print this and exit
)";
} // namespace

class CliReject : public ::testing::TestWithParam<
                      std::pair<std::vector<const char*>, const char*>> {};

TEST_P(CliReject, IsRefusedAndNamesTheProblem)
{
    Settings s;
    std::string err;
    std::vector<char*> v;
    for (const char* a : GetParam().first) v.push_back(const_cast<char*>(a));
    EXPECT_EQ(llmbridge::app::parse_cli(v, s, err), Cli::Refused);
    EXPECT_NE(err.find(GetParam().second), std::string::npos) << err;
}

using Args = std::vector<const char*>;
INSTANTIATE_TEST_SUITE_P(
    P16, CliReject,
    ::testing::Values(
        // A port past 65535 wrapped: 123536 served 58000, 70000 served 4464.
        std::make_pair(Args{"--listen", "70000"}, "out of range"),
        std::make_pair(Args{"--listen", "123536"}, "out of range"),
        std::make_pair(Args{"--listen", "8080x"}, "must be an integer"),
        std::make_pair(Args{"--listen", "99999999999999999999"}, "out of range"),
        // A negative timeout silently turned the upstream idle sweep off.
        std::make_pair(Args{"--upstream-timeout", "-5"}, "out of range"),
        std::make_pair(Args{"--connect-timeout", "5s"}, "not a valid number"),
        std::make_pair(Args{"--pool-idle", "nan"}, "out of range"),
        // Uncapped on the command line, capped at 4096 in the file.
        std::make_pair(Args{"--workers", "100000"}, "out of range"),
        std::make_pair(Args{"--workers", "0"}, "out of range"),
        // A typo became auto.
        std::make_pair(Args{"--io", "urnig"}, "must be one of: auto epoll uring"),
        std::make_pair(Args{"--log-level", "verbose"}, "must be one of"),
        // A flag missing its value was ignored, or swallowed the next flag.
        std::make_pair(Args{"--io", "urnig", "--upstream-timeout", "-5", "--workers"},
                       "must be one of"),
        std::make_pair(Args{"--workers"}, "--workers needs a value"),
        std::make_pair(Args{"--upstream", "--workers", "2"}, "--upstream needs a value"),
        std::make_pair(Args{"--duration"}, "--duration needs a value"),
        // Retired spellings are named, not aliased.
        std::make_pair(Args{"--translate", "anthropic"}, "is now --upstream-dialect"),
        std::make_pair(Args{"--upstream-dialect", "none"}, "never an absence"),
        std::make_pair(Args{"--upstream-dialect", "claude"}, "must be one of"),
        std::make_pair(Args{"--listen=8080"}, "flags take a separate value"),
        std::make_pair(Args{"--lisen", "8080"}, "unknown argument"),
        std::make_pair(Args{"--config"}, "--config needs a path"),
        std::make_pair(Args{"--config", "/nonexistent/a", "--config", "/nonexistent/b"},
                       "given twice"),
        std::make_pair(Args{"--config", "/nonexistent/a"}, "cannot read")));

// Every flag bench/*.sh and bench/enterpilot pass, spelled as they spell them.
TEST(Cli, TheBenchFlagsStillParse)
{
    Settings s;
    std::string err;
    ASSERT_EQ(parse({"--listen", "8090", "--upstream", "127.0.0.1:9101", "--upstream-dialect",
                     "anthropic", "--io", "uring", "--workers", "2", "--duration", "23",
                     "--warmup", "5", "--log-level", "error", "--upstream-timeout", "30"},
                    s, err),
              Cli::Run)
        << err;
    EXPECT_EQ(s.listen_port, 8090);
    EXPECT_EQ(s.upstream, "127.0.0.1:9101");
    EXPECT_EQ(s.dialect, "anthropic");
    EXPECT_EQ(s.io, "uring");
    EXPECT_EQ(s.workers, 2);
    EXPECT_DOUBLE_EQ(s.duration_s, 23);
    EXPECT_DOUBLE_EQ(s.warmup_s, 5);
    EXPECT_EQ(s.log_level, "error");
    EXPECT_DOUBLE_EQ(s.upstream_s, 30);
}

TEST(Cli, ValuesAndSwitchesLand)
{
    Settings s;
    std::string err;
    ASSERT_EQ(parse({"--duration", "0.5", "--listen", "0", "--timing-headers", "--listen-tls",
                     "--tls-cert", "/c", "--tls-key", "/k", "--prefault-mb", "16",
                     "--client-idle", "60", "--pool-idle", "45", "--connect-timeout", "2"},
                    s, err),
              Cli::Run)
        << err;
    EXPECT_DOUBLE_EQ(s.duration_s, 0.5); // atoi made this 0, which runs forever
    EXPECT_EQ(s.listen_port, 0);
    EXPECT_TRUE(s.timing_headers);
    EXPECT_TRUE(s.listen_tls);
    EXPECT_EQ(s.tls_cert, "/c");
    EXPECT_EQ(s.tls_key, "/k");
    EXPECT_DOUBLE_EQ(s.prefault_mb, 16);
    EXPECT_DOUBLE_EQ(s.client_idle_s, 60);
    EXPECT_DOUBLE_EQ(s.pool_idle_s, 45);
    EXPECT_DOUBLE_EQ(s.connect_s, 2);
}

// The file is applied first wherever --config sits, so a flag always wins.
TEST(Cli, AFlagBeatsTheFileWhereverItSits)
{
    const std::string path = ::testing::TempDir() + "llmbridge_cli_test.json";
    {
        std::ofstream f(path);
        f << R"({"listen":{"port":1111},"runtime":{"workers":3}})";
    }
    for (bool flag_first : {true, false})
    {
        Settings s;
        std::string err;
        const Cli r = flag_first ? parse({"--listen", "2222", "--config", path.c_str()}, s, err)
                                 : parse({"--config", path.c_str(), "--listen", "2222"}, s, err);
        ASSERT_EQ(r, Cli::Run) << err;
        EXPECT_EQ(s.listen_port, 2222);
        EXPECT_EQ(s.workers, 3);
    }
    std::remove(path.c_str());
}

TEST(Cli, HelpWinsAndIsGolden)
{
    Settings s;
    std::string err;
    EXPECT_EQ(parse({"--lisen", "--help"}, s, err), Cli::Help);
    EXPECT_EQ(parse({"-h"}, s, err), Cli::Help);
    // Pinned on purpose: a change here is a change to the interface operators read.
    EXPECT_EQ(llmbridge::app::help_text("llmbridge"), kGoldenHelp);
}
