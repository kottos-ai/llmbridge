// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Provider dialect translation — the C++ analog of LiteLLM's
// transform_request / transform_response. The OpenAI chat-completions dialect is
// the canonical "in" format; each target is a structurally-different provider
// wire format we translate to and back from.
//
// Covered targets (the high-demand, structurally-distinct ones):
//   - Anthropic Messages         (system extraction, content blocks, stop_reason)
//   - Google Gemini              (contents/parts, role "model", generationConfig)
//   - Cohere Chat v2             (messages, top_p -> "p", content blocks)
// OpenAI-compatible providers (Groq, Together, Fireworks, DeepInfra, Mistral,
// Perplexity, xAI, OpenRouter, Cerebras, vLLM, ...) need NO body translation —
// the gateway byte-forwards them (TranslateMode::None) and only rewrites
// auth/endpoint. So this module covers the cases where the body actually changes.
//
// Scope per target: the common chat path (model, system, user/assistant turns,
// max_tokens/temperature/top_p; response content / finish-reason / usage).
// Representative, not 100% provider-complete — streaming deltas, tool-calling,
// vision, and cache_control are Phase B. Operates on the JSON *body* (the
// gateway handles HTTP re-framing). Returns "" on parse failure so the caller
// can fail the request.

#include <string>
#include <string_view>

namespace llmbridge::provider
{
    // ── Anthropic Messages ──────────────────────────────────────────────────
    // OpenAI chat-completion request body  ->  Anthropic Messages request body.
    std::string openai_to_anthropic_request(std::string_view openai_body);
    // Anthropic Messages response body  ->  OpenAI chat-completion response body.
    std::string anthropic_to_openai_response(std::string_view anthropic_body);

    // ── Google Gemini (generateContent) ─────────────────────────────────────
    // OpenAI chat-completion request body  ->  Gemini generateContent body.
    std::string openai_to_gemini_request(std::string_view openai_body);
    // Gemini generateContent response body  ->  OpenAI chat-completion body.
    std::string gemini_to_openai_response(std::string_view gemini_body);

    // ── Cohere Chat v2 ──────────────────────────────────────────────────────
    // OpenAI chat-completion request body  ->  Cohere /v2/chat request body.
    std::string openai_to_cohere_request(std::string_view openai_body);
    // Cohere /v2/chat response body  ->  OpenAI chat-completion response body.
    std::string cohere_to_openai_response(std::string_view cohere_body);
} // namespace llmbridge::provider
