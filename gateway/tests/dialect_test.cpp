// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "gateway/dialect.hpp"

#include <gtest/gtest.h>

using llmbridge::client_dialect_from_target;
using llmbridge::Dialect;
using llmbridge::resolve_translation;
using llmbridge::UpstreamDialect;
using llmbridge::TranslationPlan;
using llmbridge::venue_body_dialect;

TEST(Dialect, TargetNamesTheClientDialect)
{
    EXPECT_EQ(client_dialect_from_target("/v1/messages"), Dialect::Anthropic);
    EXPECT_EQ(client_dialect_from_target("/v1/chat/completions"), Dialect::OpenAI);
    EXPECT_EQ(client_dialect_from_target("/v1beta/models/gemini-2.5-flash:generateContent"),
              Dialect::Gemini);
    EXPECT_EQ(client_dialect_from_target("/v2/chat"), Dialect::Cohere);
    // Unknown falls back to OpenAI: the historical assumption, so old behaviour holds.
    EXPECT_EQ(client_dialect_from_target("/"), Dialect::OpenAI);
    EXPECT_EQ(client_dialect_from_target("/healthz"), Dialect::OpenAI);
}

TEST(Dialect, VenueModeCarriesABodyDialect)
{
    EXPECT_EQ(venue_body_dialect(UpstreamDialect::OpenAI), Dialect::OpenAI);
    EXPECT_EQ(venue_body_dialect(UpstreamDialect::Anthropic), Dialect::Anthropic);
    EXPECT_EQ(venue_body_dialect(UpstreamDialect::Gemini), Dialect::Gemini);
    EXPECT_EQ(venue_body_dialect(UpstreamDialect::Cohere), Dialect::Cohere);
    EXPECT_EQ(venue_body_dialect(UpstreamDialect::Bedrock), Dialect::Anthropic);
    EXPECT_EQ(venue_body_dialect(UpstreamDialect::Azure), Dialect::OpenAI);
}

// The fix this whole change exists for: an Anthropic caller against an Anthropic venue
// byte-forwards. Before, the venue's Anthropic mode meant "translate an OpenAI client",
// so the caller got an OpenAI response it could not parse.
TEST(Dialect, SameDialectByteForwards)
{
    auto p = resolve_translation(Dialect::Anthropic, UpstreamDialect::Anthropic, /*stream=*/false);
    EXPECT_TRUE(p.ok);
    EXPECT_FALSE(p.translate) << "Anthropic->Anthropic must not translate";

    auto q = resolve_translation(Dialect::OpenAI, UpstreamDialect::OpenAI, /*stream=*/false);
    EXPECT_TRUE(q.ok);
    EXPECT_FALSE(q.translate);
}

// The built OpenAI-in path is untouched: an OpenAI caller still gets the venue's mode.
TEST(Dialect, OpenAiClientKeepsTheExistingTranslation)
{
    for (UpstreamDialect v : {UpstreamDialect::Anthropic, UpstreamDialect::Gemini, UpstreamDialect::Cohere})
    {
        auto p = resolve_translation(Dialect::OpenAI, v, /*stream=*/false);
        EXPECT_TRUE(p.ok);
        EXPECT_TRUE(p.translate);
        EXPECT_EQ(p.venue, v);
    }
}

// The Anthropic-in direction is not built, so it is refused, not mistranslated. The
// request must never reach the venue.
TEST(Dialect, UnbuiltPairsFailClosed)
{
    auto p = resolve_translation(Dialect::Anthropic, UpstreamDialect::OpenAI, /*stream=*/false); // -> OpenAI venue
    EXPECT_FALSE(p.ok);
    EXPECT_NE(std::string(p.why).find("no translator"), std::string::npos);

    auto q = resolve_translation(Dialect::Gemini, UpstreamDialect::Anthropic, /*stream=*/false);
    EXPECT_FALSE(q.ok);
}

// Bedrock and Azure bundle SigV4 / URL rewrites over their body dialect, so they keep
// their OpenAI-client path and refuse any other caller; dropping the transport is the
// failure this guards.
TEST(Dialect, BedrockAndAzureStayOpenAiOnly)
{
    for (UpstreamDialect v : {UpstreamDialect::Bedrock, UpstreamDialect::Azure})
    {
        auto ok = resolve_translation(Dialect::OpenAI, v, /*stream=*/false);
        EXPECT_TRUE(ok.ok);
        EXPECT_TRUE(ok.translate);
        EXPECT_EQ(ok.venue, v) << "OpenAI client must keep the transport-bundled mode";

        auto no = resolve_translation(Dialect::Anthropic, v, /*stream=*/false);
        EXPECT_FALSE(no.ok) << "a non-OpenAI client must not silently drop SigV4/URL rewrite";
    }
}

// A streamed request to a Bedrock venue is refused, because Bedrock streams from
// /model/{id}/invoke-with-response-stream in AWS event-stream framing and none of
// that is built. Before this, `stream:true` was passed through to /invoke and the
// service rejected a request we already knew could not work, which reached the
// client as a provider error with our name nowhere on it.
TEST(Dialect, BedrockRefusesAStreamedRequest)
{
    const TranslationPlan p =
        resolve_translation(Dialect::OpenAI, UpstreamDialect::Bedrock, /*stream=*/true);
    EXPECT_FALSE(p.ok);
    EXPECT_STREQ(p.why, "bedrock venue cannot stream");
    // And the same venue still serves a non-streamed request.
    EXPECT_TRUE(resolve_translation(Dialect::OpenAI, UpstreamDialect::Bedrock,
                                    /*stream=*/false).ok);
}

TEST(Dialect, StreamingChangesNothingForEveryOtherVenue)
{
    // The flag is a Bedrock fact, not a general one: every other venue streams, and a
    // guard that quietly refused elsewhere would break the workload we are built for.
    for (UpstreamDialect v : {UpstreamDialect::OpenAI, UpstreamDialect::Anthropic,
                            UpstreamDialect::Gemini, UpstreamDialect::Cohere,
                            UpstreamDialect::Azure})
    {
        const TranslationPlan s = resolve_translation(Dialect::OpenAI, v, /*stream=*/true);
        const TranslationPlan n = resolve_translation(Dialect::OpenAI, v, /*stream=*/false);
        EXPECT_EQ(s.ok, n.ok) << static_cast<int>(v);
        EXPECT_TRUE(s.ok) << static_cast<int>(v);
        EXPECT_EQ(s.translate, n.translate) << static_cast<int>(v);
        EXPECT_EQ(s.venue, n.venue) << static_cast<int>(v);
    }
    // An Anthropic client streaming to an Anthropic venue is the Claude Code path.
    EXPECT_TRUE(resolve_translation(Dialect::Anthropic, UpstreamDialect::Anthropic,
                                    /*stream=*/true).ok);
}
