// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Self-contained metrics for llmbridge — a tiny linear-bucket latency histogram and
// a monotonic clock. Zero dependencies — llmbridge stands alone as a
// self-contained, open-source project.
//
// The histogram is single-threaded by design: give each thread its own and
// merge if needed. record() is one branch + one increment; ~5 ns.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <vector>

namespace llmbridge
{
    // Monotonic wall-clock in nanoseconds. steady_clock is the right source for
    // measuring intervals (never steps backward, unaffected by NTP).
    int64_t now_ns() noexcept;

    // Epoch nanoseconds for a monotonic now_ns() stamp — a timestamp that is both
    // orderable and meaningful as wall time.
    //
    // Why not just read CLOCK_REALTIME: it can STEP, forwards or backwards, when NTP
    // disciplines it. Two requests would then be orderable by arrival but not by
    // timestamp, which is exactly the property an order book cannot lose. So the
    // realtime clock is read ONCE at startup and every later timestamp is that anchor
    // plus a monotonic delta: epoch-meaningful, strictly increasing within the
    // process, immune to NTP steps.
    //
    // The trade: without NTP correction this drifts from true wall time over a long
    // run (ppm-scale). Ordering and intra-process deltas are unaffected; joining
    // timestamps ACROSS hosts to sub-millisecond accuracy needs PTP or a periodic
    // re-anchor, and is not something a single gateway can promise on its own.
    int64_t wall_ns(int64_t mono_ns) noexcept;

    // THE ONE DEFINITION of how a request's stamps become reported intervals.
    //
    // Both reporting surfaces derive from this: the per-request `x-llmbridge-*`
    // headers and the shutdown histograms. They previously computed their own
    // groupings independently, and drifted — `connect-us` spanned t1->t3 (handshake
    // PLUS the upstream write) while the `connect(TLS)` histogram spanned t1->t2
    // (handshake only). Same name, two meanings, and a code comment that copied the
    // histogram's "exactly 0 when pooled" onto the header's number, which is never 0.
    // Sharing this function makes that class of drift unrepresentable.
    //
    // LATENCY.md is the normative prose; this is its executable form. Change one and
    // change the other.
    struct TimingSplit
    {
        int64_t compute_ns;  // (t1-t0) + (t5-t4)  our compute, both legs
        int64_t connect_ns;  // (t2-t1)            handshake only; 0 on a pooled conn
        int64_t upwrite_ns;  // (t3-t2)            the write() into the socket buffer
        int64_t upstream_ns; // (t4-t3)            the provider
        int64_t req_path_ns; // (t1-t0) + (t3-t2)  histogram grouping: compute + write
    };

    // t2 == 0 means "no connect ever happened" (pooled reuse never stamped it), in
    // which case wire-ready IS t1 and connect_ns is exactly 0.
    //
    // Streaming callers have no t5 (the response is not built at one instant): pass
    // t5 = t4 and compute_ns collapses to the request leg alone, which is the honest
    // answer rather than an invented one.
    TimingSplit timing_split(int64_t t0, int64_t t1, int64_t t2, int64_t t3, int64_t t4,
                             int64_t t5) noexcept;

    class Histogram
    {
    public:
        // Default: 20 ns buckets over 0..2.62 ms — sized to resolve a sub-ms
        // proxy-overhead claim without overflowing on a cold-connect outlier.
        Histogram() : Histogram(20, 131072) {}

        Histogram(uint64_t bucket_ns, size_t n_buckets)
            : _bucket_ns(bucket_ns), _counts(n_buckets, 0) {}

        void record(uint64_t ns) noexcept
        {
            const uint64_t idx = ns / _bucket_ns;
            if (idx < _counts.size()) ++_counts[idx];
            else ++_overflow;
            if (ns > _max) _max = ns;
            ++_total;
            _total_ns += ns;
        }

        void clear() noexcept
        {
            for (auto& c : _counts) c = 0;
            _total = _overflow = _total_ns = _max = 0;
        }

        // Fold another histogram (same bucket size) into this one — for aggregating
        // per-worker stats across a multi-worker (SO_REUSEPORT) run.
        void merge(const Histogram& o) noexcept
        {
            if (o._counts.size() > _counts.size()) _counts.resize(o._counts.size(), 0);
            for (size_t i = 0; i < o._counts.size(); ++i) _counts[i] += o._counts[i];
            _total += o._total;
            _total_ns += o._total_ns;
            _overflow += o._overflow;
            if (o._max > _max) _max = o._max;
        }

        [[nodiscard]] uint64_t total() const noexcept { return _total; }
        [[nodiscard]] uint64_t max() const noexcept { return _max; }
        [[nodiscard]] uint64_t mean_ns() const noexcept { return _total ? _total_ns / _total : 0; }
        [[nodiscard]] uint64_t overflow_count() const noexcept { return _overflow; }
        [[nodiscard]] uint64_t bucket_ns() const noexcept { return _bucket_ns; }
        [[nodiscard]] size_t n_buckets() const noexcept { return _counts.size(); }
        [[nodiscard]] uint64_t max_tracked_ns() const noexcept { return _bucket_ns * _counts.size(); }

        // Value at percentile p (0..1). Resolution = bucket_ns. Returns the
        // running max when the target falls in the overflow region.
        [[nodiscard]] uint64_t percentile(double p) const noexcept
        {
            if (_total == 0) return 0;
            if (p < 0.0) p = 0.0;
            if (p > 1.0) p = 1.0;
            uint64_t target = static_cast<uint64_t>(_total * p);
            if (target == 0) target = 1;
            uint64_t cum = 0;
            for (size_t i = 0; i < _counts.size(); ++i)
            {
                cum += _counts[i];
                if (cum >= target) return (i + 1) * _bucket_ns;
            }
            return _max;
        }

        // Pretty-print a percentile summary line; `label` prefixes it.
        void print(std::ostream& os, const char* label = "latency") const;

    private:
        uint64_t _bucket_ns;
        std::vector<uint64_t> _counts;
        uint64_t _total = 0;
        uint64_t _total_ns = 0; // exact running sum, no bucket-rounding loss
        uint64_t _overflow = 0;
        uint64_t _max = 0;
    };
} // namespace llmbridge
