// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// body_facts is the one read of the request body a policy or sink causes. Its
// correctness half is here; so are loose timing bounds that only a pathological
// shape can trip (extra walks, a quadratic skip, a full walk where an early stop
// was meant), in the style of AuthHeaders.StaysWithinAnOrderOfMagnitudeOfOneWalk.
// Percentiles are in body_perf_test.cpp, which ctest never runs.

#include "body_shape.hpp"
#include "request.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace
{
    using llmbridge::detail::body_facts;
    using llmbridge::provider::KeyCheck;
    using llmbridge::test::agent_body;

    const std::string kOpenAi = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\n\r\n";
    const std::string kAnthropic = "POST /v1/messages HTTP/1.1\r\nHost: x\r\n\r\n";
} // namespace

TEST(BodyFacts, AnOpenAIClientGetsTheStreamFlagsAndTheKeyCheck)
{
    // The facts are views into the body, so it has to outlive them.
    const std::string body = agent_body(4000, true);
    const auto f = body_facts(kOpenAi, body);
    EXPECT_EQ(f.model, "gpt-6");
    EXPECT_TRUE(f.stream);
    EXPECT_TRUE(f.include_usage);
    EXPECT_EQ(f.keys, KeyCheck::Ok);
    EXPECT_EQ(body_facts(kOpenAi, R"({"model":"a","stream":true,"model":"b"})").keys,
              KeyCheck::DuplicateKey);
    // An unknown path is OpenAI, the gateway's historical assumption.
    EXPECT_TRUE(body_facts("POST /v9/x HTTP/1.1\r\n\r\n", R"({"stream":true})").stream);
}

// Usage on any other dialect comes from the venue's own events, and a second model
// is caught by the sink against the model served, so the body is read for the
// model alone and stops there.
TEST(BodyFacts, AnotherDialectGetsTheModelAloneAndNoKeyCheck)
{
    const auto f = body_facts(kAnthropic,
                              R"({"model":"a","stream":true,"stream_options":{"include_usage":true},)"
                              R"("model":"b","stream":true})");
    EXPECT_EQ(f.model, "a") << "the first, as model_of reads it";
    EXPECT_FALSE(f.stream);
    EXPECT_FALSE(f.include_usage);
    EXPECT_EQ(f.keys, KeyCheck::Ok);
    // The dialect comes from the request line only, never from a header.
    const std::string spoof = "POST /v1/chat/completions HTTP/1.1\r\nX-Path: /v1/messages\r\n\r\n";
    EXPECT_TRUE(body_facts(spoof, R"({"stream":true})").stream);
}

#if defined(__has_feature)
    #define LLMBRIDGE_TEST_UNDER_CLANG_SANITIZER \
        (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
#else
    #define LLMBRIDGE_TEST_UNDER_CLANG_SANITIZER 0
#endif
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !LLMBRIDGE_TEST_UNDER_CLANG_SANITIZER
namespace
{
    // The fastest of several rounds, each averaging many calls: a context switch
    // lands in one round and the minimum discards it.
    template <class F>
    double best_us(F&& f, int calls)
    {
        double best = 1e300;
        for (int round = 0; round < 7; ++round)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < calls; ++i) f();
            const double us = std::chrono::duration<double, std::micro>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count() /
                              calls;
            best = std::min(best, us);
        }
        return best;
    }

    volatile size_t g_sink = 0;

    double facts_us(const std::string& head, const std::string& body, int calls)
    {
        return best_us([&] { g_sink += body_facts(head, body).model.size(); }, calls);
    }
} // namespace

// Relative to the same process's bare walk, so runner speed cancels. The single
// pass measured 1.0-1.1x a bare walk; the four separate walks it replaced read ~6x
// on this body, because every key sits after the messages.
TEST(BodyFactsPerf, AnOpenAIBodyIsReadInOneWalk)
{
    const std::string body = agent_body(200'000, true);
    const double walk = best_us([&] { g_sink += llmbridge::provider::top_level_member_count(body); }, 40);
    const double facts = facts_us(kOpenAi, body, 40);
    EXPECT_LT(facts, 2.0 * walk) << "facts " << facts << " us, one walk " << walk << " us";
}

// A sanity bound, not a benchmark. Measured 73 us p50 on an i7-9750H laptop,
// Release, 2026-10-08 (body_perf_test.cpp); this passes anything under 1,000 us, an
// order of magnitude of headroom, and takes the best of seven rounds of 40 calls,
// so a slower runner or a preemption cannot trip it. Extra walks are the ratio
// test's job; this catches the walk itself getting an order of magnitude slower.
TEST(BodyFactsPerf, A200KBodyStaysWithinAnOrderOfMagnitudeOfBaseline)
{
    const double us = facts_us(kOpenAi, agent_body(200'000, true), 40);
    EXPECT_LT(us, 1000.0) << us << " us";
}

// A quadratic skip reads ~27x worse per KB at 800 KB than at 30 KB; a linear one
// reads about the same. 3x leaves room for 800 KB falling out of the cache.
TEST(BodyFactsPerf, CostGrowsLinearlyWithTheBody)
{
    const double small = facts_us(kOpenAi, agent_body(30'000, true), 200) / 30.0;
    const double large = facts_us(kOpenAi, agent_body(800'000, true), 10) / 800.0;
    EXPECT_LT(large, 3.0 * small) << "us/KB at 800 KB " << large << ", at 30 KB " << small;
}

// Model first, as Claude Code sends it: the read stops at the first key, so 800 KB
// costs what 2 KB does. A full walk of the larger body is ~300x the smaller one.
TEST(BodyFactsPerf, AnotherDialectStopsAtTheModel)
{
    const double small = facts_us(kAnthropic, agent_body(2'000, false), 2'000);
    const double large = facts_us(kAnthropic, agent_body(800'000, false), 2'000);
    EXPECT_LT(large, 3.0 * small + 0.05) << "800 KB " << large << " us, 2 KB " << small << " us";
    EXPECT_LT(large, 20.0) << large << " us";
}
#endif
