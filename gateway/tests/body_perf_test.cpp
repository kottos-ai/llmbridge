// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The percentile half of the body_facts guard (body_facts_test.cpp has the loose
// bounds ctest runs). Built by default and never registered with ctest, for the
// same reason as request_perf_test.cpp: a timing tolerance tight enough to read is
// too tight for a shared runner. Run ./bin/gateway_body_perf_test by hand.
//
// Baseline, i7-9750H laptop, Release build, 2026-10-08, p50: an OpenAI client's
// single pass is 1.1 us at 2 KB, 11 us at 30 KB, 73 us at 200 KB and 290 us at
// 800 KB in either key order, within 2% of a bare walk. Any other client with the
// model first is 0.1 us at every size; with the model last it is one walk. The four
// separate reads this replaced cost 485 us at 200 KB with the keys last.

#include "body_shape.hpp"
#include "request.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    double pct(const std::vector<double>& sorted, double p)
    {
        return sorted[static_cast<size_t>(p / 100.0 * static_cast<double>(sorted.size() - 1))];
    }

    template <class F>
    void report(const char* label, size_t bytes, F&& f)
    {
        for (int i = 0; i < 50; ++i) f(); // warm the caches and the predictor
        std::vector<double> us;
        for (int i = 0; i < 1000; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            f();
            us.push_back(std::chrono::duration<double, std::micro>(
                             std::chrono::steady_clock::now() - t0)
                             .count());
        }
        std::sort(us.begin(), us.end());
        std::printf("%-22s %7zu B  p50 %8.1f us  p95 %8.1f us  p99 %8.1f us\n", label, bytes,
                    pct(us, 50), pct(us, 95), pct(us, 99));
    }
} // namespace

int main()
{
    const std::string openai = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n\r\n";
    const std::string anthropic = "POST /v1/messages HTTP/1.1\r\nHost: x\r\n\r\n";
    volatile size_t sink = 0;
    for (const bool keys_last : {false, true})
        for (const size_t kb : {2, 30, 200, 800})
        {
            const std::string body = llmbridge::test::agent_body(kb * 1000, keys_last);
            const char* order = keys_last ? "keys last" : "keys first";
            char label[64];
            std::snprintf(label, sizeof label, "openai, %s", order);
            report(label, body.size(), [&] {
                sink += llmbridge::detail::body_facts(openai, body).model.size();
            });
            std::snprintf(label, sizeof label, "anthropic, %s", order);
            report(label, body.size(), [&] {
                sink += llmbridge::detail::body_facts(anthropic, body).model.size();
            });
            std::snprintf(label, sizeof label, "bare walk, %s", order);
            report(label, body.size(), [&] {
                sink += llmbridge::provider::top_level_member_count(body);
            });
        }
    return 0;
}
