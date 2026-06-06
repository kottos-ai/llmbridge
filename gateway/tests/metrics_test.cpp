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
#include <string>

using llmbridge::Histogram;
using llmbridge::now_ns;

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
