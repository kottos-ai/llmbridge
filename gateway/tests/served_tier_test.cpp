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
