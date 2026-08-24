// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <string_view>

#include "gateway/gateway.hpp" // TranslateMode

/// @file
/// The translation to run is a function of two dialects, not one: what the caller
/// speaks and what the venue speaks. `TranslateMode` alone answered only the second
/// while assuming the first was OpenAI, which mistranslated any Anthropic-speaking
/// client. These pure functions resolve the pair. See DESIGN.md "Dialect resolution".
namespace llmbridge
{
    /// A body wire format. This is the payload shape only; a venue's auth scheme and
    /// URL layout are separate axes that `TranslateMode`'s Bedrock/Azure still carry.
    enum class Dialect
    {
        OpenAI,
        Anthropic,
        Gemini,
        Cohere,
    };

    /// What the caller speaks, from the request target. The caller reveals it by the
    /// endpoint it calls. An unknown target is OpenAI, the historical assumption, so a
    /// path this does not recognise behaves exactly as it did before dialects existed.
    [[nodiscard]] inline Dialect client_dialect_from_target(std::string_view target) noexcept
    {
        constexpr auto kNpos = std::string_view::npos;
        if (target.find("/chat/completions") != kNpos) return Dialect::OpenAI;
        if (target.find("/messages") != kNpos) return Dialect::Anthropic;
        if (target.find("generateContent") != kNpos) return Dialect::Gemini; // :[stream]generateContent
        if (target.find("/v2/chat") != kNpos) return Dialect::Cohere;
        return Dialect::OpenAI;
    }

    /// The body dialect a venue speaks. Bedrock carries an Anthropic body under SigV4;
    /// Azure carries an OpenAI body under a rewritten URL; None is an OpenAI-compatible
    /// venue.
    [[nodiscard]] inline Dialect venue_body_dialect(TranslateMode venue) noexcept
    {
        switch (venue)
        {
            case TranslateMode::Anthropic:
            case TranslateMode::Bedrock:
                return Dialect::Anthropic;
            case TranslateMode::Gemini:
                return Dialect::Gemini;
            case TranslateMode::Cohere:
                return Dialect::Cohere;
            case TranslateMode::None:
            case TranslateMode::Azure:
                return Dialect::OpenAI;
        }
        return Dialect::OpenAI;
    }

    /// The translation to run for one (client, venue) pair. `ok == false` means no
    /// translator exists for the pair and the request must be refused, never sent: the
    /// caller speaks a dialect the venue's path was not built to receive.
    struct TranslationPlan
    {
        bool ok = false;
        TranslateMode mode = TranslateMode::None;
        const char* why = "";
    };

    [[nodiscard]] inline TranslationPlan resolve_translation(Dialect client,
                                                             TranslateMode venue) noexcept
    {
        // Bedrock and Azure run SigV4 or a URL rewrite that a same-dialect byte-forward
        // would drop, and both are built for an OpenAI caller only. Leave them as they
        // are; refuse any other caller so no unsigned or mis-targeted bytes are sent.
        if (venue == TranslateMode::Bedrock || venue == TranslateMode::Azure)
        {
            if (client == Dialect::OpenAI) return {true, venue, ""};
            return {false, venue, "venue needs an OpenAI-dialect client"};
        }
        if (client == venue_body_dialect(venue)) return {true, TranslateMode::None, ""};
        if (client == Dialect::OpenAI) return {true, venue, ""};
        return {false, venue, "no translator for this client dialect to this venue"};
    }
} // namespace llmbridge
