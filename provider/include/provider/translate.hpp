// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Provider dialect translation: the C++ analog of LiteLLM's
// transform_request / transform_response. The OpenAI chat-completions dialect is
// the canonical "in" format; each target is a structurally-different provider
// wire format we translate to and back from.
//
// Covered targets (the high-demand, structurally-distinct ones):
//   - Anthropic Messages         (system extraction, content blocks, stop_reason)
//   - Google Gemini              (contents/parts, role "model", generationConfig)
//   - Cohere Chat v2             (messages, top_p -> "p", content blocks)
// OpenAI-compatible providers (Groq, Together, Fireworks, DeepInfra, Mistral,
// Perplexity, xAI, OpenRouter, Cerebras, vLLM, ...) need no body translation because
// the gateway byte-forwards them (UpstreamDialect::OpenAI) and only rewrites
// auth/endpoint. So this module covers the cases where the body actually changes.
//
// Scope per target: the common chat path (model, system, user/assistant turns,
// max_tokens/temperature/top_p; response content / finish-reason / usage).
// Representative, not 100% provider-complete: streaming deltas, tool-calling and
// cache_control are built; vision is not. Operates on the JSON *body* (the
// gateway handles HTTP re-framing). Returns "" on parse failure so the caller
// can fail the request.

#include <string>
#include <string_view>

namespace llmbridge::provider
{
    // ── Anthropic Messages ──────────────────────────────────────────────────
    // OpenAI chat-completion request body  ->  Anthropic Messages request body.
    /// Replace the top-level `"model"` value in an OpenAI request body, leaving every
    /// other byte alone. Empty return = refuse, never a partial edit.
    ///
    /// A splice, not a re-serialisation. The body is forwarded to a venue that already
    /// speaks this dialect, so re-emitting it from a parse would silently drop any
    /// field this parser does not model, and providers add fields faster than we
    /// adopt them. Only the model's value span moves.
    ///
    /// The TOP-LEVEL key only. A message whose content happens to contain `"model"` is
    /// text, and rewriting inside it would corrupt a prompt.
    ///
    /// Refuses a replacement carrying a quote, a backslash or a control byte: those
    /// would need escaping, a model id never contains them, and guessing at the
    /// escaping of a string that lands in a request body is how an injection starts.
    std::string rewrite_model(std::string_view openai_body, std::string_view model);

    /// The model the client asked for, as a view into `body`. Empty when the body is
    /// not an object, names no top-level `model`, or spells it with anything other
    /// than a plain string.
    ///
    /// Top-level only. A value carrying a backslash is refused instead of unescaped: this is compared
    /// against configured names, an escape means it was never one of them, and
    /// unescaping here would need an allocation on a path that has none.
    std::string_view model_of(std::string_view body) noexcept;

    /// Whether the request body asks for a streamed response: top-level `stream: true`.
    bool wants_stream(std::string_view body) noexcept;

    /// `wants_stream_usage`, when given, reports the request's top-level
    /// `stream_options.include_usage`.
    std::string openai_to_anthropic_request(std::string_view openai_body,
                                            bool* wants_stream_usage = nullptr);

    /// The same Messages body as Bedrock wants it, and the model id it names.
    ///
    /// Two differences from Anthropic direct, both required: no `model` field, because
    /// Bedrock takes the model id in the request path, and `anthropic_version` inside
    /// the JSON, where Anthropic wants a header. `model_out` receives the id so the
    /// caller can build `/model/{id}/invoke`; it is empty only when the body named no
    /// model, in which case there is no path to build and the request must be refused.
    std::string openai_to_bedrock_request(std::string_view openai_body,
                                          std::string& model_out);
    // Anthropic Messages response body  ->  OpenAI chat-completion response body.
    std::string anthropic_to_openai_response(std::string_view anthropic_body);

    // An upstream ERROR body -> the OpenAI error envelope, so a failing provider
    // (rate limit, overloaded GPU, context-length, auth) reaches the client as a
    // real, actionable error instead of a generic gateway failure. Anthropic sends
    // {"type":"error","error":{"type":"overloaded_error","message":"..."}}; we emit
    // {"error":{"message":"...","type":"...","code":null}}. Never returns empty.
    // An unparseable/foreign body still yields a valid envelope carrying `fallback`
    // as the type, so the caller can always relay the upstream's status code.
    std::string upstream_error_to_openai(std::string_view body, std::string_view fallback_type);

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
