// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "gateway/metrics.hpp"

#include <chrono>
#include <cstdio>

namespace llmbridge
{
    int64_t now_ns() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    int64_t wall_ns(int64_t mono_ns) noexcept
    {
        // Anchors captured together, once, on first use. `static` init is
        // thread-safe in C++11+ and this is not a hot path (once per response).
        //
        // CAVEAT; this DRIFTS. It assumes both clocks tick at the same rate after
        // the anchor; under NTP slew they do not, and the error grows with process
        // uptime. Good enough for correlating a request against an external log at
        // ms granularity, which is all `x-llmbridge-t0` promises. NOT an ordering
        // primitive: use `seq` for that. See LATENCY.md §3.
        static const int64_t mono0 = now_ns();
        static const int64_t wall0 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
        return wall0 + (mono_ns - mono0);
    }

    TimingSplit timing_split(int64_t t0, int64_t t1, int64_t t2, int64_t t3, int64_t t4,
                             int64_t t5) noexcept
    {
        const int64_t wire = t2 ? t2 : t1; // no connect stamped => wire-ready at t1
        TimingSplit s{};
        s.connect_ns = wire - t1;
        s.upwrite_ns = t3 - wire;
        s.upstream_ns = t4 - t3;
        s.req_path_ns = (t1 - t0) + s.upwrite_ns;
        s.compute_ns = (t1 - t0) + (t5 - t4);
        return s;
    }

    namespace
    {
        // Format a ns duration in whatever unit reads best.
        void fmt_ns(char* out, size_t out_sz, uint64_t ns)
        {
            if (ns < 10'000ULL)
                std::snprintf(out, out_sz, "%llu ns", static_cast<unsigned long long>(ns));
            else if (ns < 10'000'000ULL)
                std::snprintf(out, out_sz, "%.2f us", static_cast<double>(ns) / 1000.0);
            else
                std::snprintf(out, out_sz, "%.2f ms", static_cast<double>(ns) / 1'000'000.0);
        }
    } // namespace

    void Histogram::print(std::ostream& os, const char* label) const
    {
        // An empty histogram must NOT print zeros. It did once, and "p99=0 ns" reads
        // as a spectacular result instead of as no data, while bench/run_bench.sh
        // seds this very line for `p99=`, so a zero-sample run would have published a
        // fabricated 0 us added latency. Streaming workloads hit this every time:
        // streams are counted in `requests` but never recorded here (LATENCY.md §4).
        if (_total == 0)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s  count=0  (no samples)\n", label);
            os << buf;
            return;
        }
        char p50[32], p99[32], p999[32], pmax[32];
        fmt_ns(p50, sizeof(p50), percentile(0.50));
        fmt_ns(p99, sizeof(p99), percentile(0.99));
        fmt_ns(p999, sizeof(p999), percentile(0.999));
        fmt_ns(pmax, sizeof(pmax), _max);

        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%s  count=%llu  p50=%s  p99=%s  p99.9=%s  max=%s%s\n",
                      label, static_cast<unsigned long long>(_total),
                      p50, p99, p999, pmax, _overflow ? "  [overflow!]" : "");
        os << buf;
    }
} // namespace llmbridge
