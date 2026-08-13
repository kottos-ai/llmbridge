// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The config parser exists to REFUSE things, so most of this file is rejection
// cases. The one that matters most is the unknown key: a parser that ignores a
// misspelled setting fails open, silently, with no `ps` output to catch it, which
// is the exact shape of the `--listen-tls`-on-a-non-TLS-build defect.

#include "config.hpp"

#include "gateway/gateway.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <utility>

using llmbridge::app::ConfigFile;
using llmbridge::app::parse_config;

TEST(Config, FullFileAppliesEveryGroup)
{
    const std::string text = R"({
      "_note": "comments are keys beginning with underscore",
      "listen":   { "port": 8443, "tls": true, "cert": "/c.pem", "key": "/k.pem" },
      "upstream": { "url": "https://api.anthropic.com", "translate": "anthropic" },
      "timeouts": { "upstream_s": 90, "client_idle_s": 259200, "pool_idle_s": 45 },
      "runtime":  { "io": "uring", "workers": 3, "timing_headers": true,
                    "duration_s": 12, "warmup_s": 2 }
    })";
    ConfigFile c;
    std::string err;
    ASSERT_TRUE(parse_config(text, c, err)) << err;

    EXPECT_TRUE(c.has_listen_port);
    EXPECT_EQ(c.listen_port, 8443);
    EXPECT_TRUE(c.has_listen_tls);
    EXPECT_TRUE(c.listen_tls);
    EXPECT_EQ(c.tls_cert, "/c.pem");
    EXPECT_EQ(c.tls_key, "/k.pem");
    EXPECT_EQ(c.upstream_url, "https://api.anthropic.com");
    EXPECT_EQ(c.translate_mode, "anthropic");
    EXPECT_TRUE(c.has_upstream_s);
    EXPECT_DOUBLE_EQ(c.upstream_s, 90);
    EXPECT_DOUBLE_EQ(c.client_idle_s, 259200);
    EXPECT_DOUBLE_EQ(c.pool_idle_s, 45);
    EXPECT_EQ(c.io, "uring");
    EXPECT_EQ(c.workers, 3);
    EXPECT_TRUE(c.timing_headers);
    EXPECT_DOUBLE_EQ(c.duration_s, 12);
    EXPECT_DOUBLE_EQ(c.warmup_s, 2);
}

// The values must survive the DOM they were parsed from. provider::json is
// zero-copy, so every string in the parsed Value is a view into the input buffer;
// if a ConfigFile field ever held a string_view instead of a string, this test
// reads freed memory and ASan says so.
TEST(Config, ValuesOutliveTheParsedBuffer)
{
    ConfigFile c;
    std::string err;
    {
        std::string scratch = R"({"upstream":{"url":"https://example.invalid/v1"}})";
        ASSERT_TRUE(parse_config(scratch, c, err)) << err;
        scratch.assign(4096, 'x'); // clobber the buffer the DOM pointed into
    }
    EXPECT_EQ(c.upstream_url, "https://example.invalid/v1");
}

// An absent key must leave the caller's default alone, which is what makes
// "file first, flags second" work at all.
TEST(Config, AbsentKeysAreNotApplied)
{
    ConfigFile c;
    std::string err;
    ASSERT_TRUE(parse_config(R"({"listen":{"port":9000}})", c, err)) << err;
    EXPECT_TRUE(c.has_listen_port);
    EXPECT_FALSE(c.has_listen_tls);
    EXPECT_FALSE(c.has_workers);
    EXPECT_TRUE(c.tls_cert.empty());
    EXPECT_TRUE(c.upstream_url.empty());
}

class ConfigReject : public ::testing::TestWithParam<std::pair<const char*, const char*>> {};

TEST_P(ConfigReject, IsRefusedAndNamesTheProblem)
{
    ConfigFile c;
    std::string err;
    EXPECT_FALSE(parse_config(GetParam().first, c, err)) << "accepted: " << GetParam().first;
    EXPECT_NE(err.find(GetParam().second), std::string::npos)
        << "the error did not name the problem. got: " << err;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, ConfigReject,
    ::testing::Values(
        // THE ONE THAT MATTERS: a misspelling must not be silently ignored.
        std::make_pair(R"({"listen":{"listen_tls":true}})", "listen_tls"),
        std::make_pair(R"({"lisen":{"port":1}})", "lisen"),
        std::make_pair(R"({"runtime":{"worker":2}})", "worker"),
        // wrong types
        std::make_pair(R"({"listen":{"tls":"yes"}})", "true or false"),
        std::make_pair(R"({"listen":{"port":"8443"}})", "must be a number"),
        std::make_pair(R"({"upstream":{"url":443}})", "must be a string"),
        std::make_pair(R"({"listen":"8443"})", "must be an object"),
        // bad enum values, and the message lists what is allowed
        std::make_pair(R"({"upstream":{"translate":"claude"}})", "must be one of"),
        std::make_pair(R"({"runtime":{"io":"kqueue"}})", "must be one of"),
        // out of range: a typo meaning milliseconds must not disable a timeout
        std::make_pair(R"({"listen":{"port":70000}})", "out of range"),
        std::make_pair(R"({"timeouts":{"pool_idle_s":99999999}})", "out of range"),
        std::make_pair(R"({"runtime":{"workers":0}})", "out of range"),
        // structure
        std::make_pair("[]", "top level must be an object"),
        std::make_pair("{", "not valid JSON"),
        // paths must not carry escapes the zero-copy DOM would hand over undecoded
        std::make_pair(R"({"listen":{"cert":"/a\\b.pem"}})", "backslash")));

// Comments are what let unknown keys be strict at all: without them an operator
// would have no way to annotate the file.
TEST(Config, UnderscoreKeysAreCommentsEverywhere)
{
    ConfigFile c;
    std::string err;
    const std::string text = R"({
      "_why": "top level",
      "listen": { "_why": "nested too", "port": 1234 }
    })";
    ASSERT_TRUE(parse_config(text, c, err)) << err;
    EXPECT_EQ(c.listen_port, 1234);
}

// The shipped example claims every value in it is the built-in default. That claim
// rots the moment a default changes, and a reference file that lies is worse than
// none, so it is pinned here against the constants themselves. If this fails, either
// the example or the default moved and the other has to follow.
TEST(Config, ShippedExampleMatchesTheRealDefaults)
{
    std::ifstream in(LLMBRIDGE_EXAMPLE_CONFIG, std::ios::binary);
    ASSERT_TRUE(in) << "cannot read " << LLMBRIDGE_EXAMPLE_CONFIG;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    ConfigFile c;
    std::string err;
    ASSERT_TRUE(parse_config(text, c, err)) << "the shipped example does not parse: " << err;

    // Mirrors the initialisers at the top of app/main.cpp.
    EXPECT_EQ(c.listen_port, 8088);
    EXPECT_FALSE(c.listen_tls);
    EXPECT_TRUE(c.tls_cert.empty()) << "a certificate with tls:false is refused at startup";
    EXPECT_TRUE(c.tls_key.empty());
    EXPECT_EQ(c.upstream_url, "127.0.0.1:9001");
    EXPECT_EQ(c.translate_mode, "none");
    EXPECT_DOUBLE_EQ(c.upstream_s,
                     static_cast<double>(llmbridge::Gateway::kDefaultUpstreamIdleNs) / 1e9);
    EXPECT_DOUBLE_EQ(c.client_idle_s,
                     static_cast<double>(llmbridge::Gateway::kDefaultClientIdleNs) / 1e9);
    EXPECT_DOUBLE_EQ(c.pool_idle_s,
                     static_cast<double>(llmbridge::Gateway::kDefaultPoolIdleNs) / 1e9);
    EXPECT_EQ(c.io, "auto");
    EXPECT_EQ(c.workers, 1);
    EXPECT_FALSE(c.timing_headers);
    EXPECT_DOUBLE_EQ(c.duration_s, 0);
    EXPECT_DOUBLE_EQ(c.warmup_s, 0);

    // Every settable key must APPEAR, or the example silently stops documenting one.
    EXPECT_TRUE(c.has_listen_port && c.has_listen_tls && c.has_upstream_s &&
                c.has_client_idle_s && c.has_pool_idle_s && c.has_workers &&
                c.has_timing_headers && c.has_duration_s && c.has_warmup_s)
        << "the example is missing a key it is supposed to document";
}
