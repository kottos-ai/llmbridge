// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Tests for the vendored metrics (gateway/metrics.hpp): the linear-bucket
// Histogram and the monotonic now_ns() clock.

#include "gateway/metrics.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>

using llmbridge::Histogram;
using llmbridge::now_ns;
using llmbridge::timing_split;
using llmbridge::TimingSplit;

namespace
{
    constexpr uint64_t kBucket = 20;
    constexpr size_t kBuckets = 131072;
    constexpr uint64_t kRange = kBucket * kBuckets; // 2,621,440 ns ≈ 2.62 ms
} // namespace

TEST(Histogram, DefaultConfigIsSubMsResolution)
{
    Histogram h;
    EXPECT_EQ(h.bucket_ns(), kBucket);
    EXPECT_EQ(h.n_buckets(), kBuckets);
    EXPECT_EQ(h.max_tracked_ns(), kRange);
    EXPECT_EQ(h.total(), 0u);
}

TEST(Histogram, RecordCountsAndMean)
{
    Histogram h;
    for (int i = 0; i < 100; ++i) h.record(10'000);
    EXPECT_EQ(h.total(), 100u);
    EXPECT_EQ(h.mean_ns(), 10'000u);
}

// Single in-range value: percentile lands in (v, v+bucket].
class HistSingle : public ::testing::TestWithParam<uint64_t> {};
TEST_P(HistSingle, PercentileResolution)
{
    const uint64_t v = GetParam();
    Histogram h;
    h.record(v);
    EXPECT_EQ(h.total(), 1u);
    const uint64_t p = h.percentile(0.50);
    EXPECT_GT(p, v);
    EXPECT_LE(p, v + kBucket);
    EXPECT_EQ(h.overflow_count(), 0u);
}
INSTANTIATE_TEST_SUITE_P(InRange, HistSingle,
                         ::testing::Values(0u, 1u, 5u, 10u, 19u, 20u, 21u, 100u, 999u,
                                           12'000u, 32'000u, 100'000u, 500'000u,
                                           1'000'000u, 2'000'000u, 2'600'000u),
                         [](const testing::TestParamInfo<uint64_t>& i) {
                             return std::string("ns_") + std::to_string(i.param);
                         });

TEST(Histogram, OverflowCountsAndPreservesMax)
{
    Histogram h;
    const uint64_t big = kRange + 500'000;
    h.record(big);
    EXPECT_EQ(h.overflow_count(), 1u);
    EXPECT_EQ(h.max(), big);
    EXPECT_EQ(h.percentile(0.99), big); // overflow -> running max
}

TEST(Histogram, PercentilesAreMonotonic)
{
    Histogram h;
    for (uint64_t v = 0; v < 100'000; v += 100) h.record(v);
    EXPECT_LE(h.percentile(0.50), h.percentile(0.90));
    EXPECT_LE(h.percentile(0.90), h.percentile(0.99));
    EXPECT_LE(h.percentile(0.99), h.percentile(1.0));
}

TEST(Histogram, ClearResets)
{
    Histogram h;
    h.record(1234);
    h.record(5678);
    ASSERT_EQ(h.total(), 2u);
    h.clear();
    EXPECT_EQ(h.total(), 0u);
    EXPECT_EQ(h.max(), 0u);
}

TEST(Histogram, EmptyPercentileIsZero)
{
    Histogram h;
    EXPECT_EQ(h.percentile(0.5), 0u);
}

TEST(Histogram, CustomBucketConfig)
{
    Histogram h(1000, 1000); // 1 µs buckets, 0..1 ms
    EXPECT_EQ(h.bucket_ns(), 1000u);
    EXPECT_EQ(h.n_buckets(), 1000u);
    EXPECT_EQ(h.max_tracked_ns(), 1'000'000u);
}

TEST(NowNs, IsMonotonicNonDecreasing)
{
    int64_t a = now_ns();
    int64_t b = now_ns();
    int64_t c = now_ns();
    EXPECT_GE(b, a);
    EXPECT_GE(c, b);
    EXPECT_GT(c, 0);
}

// An empty histogram must announce that it has no data instead of print zeros.
// This is not cosmetic: bench/run_bench.sh seds the `added-total` line for
// `p99=<n> us`, so an all-zero print made a zero-sample run publishable as a
// FABRICATED 0 us added latency. Streaming workloads produce exactly this state
// every run -- streams count in `requests` but are never recorded here
// (LATENCY.md section 4). Delete the `_total == 0` guard in Histogram::print and
// both expectations below fail.
TEST(Histogram, EmptyPrintsNoSamplesNotZeros)
{
    Histogram h;
    std::ostringstream os;
    h.print(os, "added-total");
    const std::string out = os.str();

    EXPECT_NE(out.find("count=0"), std::string::npos) << out;
    EXPECT_NE(out.find("(no samples)"), std::string::npos) << out;
    // The bench harness must find NO percentile to scrape.
    EXPECT_EQ(out.find("p99="), std::string::npos) << out;
    EXPECT_EQ(out.find("p50="), std::string::npos) << out;
}

// The non-empty path is unaffected: one sample still reports percentiles.
TEST(Histogram, NonEmptyStillPrintsPercentiles)
{
    Histogram h;
    h.record(5'000);
    std::ostringstream os;
    h.print(os, "added-total");
    const std::string out = os.str();

    EXPECT_NE(out.find("count=1"), std::string::npos) << out;
    EXPECT_NE(out.find("p99="), std::string::npos) << out;
    EXPECT_EQ(out.find("(no samples)"), std::string::npos) << out;
}

// ---- timing_split: the single definition shared by headers and histograms ----
//
// The bug these lock down: `connect-us` (header) used to span t1->t3 -- handshake
// PLUS the upstream write -- while the `connect(TLS)` histogram spanned t1->t2,
// handshake only. One name, two meanings, on the same request. Both surfaces now
// derive from timing_split(), so the drift is unrepresentable; these assert the
// arithmetic that makes that safe.

TEST(TimingSplit, PooledConnectionHasExactlyZeroConnect)
{
    // Pooled reuse stamps t2 == t1: no handshake happened.
    const TimingSplit s = timing_split(/*t0*/ 0, /*t1*/ 100, /*t2*/ 100, /*t3*/ 140,
                                       /*t4*/ 9000, /*t5*/ 9100);
    EXPECT_EQ(s.connect_ns, 0) << "a pooled connection must report exactly 0 connect";
    EXPECT_EQ(s.upwrite_ns, 40) << "the write is its own interval, not part of connect";
    EXPECT_EQ(s.req_path_ns, 140);          // (t1-t0)=100 compute + 40 write
    EXPECT_EQ(s.compute_ns, 200);           // (t1-t0)=100 + (t5-t4)=100
    EXPECT_EQ(s.upstream_ns, 8860);
}

TEST(TimingSplit, UnstampedT2FallsBackToT1)
{
    // t2 == 0 means no connect ever ran; wire-ready IS t1.
    const TimingSplit s = timing_split(0, 100, 0, 140, 9000, 9100);
    EXPECT_EQ(s.connect_ns, 0);
    EXPECT_EQ(s.upwrite_ns, 40) << "must not attribute the write to a phantom handshake";
}

TEST(TimingSplit, ColdConnectionSeparatesHandshakeFromWrite)
{
    // Cold: 50 ms handshake between t1 and t2, then a 40 ns write.
    const TimingSplit s = timing_split(0, 100, 50'000'100, 50'000'140, 60'000'000,
                                       60'000'100);
    EXPECT_EQ(s.connect_ns, 50'000'000) << "the handshake must land in connect, alone";
    EXPECT_EQ(s.upwrite_ns, 40);
    // The handshake must NOT inflate req_path -- that is the added-latency claim.
    EXPECT_EQ(s.req_path_ns, 140);
}

TEST(TimingSplit, ConnectPlusWriteReproducesTheOldHeaderSpan)
{
    // Compatibility bridge: the retired `connect-us` was t3-t1. Anyone with the old
    // semantics recovers it by adding the two headers that replaced it.
    const int64_t t1 = 100, t2 = 50'000'100, t3 = 50'000'140;
    const TimingSplit s = timing_split(0, t1, t2, t3, 60'000'000, 60'000'100);
    EXPECT_EQ(s.connect_ns + s.upwrite_ns, t3 - t1);
}

TEST(TimingSplit, ReqPathIsComputeLegPlusWriteNotTheWholeCompute)
{
    // req_path (histogram) and compute (header) are DIFFERENT groupings of the same
    // stamps: req_path carries the write, compute carries the response leg.
    const TimingSplit s = timing_split(0, 100, 100, 140, 9000, 9100);
    EXPECT_NE(s.req_path_ns, s.compute_ns);
    EXPECT_EQ(s.req_path_ns - s.upwrite_ns, s.compute_ns - (9100 - 9000));
}

TEST(TimingSplit, StreamingPassesT5EqualT4SoComputeIsRequestLegOnly)
{
    const TimingSplit s = timing_split(0, 100, 100, 140, 9000, /*t5 == t4*/ 9000);
    EXPECT_EQ(s.compute_ns, 100) << "a stream must report the request leg, not an invention";
}
