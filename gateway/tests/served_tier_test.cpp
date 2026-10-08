// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// note_served_tier (stream.hpp) runs on every chunk of every stream, on both
// backends. Introduced without a bound (v0.49.0): a venue that never sends
// service_tier meant scanning every chunk of the stream, for its whole life,
// for nothing. Fixed the next day (v0.50.0) by giving up after kTierTries
// chunks. No test reached either version. These pin the bound in place.

#include "stream.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{
    using llmbridge::Connection;
    using llmbridge::detail::kTierTries;
    using llmbridge::detail::note_served_model;
    using llmbridge::detail::note_served_tier;
} // namespace

TEST(ServedTier, GivesUpAfterFourTriesWithNothingFound)
{
    Connection c;
    const std::string no_tier = R"({"id":"x","choices":[{"delta":{"content":"hi"}}]})";
    for (int i = 0; i < 4; ++i)
    {
        note_served_tier(&c, no_tier, /*tail=*/false);
        EXPECT_EQ(c.served_tier_tries, i + 1);
    }
    // A 5th, 6th, 100th chunk must not keep scanning: the whole point of the
    // bound is that a chatty stream with no such field pays this cost once,
    // not once per chunk for its entire life.
    for (int i = 0; i < 96; ++i) note_served_tier(&c, no_tier, /*tail=*/false);
    EXPECT_EQ(c.served_tier_tries, kTierTries) << "tries must not exceed the cap";
    EXPECT_EQ(c.served_tier_len, 0);
}

TEST(ServedTier, FindsItInTheHeadOfAChunk)
{
    Connection c;
    const std::string chunk =
        R"({"id":"x","service_tier":"flex","choices":[{"delta":{"content":"hi"}}]})";
    note_served_tier(&c, chunk, /*tail=*/false);
    ASSERT_GT(c.served_tier_len, 0);
    EXPECT_EQ(std::string(c.served_tier, c.served_tier_len), "flex");
}

TEST(ServedTier, FindsItInTheTailOfANonStreamedBody)
{
    Connection c;
    // Far enough from the front that a head-only search (kTierHead) would miss it;
    // this is what the non-streaming caller passes tail=true for.
    const std::string padding(3000, ' ');
    const std::string body =
        R"({"id":"x","choices":[{"message":{"content":"hi"}}])" + padding +
        R"(,"service_tier":"priority","usage":{"total_tokens":9}})";
    note_served_tier(&c, body, /*tail=*/true);
    ASSERT_GT(c.served_tier_len, 0);
    EXPECT_EQ(std::string(c.served_tier, c.served_tier_len), "priority");
}

TEST(ServedTier, OnceFoundIsNeverOverwritten)
{
    Connection c;
    const std::string first = R"({"service_tier":"flex","choices":[]})";
    note_served_tier(&c, first, /*tail=*/false);
    ASSERT_EQ(std::string(c.served_tier, c.served_tier_len), "flex");

    // A later chunk naming a different tier must not replace it: the field is a
    // property of the whole response, not of the chunk it happened to arrive in.
    const std::string second = R"({"service_tier":"priority","choices":[]})";
    note_served_tier(&c, second, /*tail=*/false);
    EXPECT_EQ(std::string(c.served_tier, c.served_tier_len), "flex");
    // Finding it on the first try must not have spent any of the retry budget:
    // the two guards (found vs. exhausted) are independent conditions.
    EXPECT_EQ(c.served_tier_tries, 1);
}

TEST(ServedTier, GivingUpIsPermanentEvenIfATierAppearsLater)
{
    Connection c;
    const std::string no_tier = R"({"choices":[{"delta":{"content":"x"}}]})";
    for (int i = 0; i < kTierTries; ++i) note_served_tier(&c, no_tier, /*tail=*/false);
    ASSERT_EQ(c.served_tier_tries, kTierTries);

    // A field that only shows up after the budget is spent is never recorded.
    // Deliberate: "give up quickly" would not hold if a late arrival reopened it.
    const std::string late = R"({"service_tier":"flex","choices":[]})";
    note_served_tier(&c, late, /*tail=*/false);
    EXPECT_EQ(c.served_tier_len, 0);
}

// note_served_model shares the budget's shape: found once, or given up on after
// kTierTries reads, and it reads only the top-level model of each event.
TEST(ServedModel, ReadsTheTopLevelModelOfEachDialectAndNothingNested)
{
    Connection a;
    note_served_model(&a, "event: message_start\ndata: {\"type\":\"message_start\","
                          "\"message\":{\"id\":\"m\",\"model\":\"claude-x\"}}\n\n", true);
    EXPECT_EQ(std::string(a.served_model, a.served_model_len), "claude-x");

    Connection o;
    note_served_model(&o, "data: {\"id\":\"c\",\"meta\":{\"model\":\"decoy\"},\"model\":\"gpt-x\"}\n\n",
                      true);
    EXPECT_EQ(std::string(o.served_model, o.served_model_len), "gpt-x");

    Connection n;
    note_served_model(&n, R"({"choices":[{"message":{"model":"decoy"}}]})", false);
    EXPECT_EQ(n.served_model_len, 0);
    // A model in a later event of the first read is found; one in a ping is not a model.
    Connection l;
    note_served_model(&l, "event: ping\ndata: {\"type\":\"ping\"}\n\n"
                          "data: {\"model\":\"later\"}\n\n", true);
    EXPECT_EQ(std::string(l.served_model, l.served_model_len), "later");
}

TEST(ServedModel, GivesUpAfterFourTriesAndCutsAt64Bytes)
{
    Connection c;
    for (int i = 0; i < 10; ++i) note_served_model(&c, "data: {\"choices\":[]}\n\n", true);
    EXPECT_EQ(c.served_model_tries, kTierTries);
    note_served_model(&c, "data: {\"model\":\"late\"}\n\n", true);
    EXPECT_EQ(c.served_model_len, 0) << "giving up is permanent";

    Connection t;
    note_served_model(&t, R"({"model":")" + std::string(100, 'm') + R"("})", false);
    EXPECT_EQ(std::string(t.served_model, t.served_model_len), std::string(64, 'm'));
}
