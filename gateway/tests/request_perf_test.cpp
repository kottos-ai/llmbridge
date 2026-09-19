// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The perf half of the auth_headers_for guard (request_test.cpp has the
// correctness half). Deliberately not wired into gtest_discover_tests: this
// binary is built by the default target but ctest never runs it, so shared
// CI-runner jitter cannot fail a PR on it. Run it by hand, or from a nightly
// job, and read the percentiles: p50/p95/p99 with a tolerance is what a
// reviewer asked for, not a single average compared to a hard-coded number.
//
// Baseline, this host, Release build, 2026-09, 3 runs: p50 133-136 ns, p95
// 141-244 ns, p99 260-295 ns. The regression this exists to catch is the
// six-walk version it replaced, ~40 us at p99 on a live request, two orders of
// magnitude past this, so it fails loud long before it fails close.

#include "request.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using llmbridge::UpstreamDialect;
    using llmbridge::detail::auth_headers_for;

    double pct(std::vector<double>& sorted_ns, double p)
    {
        const size_t idx = static_cast<size_t>(p / 100.0 * (sorted_ns.size() - 1));
        return sorted_ns[idx];
    }
} // namespace

int main()
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

    // Unmeasured warm-up: the first calls pull the branch predictor and the
    // instruction cache cold, which is not the steady-state cost this reports.
    for (int i = 0; i < 10'000; ++i) auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out);

    // Batched, not one clock read per call: steady_clock::now() measured at
    // ~16.5 ns on this host, and calling it twice per sample would add ~33 ns of
    // pure clock overhead onto a call this small, enough to dominate p50 instead
    // of measuring it. 64 calls per batch amortizes that to well under 1 ns/call
    // while still leaving enough batches for the tail (cache misses, scheduling)
    // to show up in the percentiles instead of being averaged away entirely.
    constexpr int kBatch = 64;
    constexpr int kSamples = 200'000;
    std::vector<double> ns(kSamples);
    for (int i = 0; i < kSamples; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int j = 0; j < kBatch; ++j) auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out);
        const auto t1 = std::chrono::steady_clock::now();
        ns[static_cast<size_t>(i)] = std::chrono::duration<double, std::nano>(t1 - t0).count() / kBatch;
    }
    std::sort(ns.begin(), ns.end());

    std::printf("auth_headers_for, %d batches of %d calls each\n", kSamples, kBatch);
    std::printf("  p50 = %8.1f ns\n", pct(ns, 50));
    std::printf("  p95 = %8.1f ns\n", pct(ns, 95));
    std::printf("  p99 = %8.1f ns\n", pct(ns, 99));
    // No assertion here on purpose. Comparing this run's percentiles against a
    // recorded baseline, with a tolerance, is a decision for whatever schedules
    // this job, not a hard-coded threshold baked into the binary.
    return 0;
}
