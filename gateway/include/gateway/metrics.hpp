#pragma once

// Self-contained metrics for Kottos — a tiny linear-bucket latency histogram and
// a monotonic clock. Zero dependencies — Kottos stands alone as a
// self-contained, open-source project.
//
// The histogram is single-threaded by design: give each thread its own and
// merge if needed. record() is one branch + one increment; ~5 ns.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <vector>

namespace kottos
{
    // Monotonic wall-clock in nanoseconds. steady_clock is the right source for
    // measuring intervals (never steps backward, unaffected by NTP).
    int64_t now_ns() noexcept;

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
} // namespace kottos
