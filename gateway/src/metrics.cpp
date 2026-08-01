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
        static const int64_t mono0 = now_ns();
        static const int64_t wall0 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
        return wall0 + (mono_ns - mono0);
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
