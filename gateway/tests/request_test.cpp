// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// auth_headers_for's one-pass scan replaced six find_header() walks per request
// (request.cpp's scan_auth_headers), measured at ~40 us p99 on a real request.
// Raised on a public thread as the scary class of optimization: a later cleanup
// could silently reorder the walk, or drop the pool, without anything failing.
// The correctness half of the guard: order and duplicates must not change the
// result, and one golden fixture pins what the result is. The percentile harness
// lives separately, in request_perf_test.cpp, deliberately not run from here: a
// tight timing tolerance inside the PR-blocking suite is exactly the "noisy job
// blocks every PR" shape a reviewer warned against. What does belong here is the
// loose sanity bound at the end, an order of magnitude over measured, which only a
// pathological regression can trip.

#include "request.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace
{
    using llmbridge::UpstreamDialect;
    using llmbridge::detail::auth_headers_for;
} // namespace

TEST(AuthHeaders, OrderDoesNotChangeTheResult)
{
    const std::vector<std::string> strip;
    const std::vector<std::string> lines = {
        "Authorization: Bearer sk-live-abc\r\n",
        "anthropic-version: 2024-01-01\r\n",
        "Host: client\r\n",
    };
    std::vector<size_t> idx = {0, 1, 2};
    std::string baseline;
    bool have_baseline = false;
    do
    {
        std::string headers;
        for (const size_t i : idx) headers += lines[i];
        std::string out;
        ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
        if (!have_baseline) { baseline = out; have_baseline = true; }
        else EXPECT_EQ(out, baseline) << "header order must not change the resolved credential";
    } while (std::next_permutation(idx.begin(), idx.end()));
}

TEST(AuthHeaders, FirstDuplicateWins)
{
    const std::vector<std::string> strip;
    const std::string headers =
        "x-api-key: first\r\n"
        "x-api-key: second\r\n"
        "anthropic-version: 2024-01-01\r\n";
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
    EXPECT_NE(out.find("x-api-key: first"), std::string::npos);
    EXPECT_EQ(out.find("second"), std::string::npos) << "the second x-api-key must never surface";
}

TEST(AuthHeaders, DuplicateAcrossCaseStillResolvesFirst)
{
    // Header names are matched case-insensitively; a duplicate spelled differently
    // is still the same header to the merged walk, not a second candidate.
    const std::vector<std::string> strip;
    const std::string headers =
        "X-API-KEY: first\r\n"
        "x-api-key: second\r\n"
        "anthropic-version: 2024-01-01\r\n";
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
    EXPECT_NE(out.find("x-api-key: first"), std::string::npos);
    EXPECT_EQ(out.find("second"), std::string::npos);
}

TEST(AuthHeaders, StrippedHeaderIsInvisible)
{
    const std::vector<std::string> strip = {"authorization:"};
    const std::string headers =
        "Authorization: Bearer tenant-token\r\n"
        "anthropic-version: 2024-01-01\r\n";
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
    EXPECT_EQ(out.find("x-api-key"), std::string::npos) << "a stripped Authorization yields no credential";
}

TEST(AuthHeaders, ControlByteInValueIsRefused)
{
    const std::vector<std::string> strip;
    const std::string headers = "x-api-key: sk\rX-Smuggled: yes\r\n";
    std::string out;
    EXPECT_FALSE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
}

// The golden fixture. The permutation test above proves every ordering agrees
// with every other; it never says what they agree on, so a cleanup that changed
// the emitted format for all of them at once (a lowercased name, a dropped CRLF,
// the two lines swapped) would pass it. This pins the exact bytes for the one
// request that exercises everything at once: a duplicated x-api-key in mixed
// case, a duplicated anthropic-version, a bearer that must lose to the api key,
// and unrelated headers interleaved throughout. Any semantic change to the walk
// fails this, and the failure names the byte that moved.
TEST(AuthHeaders, GoldenSerializedResult)
{
    const std::vector<std::string> strip;
    const std::string headers =
        "Host: client\r\n"
        "x-api-key: first-key\r\n"
        "Content-Type: application/json\r\n"
        "X-API-KEY: second-key\r\n"
        "anthropic-version: 2024-01-01\r\n"
        "Authorization: Bearer sk-bearer\r\n"
        "anthropic-version: 2025-01-01\r\n";
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
    EXPECT_EQ(out, "x-api-key: first-key\r\nanthropic-version: 2024-01-01\r\n");
}

// A sanity bound, not a benchmark. Measured on the reference host at ~135 ns
// p50 (request_perf_test.cpp); this passes anything under 2,000 ns, an order of
// magnitude of headroom, and averages over 200,000 calls so a context switch or
// two lands as noise instead of a spike. What it catches is the pathological
// shape: the six-walk version this scan replaced measured ~40 us at p99 on a
// live request, twenty times past this line. A tight tolerance would flake on a
// shared runner; this one cannot without something being genuinely wrong.
// Skipped under the sanitizers, which slow every path uniformly.
#if defined(__has_feature)
    #define LLMBRIDGE_TEST_UNDER_CLANG_SANITIZER \
        (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
#else
    #define LLMBRIDGE_TEST_UNDER_CLANG_SANITIZER 0
#endif
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !LLMBRIDGE_TEST_UNDER_CLANG_SANITIZER
TEST(AuthHeaders, StaysWithinAnOrderOfMagnitudeOfOneWalk)
{
    const std::vector<std::string> strip;
    const std::string headers =
        "Host: client\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer sk-live-0123456789abcdef\r\n"
        "anthropic-version: 2023-06-01\r\n"
        "X-Request-Id: abc-123\r\n"
        "Accept: application/json\r\n";
    std::string out;
    constexpr int kIters = 200'000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i)
        ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
    const auto t1 = std::chrono::steady_clock::now();
    const double ns_per_call = std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
    EXPECT_LT(ns_per_call, 2000.0) << "ns/call=" << ns_per_call;
}
#endif

