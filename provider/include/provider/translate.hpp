// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// OpenAI chat-completion bodies to and from Anthropic, Bedrock, Gemini and Cohere. Edits
// are splices of top-level keys, never re-serialisations: DESIGN.md "Translation model".

#include <string>
#include <string_view>

#include "provider/openai.hpp"

namespace llmbridge::provider
{
    /// Replace the top-level `"model"` value; empty means refused, never a partial edit.
    std::string rewrite_model(std::string_view openai_body, std::string_view model);

    /// Set a top-level string key, inserting it when absent; empty means refused.
    std::string upsert_string(std::string_view openai_body, std::string_view key,
                              std::string_view value, std::string_view* had = nullptr);

    /// Both of the route's overrides in one pass; either may be empty.
    std::string apply_overrides(std::string_view openai_body, std::string_view model,
                                std::string_view service_tier,
                                std::string_view* had_tier = nullptr);
    bool apply_overrides(std::string_view openai_body, std::string_view model,
                         std::string_view service_tier, std::string_view* had_tier,
                         std::string& out); // into `out`, capacity kept; false on refusal

    /// The top-level `model` as a view into `body`; empty unless it is a plain string.
    std::string_view model_of(std::string_view body) noexcept;

    bool wants_stream(std::string_view body) noexcept;    // top-level `stream: true`
    bool stream_usage_of(std::string_view body) noexcept; // `stream_options.include_usage`

    /// Whether the readers above can disagree with a provider's parser about this body.
    enum class KeyCheck { Ok, DuplicateKey, EscapedKey, TooManyKeys };
    KeyCheck top_level_key_check(std::string_view body) noexcept;

    /// The four readers above in one walk of the top level, with the same answers.
    struct TopLevelFacts
    {
        std::string_view model;
        bool stream = false;
        bool include_usage = false;
        KeyCheck keys = KeyCheck::Ok;
    };
    TopLevelFacts top_level_facts(std::string_view body) noexcept;

    /// The model a reply says served it: top-level `model`, or `message.model` in message_start.
    std::string_view reply_model(std::string_view json) noexcept;
    size_t top_level_member_count(std::string_view body) noexcept; // the bare walk, for tests

    std::string openai_to_anthropic_request(std::string_view openai_body,
                                            bool* wants_stream_usage = nullptr);
    /// The same into `out`, capacity kept across agent turns; false refuses, `out` empty.
    bool openai_to_anthropic_request(std::string_view openai_body, std::string& out,
                                     bool* wants_stream_usage = nullptr);

    /// Bedrock's Messages body; `model_out` gets the id for `/model/{id}/invoke`. No model refuses.
    std::string openai_to_bedrock_request(std::string_view openai_body,
                                          std::string& model_out);
    bool openai_to_bedrock_request(std::string_view openai_body, std::string& model_out,
                                   std::string& out); // into `out`; false refuses
    std::string anthropic_to_openai_response(std::string_view anthropic_body);
    /// The same into `out` (capacity kept), with the counts the reply states in `usage`.
    bool anthropic_to_openai_response(std::string_view anthropic_body, std::string& out,
                                      openai::Usage& usage);

    /// An upstream error body as the OpenAI error envelope. Never empty: a foreign body
    /// still yields one, typed `fallback_type`, so the upstream's status can be relayed.
    std::string upstream_error_to_openai(std::string_view body, std::string_view fallback_type);

    std::string openai_to_gemini_request(std::string_view openai_body);
    std::string gemini_to_openai_response(std::string_view gemini_body);
    bool gemini_to_openai_response(std::string_view gemini_body, std::string& out,
                                   openai::Usage& usage);
    std::string openai_to_cohere_request(std::string_view openai_body);
    std::string cohere_to_openai_response(std::string_view cohere_body);
    bool cohere_to_openai_response(std::string_view cohere_body, std::string& out,
                                   openai::Usage& usage);
} // namespace llmbridge::provider
