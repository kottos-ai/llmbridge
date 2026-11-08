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

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    using llmbridge::ClientConn;
    using llmbridge::detail::kTierTries;
    using llmbridge::detail::note_served_model;
    using llmbridge::detail::note_served_tier;
    using llmbridge::detail::note_stream_tier;

    /// A Responses stream's first and last events, each the whole response object, and a
    /// delta between them. The last names its own tier, then the client's tools and
    /// metadata, each of which can name one too.
    const std::string kCreated =
        "event: response.created\ndata: {\"type\":\"response.created\",\"sequence_number\":0,"
        "\"response\":{\"id\":\"r\",\"status\":\"in_progress\",\"output\":[],"
        "\"service_tier\":\"auto\",\"tools\":[]}}\n\n";
    const std::string kDelta =
        "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\","
        "\"delta\":\"the \\\"type\\\":\\\"response.completed\\\" event\"}\n\n";
    const std::string kCompleted =
        "event: response.completed\ndata: {\"type\":\"response.completed\",\"sequence_number\":9,"
        "\"response\":{\"id\":\"r\",\"status\":\"completed\",\"output\":[],\"service_tier\":\"default\","
        "\"tools\":[{\"type\":\"function\",\"parameters\":{\"properties\":{\"service_tier\":"
        "{\"type\":\"string\"}}}}],\"usage\":{\"input_tokens\":9,\"output_tokens\":1},"
        "\"metadata\":{\"service_tier\":\"gold\"}}}\n\n";
} // namespace

TEST(ServedTier, GivesUpAfterFourTriesWithNothingFound)
{
    ClientConn c;
    const std::string no_tier = R"({"id":"x","choices":[{"delta":{"content":"hi"}}]})";
    for (int i = 0; i < 4; ++i)
    {
        note_served_tier(&c, no_tier, /*tail=*/false);
        EXPECT_EQ(c.req.f.served_tier_tries, i + 1);
    }
    // A 5th, 6th, 100th chunk must not keep scanning: the whole point of the
    // bound is that a chatty stream with no such field pays this cost once,
    // not once per chunk for its entire life.
    for (int i = 0; i < 96; ++i) note_served_tier(&c, no_tier, /*tail=*/false);
    EXPECT_EQ(c.req.f.served_tier_tries, kTierTries) << "tries must not exceed the cap";
    EXPECT_EQ(c.req.f.served_tier.n, 0);
}

TEST(ServedTier, FindsItInTheHeadOfAChunk)
{
    ClientConn c;
    const std::string chunk =
        R"({"id":"x","service_tier":"flex","choices":[{"delta":{"content":"hi"}}]})";
    note_served_tier(&c, chunk, /*tail=*/false);
    ASSERT_GT(c.req.f.served_tier.n, 0);
    EXPECT_EQ(std::string(c.req.f.served_tier.view()), "flex");
}

TEST(ServedTier, FindsItInTheTailOfANonStreamedBody)
{
    ClientConn c;
    // Far enough from the front that a head-only search (kTierHead) would miss it;
    // this is what the non-streaming caller passes tail=true for.
    const std::string padding(3000, ' ');
    const std::string body =
        R"({"id":"x","choices":[{"message":{"content":"hi"}}])" + padding +
        R"(,"service_tier":"priority","usage":{"total_tokens":9}})";
    note_served_tier(&c, body, /*tail=*/true);
    ASSERT_GT(c.req.f.served_tier.n, 0);
    EXPECT_EQ(std::string(c.req.f.served_tier.view()), "priority");
}

TEST(ServedTier, AToolInputBeforeTheUsageCannotNameIt)
{
    // Raw JSON the model wrote, ahead of the reply's own usage block.
    ClientConn c;
    note_served_tier(&c,
                     R"({"content":[{"type":"tool_use","input":{"service_tier":"priority"}}],)"
                     R"("usage":{"input_tokens":5,"service_tier":"standard"}})",
                     /*tail=*/true);
    EXPECT_EQ(std::string(c.req.f.served_tier.view()), "standard");
    ClientConn q;
    note_served_tier(&q, R"({"choices":[{"delta":{"content":"\"service_tier\":\"x\""}}],)"
                         R"("service_tier":"flex"})",
                     /*tail=*/false);
    EXPECT_EQ(std::string(q.req.f.served_tier.view()), "flex");
}

TEST(ServedTier, OnceFoundIsNeverOverwritten)
{
    ClientConn c;
    const std::string first = R"({"service_tier":"flex","choices":[]})";
    note_served_tier(&c, first, /*tail=*/false);
    ASSERT_EQ(std::string(c.req.f.served_tier.view()), "flex");

    // A later chunk naming a different tier must not replace it: the field is a
    // property of the whole response, not of the chunk it happened to arrive in.
    const std::string second = R"({"service_tier":"priority","choices":[]})";
    note_served_tier(&c, second, /*tail=*/false);
    EXPECT_EQ(std::string(c.req.f.served_tier.view()), "flex");
    // Finding it on the first try must not have spent any of the retry budget:
    // the two guards (found vs. exhausted) are independent conditions.
    EXPECT_EQ(c.req.f.served_tier_tries, 1);
}

TEST(ServedTier, GivingUpIsPermanentEvenIfATierAppearsLater)
{
    ClientConn c;
    const std::string no_tier = R"({"choices":[{"delta":{"content":"x"}}]})";
    for (int i = 0; i < kTierTries; ++i) note_served_tier(&c, no_tier, /*tail=*/false);
    ASSERT_EQ(c.req.f.served_tier_tries, kTierTries);

    // A field that only shows up after the budget is spent is never recorded.
    // Deliberate: "give up quickly" would not hold if a late arrival reopened it.
    const std::string late = R"({"service_tier":"flex","choices":[]})";
    note_served_tier(&c, late, /*tail=*/false);
    EXPECT_EQ(c.req.f.served_tier.n, 0);
}

// note_served_model shares the budget's shape: found once, or given up on after
// kTierTries reads, and it reads only the top-level model of each event.
TEST(ServedModel, ReadsTheTopLevelModelOfEachDialectAndNothingNested)
{
    ClientConn a;
    note_served_model(&a, "event: message_start\ndata: {\"type\":\"message_start\","
                          "\"message\":{\"id\":\"m\",\"model\":\"claude-x\"}}\n\n", true);
    EXPECT_EQ(std::string(a.req.f.served_model.view()), "claude-x");

    ClientConn o;
    note_served_model(&o, "data: {\"id\":\"c\",\"meta\":{\"model\":\"decoy\"},\"model\":\"gpt-x\"}\n\n",
                      true);
    EXPECT_EQ(std::string(o.req.f.served_model.view()), "gpt-x");

    ClientConn n;
    note_served_model(&n, R"({"choices":[{"message":{"model":"decoy"}}]})", false);
    EXPECT_EQ(n.req.f.served_model.n, 0);
    // A model in a later event of the first read is found; one in a ping is not a model.
    ClientConn l;
    note_served_model(&l, "event: ping\ndata: {\"type\":\"ping\"}\n\n"
                          "data: {\"model\":\"later\"}\n\n", true);
    EXPECT_EQ(std::string(l.req.f.served_model.view()), "later");
}

TEST(ServedModel, GivesUpAfterFourTriesAndCutsAt64Bytes)
{
    ClientConn c;
    for (int i = 0; i < 10; ++i) note_served_model(&c, "data: {\"choices\":[]}\n\n", true);
    EXPECT_EQ(c.req.f.served_model_tries, kTierTries);
    note_served_model(&c, "data: {\"model\":\"late\"}\n\n", true);
    EXPECT_EQ(c.req.f.served_model.n, 0) << "giving up is permanent";

    ClientConn t;
    note_served_model(&t, R"({"model":")" + std::string(100, 'm') + R"("})", false);
    EXPECT_EQ(std::string(t.req.f.served_model.view()), std::string(64, 'm'));
}

TEST(StreamChunk, WrapsWhatWasAppendedInOneHexSizedChunk)
{
    ClientConn c;
    c.req.f.stream_chunked_out = true;
    for (const size_t n : {size_t{1}, size_t{15}, size_t{16}, size_t{4122}, size_t{1} << 20})
    {
        std::string out = "head";
        out.append(n, 'x');
        llmbridge::detail::chunk_wrap(&c, out, 4);
        char want[24];
        std::snprintf(want, sizeof want, "%zx\r\n", n);
        EXPECT_EQ(out.substr(0, 4 + std::strlen(want)), "head" + std::string(want)) << n;
        EXPECT_EQ(out.size(), 4 + std::strlen(want) + n + 2) << n;
    }
}

// A Responses stream states its tier twice: response.created echoes what the request asked,
// `auto` when it named none, and the last event states what served. Read the Chat
// Completions way, the first and only tier kept was the echo. The echo stands in until the
// last event, which comes after the whole reply, past the four reads a stream is searched.
TEST(ServedTier, AResponsesStreamTakesItsLastEventsTierOverTheEcho)
{
    ClientConn c;
    c.req.f.responses_api = true;
    note_stream_tier(&c, kCreated);
    for (int i = 0; i < 6; ++i) note_stream_tier(&c, kDelta);
    EXPECT_EQ(std::string(c.req.f.served_tier.view()), "auto")
        << "the echo stands in, and a marker quoted in the reply's text is not the last event";
    note_stream_tier(&c, kCompleted);
    EXPECT_EQ(std::string(c.req.f.served_tier.view()), "default")
        << "the tools' and the metadata's keys come after the reply's own";
}

// The last event in two reads, its type in one and its tier in the next.
TEST(ServedTier, AResponsesStreamsLastEventSplitAcrossReadsStillNamesIt)
{
    ClientConn c;
    c.req.f.responses_api = true;
    note_stream_tier(&c, kCreated);
    const size_t cut = kCompleted.find("\"output\"");
    note_stream_tier(&c, kCompleted.substr(0, cut));
    EXPECT_EQ(std::string(c.req.f.served_tier.view()), "auto");
    note_stream_tier(&c, kCompleted.substr(cut));
    EXPECT_EQ(std::string(c.req.f.served_tier.view()), "default");
}

// A Chat Completions stream is read as before: its first tier, within four reads.
TEST(ServedTier, AChatStreamIsUnchangedByTheResponsesRule)
{
    ClientConn c;
    note_stream_tier(&c, R"({"service_tier":"flex","choices":[]})");
    note_stream_tier(&c, kCompleted);
    EXPECT_EQ(std::string(c.req.f.served_tier.view()), "flex");
    EXPECT_EQ(c.req.f.served_tier_tries, 1);
}

// A Responses body states its tier ahead of the client's tools and metadata, which can run
// past the 2 KiB tail a body is searched in, and the metadata can name a tier of its own.
TEST(ServedTier, AResponsesBodyStatesItAheadOfToolsLongerThanTheTail)
{
    ClientConn c;
    c.req.f.responses_api = true;
    const std::string body = R"({"id":"r","object":"response","output":[],"service_tier":"default","tools":[)" +
                             std::string(3000, ' ') +
                             R"(],"usage":{"input_tokens":9},"metadata":{"service_tier":"gold"}})";
    note_served_tier(&c, body, /*tail=*/true);
    EXPECT_EQ(std::string(c.req.f.served_tier.view()), "default");
}

// A read boundary can fall anywhere in the last event, its type and its tier included: every
// split of it, and the whole stream in reads of 1, 3, 7 and 97 bytes, still names its tier.
TEST(ServedTier, EveryReadBoundaryInAResponsesStreamStillNamesItsTier)
{
    for (size_t cut = 1; cut < kCompleted.size(); ++cut)
    {
        ClientConn c;
        c.req.f.responses_api = true;
        note_stream_tier(&c, kCreated);
        note_stream_tier(&c, kCompleted.substr(0, cut));
        note_stream_tier(&c, kCompleted.substr(cut));
        ASSERT_EQ(std::string(c.req.f.served_tier.view()), "default") << "split at " << cut;
    }
    const std::string stream = kCreated + kDelta + kCompleted;
    for (const size_t n : {1, 3, 7, 97})
    {
        ClientConn c;
        c.req.f.responses_api = true;
        for (size_t at = 0; at < stream.size(); at += n) note_stream_tier(&c, std::string_view(stream).substr(at, n));
        EXPECT_EQ(std::string(c.req.f.served_tier.view()), "default") << "reads of " << n;
    }
}
