// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The venue table: names, capabilities, translators, paths and finish reasons.

#include "provider/dialect.hpp"

#include <string>
#include <string_view>

#include "provider/translate.hpp"
#include "venues.hpp"

namespace llmbridge::provider
{
    namespace
    {
        using F = openai::Finish;

        // Each provider's documented stop reasons. One not listed is the venue's default.
        constexpr FinishRule kAnthropicFinish[] = {
            {"end_turn", F::Stop},
            {"stop_sequence", F::Stop},
            {"pause_turn", F::Stop},
            {"max_tokens", F::Length},
            {"model_context_window_exceeded", F::Length},
            {"tool_use", F::ToolCalls},
            {"refusal", F::ContentFilter},
        };
        constexpr FinishRule kGeminiFinish[] = {
            {"STOP", F::Stop},
            {"MAX_TOKENS", F::Length},
            {"SAFETY", F::ContentFilter},
            {"RECITATION", F::ContentFilter},
            {"LANGUAGE", F::ContentFilter},
            {"BLOCKLIST", F::ContentFilter},
            {"PROHIBITED_CONTENT", F::ContentFilter},
            {"SPII", F::ContentFilter},
            {"IMAGE_SAFETY", F::ContentFilter},
            {"MALFORMED_FUNCTION_CALL", F::Error},
            {"UNEXPECTED_TOOL_CALL", F::Error},
            {"TOO_MANY_TOOL_CALLS", F::Error},
        };
        constexpr FinishRule kCohereFinish[] = {
            {"COMPLETE", F::Stop},
            {"STOP_SEQUENCE", F::Stop},
            {"MAX_TOKENS", F::Length},
            {"TOOL_CALL", F::ToolCalls},
            {"ERROR", F::Error},
            {"TIMEOUT", F::Error},
        };

        // A model as one path segment: decoded from its JSON form, then every byte
        // outside RFC 3986's unreserved set percent-encoded, slashes included, since an
        // inference-profile id may hold them.
        bool append_segment(std::string& path, std::string_view model)
        {
            std::string decoded;
            if (model.find('\\') != std::string_view::npos) model = decoded = json::unescape_string(model);
            if (model.empty()) return false;
            static constexpr char kHex[] = "0123456789ABCDEF";
            for (const char ch : model)
            {
                const auto c = static_cast<unsigned char>(ch);
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~')
                    path += ch;
                else
                    path.append({'%', kHex[c >> 4], kHex[c & 15]});
            }
            return true;
        }

        bool anthropic_target(std::string_view, std::string& path)
        {
            path += "/v1/messages";
            return true;
        }
        bool cohere_target(std::string_view, std::string& path)
        {
            path += "/v2/chat";
            return true;
        }
        bool azure_target(std::string_view, std::string& path)
        {
            path += "/chat/completions";
            return true;
        }
        // Gemini names the model in the path; a `models/` prefix is the API's own.
        bool gemini_target(std::string_view model, std::string& path)
        {
            if (model.substr(0, 7) == "models/") model.remove_prefix(7);
            path += "/v1beta/models/";
            if (!append_segment(path, model)) return false;
            path += ":generateContent";
            return true;
        }
        // Vertex's global publisher path, which an API key reaches with no project.
        bool vertex_target(std::string_view model, std::string& path)
        {
            if (model.substr(0, 7) == "models/") model.remove_prefix(7);
            path += "/v1/publishers/google/models/";
            if (!append_segment(path, model)) return false;
            path += ":generateContent";
            return true;
        }
        bool bedrock_target(std::string_view model, std::string& path)
        {
            path += "/model/";
            if (!append_segment(path, model)) return false;
            path += "/invoke";
            return true;
        }

        // Azure answers in the OpenAI shape it was asked in.
        bool azure_response(std::string_view body, std::string& out, openai::Usage& u)
        {
            out.assign(body);
            u = openai::scan_usage(body);
            return !out.empty();
        }
        bool anthropic_response(std::string_view body, std::string& out, openai::Usage& u)
        {
            return anthropic_to_openai_response(body, out, u);
        }
        bool gemini_response(std::string_view body, std::string& out, openai::Usage& u)
        {
            return gemini_to_openai_response(body, out, u);
        }
        bool cohere_response(std::string_view body, std::string& out, openai::Usage& u)
        {
            return cohere_to_openai_response(body, out, u);
        }

        constexpr std::string_view kNames[] = {"openai", "anthropic", "gemini", "cohere", "bedrock", "azure", "vertex"};

        // Indexed by Venue. Bedrock answers with Anthropic's envelope, so it shares the
        // response leg; it cannot stream, since its stream is AWS event-stream framing.
        const VenueSpec kSpecs[] = {
            {kNames[0], Wire::OpenAI, Auth::Forward, true, true, nullptr, nullptr, nullptr, {}, F::Stop},
            {kNames[1], Wire::Anthropic, Auth::XApiKey, true, true, detail::anthropic_request,
             anthropic_response, anthropic_target, kAnthropicFinish, F::Stop},
            {kNames[2], Wire::Gemini, Auth::GoogApiKey, false, true, detail::gemini_request,
             gemini_response, gemini_target, kGeminiFinish, F::Stop},
            {kNames[3], Wire::Cohere, Auth::Bearer, false, true, detail::cohere_request,
             cohere_response, cohere_target, kCohereFinish, F::Stop},
            {kNames[4], Wire::Anthropic, Auth::SigV4, false, false, detail::bedrock_request,
             anthropic_response, bedrock_target, kAnthropicFinish, F::Stop},
            {kNames[5], Wire::OpenAI, Auth::ApiKey, true, false, detail::azure_request,
             azure_response, azure_target, {}, F::Stop},
            {kNames[6], Wire::Gemini, Auth::GoogApiKey, false, true, detail::gemini_request,
             gemini_response, vertex_target, kGeminiFinish, F::Stop},
        };
        static_assert(std::size(kSpecs) == std::size(kNames));
    } // namespace

    // Azure serves the OpenAI dialect: the body goes as the client wrote it, so an option
    // we do not model is not dropped, with an override spliced over the model's value.
    bool detail::azure_request(const json::Value& in, RequestOut& o)
    {
        o.body.clear();
        const std::string_view raw = o.raw.empty() ? in.sv : o.raw;
        if (o.model.empty())
        {
            o.body.assign(raw);
            return !raw.empty();
        }
        const json::Value* m = in.find("model");
        if (!m || !m->is_string() || m->sv.data() < raw.data() ||
            m->sv.data() + m->sv.size() > raw.data() + raw.size())
            return false;
        const size_t at = static_cast<size_t>(m->sv.data() - raw.data());
        o.body.append(raw.substr(0, at)).append(o.model).append(raw.substr(at + m->sv.size()));
        return true;
    }

    const VenueSpec& spec(Venue v) noexcept { return kSpecs[static_cast<size_t>(v)]; }

    bool gemini_forward_target(std::string_view target, std::string_view model, Venue venue, std::string& into)
    {
        const PathModel pm = gemini_path_model(target);
        if (pm.at == std::string_view::npos)
        {
            if (!model.empty()) return false;
            into.append(target);
            return true;
        }
        constexpr std::string_view kPublisher = "/publishers/google/models/";
        std::string_view head = target.substr(0, pm.at);
        const bool publisher = head.ends_with(kPublisher);
        if (venue == Venue::Vertex && !publisher) head = "/v1/publishers/google/models/";
        else if (venue == Venue::Gemini && publisher) head = "/v1beta/models/";
        into.append(head).append(model.empty() ? pm.model : model).append(target.substr(pm.at + pm.model.size()));
        return true;
    }

    std::optional<Venue> venue_named(std::string_view name) noexcept
    {
        for (size_t i = 0; i < std::size(kNames); ++i)
            if (kNames[i] == name) return static_cast<Venue>(i);
        return std::nullopt;
    }

    std::span<const std::string_view> venue_names() noexcept { return kNames; }

    openai::Finish finish_of(const VenueSpec& s, std::string_view reason) noexcept
    {
        for (const FinishRule& r : s.finish)
            if (r.from == reason) return r.to;
        return s.finish_default;
    }
} // namespace llmbridge::provider
