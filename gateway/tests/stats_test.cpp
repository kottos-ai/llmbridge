// Tests for kottos::Stats (gateway/gateway.hpp) — the per-request accounting
// struct: zeroed counters and three independent sub-ms-resolution histograms.
// (Histogram internals are covered in metrics_test.cpp.)

#include "gateway/gateway.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using kottos::Stats;

TEST(StatsInit, CountersZeroed)
{
    Stats s;
    EXPECT_EQ(s.requests, 0u);
    EXPECT_EQ(s.errors, 0u);
    EXPECT_EQ(s.upstream_conns_opened, 0u);
}

TEST(StatsInit, AllThreeHistogramsEmptyAndSubMsConfig)
{
    Stats s;
    for (const auto* h : {&s.overhead, &s.req_path, &s.resp_path})
    {
        EXPECT_EQ(h->total(), 0u);
        EXPECT_EQ(h->bucket_ns(), 20u);
        EXPECT_EQ(h->n_buckets(), 131072u);
    }
}

TEST(StatsRecord, HistogramsAreIndependent)
{
    Stats s;
    s.overhead.record(1000);
    EXPECT_EQ(s.overhead.total(), 1u);
    EXPECT_EQ(s.req_path.total(), 0u);
    EXPECT_EQ(s.resp_path.total(), 0u);
    s.req_path.record(2000);
    s.req_path.record(3000);
    EXPECT_EQ(s.req_path.total(), 2u);
    EXPECT_EQ(s.resp_path.total(), 0u);
}

TEST(StatsCounters, AreIndependentlyIncrementable)
{
    Stats s;
    s.requests += 5;
    s.errors += 2;
    s.upstream_conns_opened += 9;
    EXPECT_EQ(s.requests, 5u);
    EXPECT_EQ(s.errors, 2u);
    EXPECT_EQ(s.upstream_conns_opened, 9u);
}

TEST(StatsRecord, OverheadPercentileReflectsRecorded)
{
    Stats s;
    s.overhead.record(12'000); // 12 µs
    s.overhead.record(32'000); // 32 µs
    EXPECT_EQ(s.overhead.total(), 2u);
    EXPECT_GE(s.overhead.percentile(0.50), 12'000u);
    EXPECT_LT(s.overhead.percentile(0.99), 1'000'000u);
}
