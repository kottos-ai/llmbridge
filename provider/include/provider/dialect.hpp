// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The venues in one table: each one's name, body dialect, credential scheme and
// streaming, its translators, the path it is sent to and its finish reasons.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "provider/json.hpp"
#include "provider/openai.hpp"

namespace llmbridge::provider
{
    /// Where a request is sent. Bedrock and Azure carry transport quirks over a body
    /// dialect: DESIGN.md "Dialect resolution".
    enum class Venue
    {
        OpenAI,
        Anthropic,
        Gemini,
        Cohere,
        Bedrock,
        Azure,
        Vertex, ///< Gemini on Google Cloud: Gemini's body, the publisher's model path
    };

    /// A body's wire format, apart from auth and URL layout.
    enum class Wire : uint8_t { OpenAI, Anthropic, Gemini, Cohere };

    /// How a venue takes the client's credential.
    enum class Auth : uint8_t { Forward, XApiKey, GoogApiKey, Bearer, ApiKey, SigV4 };

    /// What a request translator writes. `model`, when set, replaces the client's; it
    /// is JSON string content with nothing to escape.
    struct RequestOut
    {
        std::string& body;           ///< the venue's body, capacity kept
        std::string_view raw{};      ///< the client's body as sent, for a venue that forwards it
        std::string_view model{};
        bool include_usage = false;  ///< `stream_options.include_usage`, as read
    };

    /// Request members a translator forwards byte for byte and never reads into (a
    /// tool's `parameters` schema), so a body is parsed without building them.
    inline constexpr std::string_view kOpaqueRequestMembers[] = {"parameters"};

    /// The client's OpenAI body, parsed for a request translator.
    inline json::Value parse_request_body(std::string_view body, bool& ok)
    {
        return json::parse(body, ok, json::Keys::Unique, kOpaqueRequestMembers);
    }

    struct FinishRule
    {
        std::string_view from;
        openai::Finish to;
    };

    struct VenueSpec
    {
        std::string_view name; ///< the one spelling: flags, config and logs
        Wire wire;
        Auth auth;
        bool streams;      ///< a translated stream is served; else stream:true is refused
        bool same_wire_forwards; ///< a client of the same wire is byte-forwarded
        bool (*request)(const json::Value& in, RequestOut& out); ///< null: byte-forward
        bool (*response)(std::string_view body, std::string& out, openai::Usage& u);
        bool (*target)(std::string_view model, std::string& path); ///< appends; false: no model
        std::span<const FinishRule> finish;
        openai::Finish finish_default;
    };

    [[nodiscard]] const VenueSpec& spec(Venue v) noexcept;
    [[nodiscard]] std::optional<Venue> venue_named(std::string_view name) noexcept;
    /// Every venue's name, in Venue order.
    [[nodiscard]] std::span<const std::string_view> venue_names() noexcept;

    /// A venue's finish reason in OpenAI's terms, by its table.
    [[nodiscard]] openai::Finish finish_of(const VenueSpec& s, std::string_view reason) noexcept;

    /// Whether `s` can stand inside a JSON string unescaped: no quote, backslash or control.
    [[nodiscard]] constexpr bool plain_json_text(std::string_view s) noexcept
    {
        for (const char ch : s)
        {
            const auto u = static_cast<unsigned char>(ch);
            if (u < 0x20 || u == 0x7F || ch == '"' || ch == '\\') return false;
        }
        return true;
    }

    namespace detail
    {
        constexpr bool ends_with(std::string_view s, std::string_view tail) noexcept
        {
            return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
        }
    } // namespace detail

    /// What the caller speaks, from its method and full request target: exact routes on
    /// the path, the query stripped. Anything else is OpenAI.
    [[nodiscard]] constexpr Wire client_wire(std::string_view method, std::string_view target) noexcept
    {
        const std::string_view path = target.substr(0, target.find('?'));
        if (detail::ends_with(path, "/chat/completions")) return Wire::OpenAI; // the common case
        // Anthropic's batches take GET and DELETE as well; everything else here is a POST.
        if (path.find("/v1/messages/batches") != std::string_view::npos) return Wire::Anthropic;
        if (method != "POST") return Wire::OpenAI;
        if (detail::ends_with(path, "/v1/messages") || detail::ends_with(path, "/v1/messages/count_tokens"))
            return Wire::Anthropic;
        if (detail::ends_with(path, ":generateContent") || detail::ends_with(path, ":streamGenerateContent"))
            return Wire::Gemini;
        if (detail::ends_with(path, "/v2/chat")) return Wire::Cohere;
        return Wire::OpenAI;
    }

    /// Whether `m` can stand as a model's path segment as it is: 1 to 255 bytes of
    /// [A-Za-z0-9._-], so it can neither leave its segment nor need encoding.
    [[nodiscard]] constexpr bool path_model_ok(std::string_view m) noexcept
    {
        if (m.empty() || m.size() > 255) return false;
        for (const char c : m)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '-'))
                return false;
        return true;
    }

    /// Where a Gemini target names its model: the segment after the last `/models/` of
    /// the path, up to the method's colon (`/v1beta/models/{model}:generateContent`, and
    /// Vertex's `.../publishers/google/models/{model}:...`). `at` is its offset in
    /// `target`; npos when the path names none.
    struct PathModel
    {
        size_t at = std::string_view::npos;
        std::string_view model{};
    };
    [[nodiscard]] constexpr PathModel gemini_path_model(std::string_view target) noexcept;

    /// A Vertex path under a project, `/v1/projects/{p}/locations/{l}/...`, in the path
    /// and not the query: the only layout an OAuth token is served at.
    [[nodiscard]] constexpr bool gemini_vertex_scoped(std::string_view target) noexcept
    {
        const std::string_view path = target.substr(0, target.find('?'));
        return path.find("/projects/") != std::string_view::npos && path.find("/locations/") != std::string_view::npos;
    }

    /// A Gemini client's target as `venue` takes it, appended to `into`: its model
    /// replaced by `model` when set (path_model_ok), and its layout the venue's when the
    /// client wrote the other host's. A Gemini API venue gets `/v1beta/models/{model}:...`
    /// for a Vertex publisher path; a Vertex venue gets `/v1/publishers/google/models/
    /// {model}:...` for a Gemini API path; a project-scoped Vertex path goes as written.
    /// False when `model` is set and the target names none to replace.
    bool gemini_forward_target(std::string_view target, std::string_view model, Venue venue, std::string& into);

    constexpr PathModel gemini_path_model(std::string_view target) noexcept
    {
        const std::string_view path = target.substr(0, target.find('?'));
        const size_t colon = path.rfind(':');
        if (colon == std::string_view::npos) return {};
        constexpr std::string_view kModels = "/models/";
        const size_t m = path.substr(0, colon).rfind(kModels);
        if (m == std::string_view::npos) return {};
        const size_t at = m + kModels.size();
        return {at, path.substr(at, colon - at)};
    }
} // namespace llmbridge::provider
