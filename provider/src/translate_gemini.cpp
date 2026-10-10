// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// OpenAI <-> Google Gemini generateContent, whole bodies only.

#include "provider/translate.hpp"

#include <string>
#include <string_view>

#include "content.hpp"
#include "json_scan.hpp"
#include "openai_common.hpp" // detail::now_secs / gemini_usage
#include "provider/json.hpp"

namespace llmbridge::provider
{
    using detail::append_text;

    // ── Google Gemini (generateContent) ─────────────────────────────────────

    std::string openai_to_gemini_request(std::string_view openai_body)
    {
        bool ok = false;
        json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};

        std::string system;          // collected system content (raw), joined by \n
        bool has_system = false;
        std::string contents = "[";  // gemini "contents" turns
        bool first = true;
        // A body carrying no `messages` array is not a chat request. Refuse instead of guessing.
        const json::Value* msgs = v.find("messages");
        if (!msgs || !msgs->is_array()) return {};
        if (msgs)
        {
            for (const auto& m : msgs->arr)
            {
                const std::string_view role = m.str_or("role");
                const json::Value* content = m.find("content");
                if (role == "system")
                {
                    if (has_system) system += "\\n";
                    if (!append_text(system, content)) return {};
                    has_system = true;
                    continue;
                }
                if (!first) contents += ',';
                first = false;
                // OpenAI "assistant" -> Gemini "model"; everything else -> "user".
                contents += "{\"role\":";
                contents += role == "assistant" ? "\"model\"" : "\"user\"";
                contents += ",\"parts\":[{\"text\":\"";
                if (!append_text(contents, content)) return {};
                contents += "\"}]}";
            }
        }
        contents += ']';

        std::string out = "{\"contents\":" + contents;
        if (has_system)
        {
            out += ",\"systemInstruction\":{\"parts\":[{\"text\":\"";
            out += system;
            out += "\"}]}";
        }
        // generationConfig: only the keys the OpenAI request actually set.
        std::string gc;
        if (std::string_view mt = v.num_or("max_tokens"); !mt.empty()) { gc += "\"maxOutputTokens\":"; gc += mt; }
        if (std::string_view t = v.num_or("temperature"); !t.empty())
        {
            if (!gc.empty()) gc += ',';
            gc += "\"temperature\":";
            gc += t;
        }
        if (std::string_view p = v.num_or("top_p"); !p.empty())
        {
            if (!gc.empty()) gc += ',';
            gc += "\"topP\":";
            gc += p;
        }
        if (!gc.empty()) out += ",\"generationConfig\":{" + gc + "}";
        out += "}";
        return out;
    }

    bool gemini_to_openai_response(std::string_view gemini_body, std::string& out,
                                   openai::Usage& usage)
    {
        out.clear();
        usage = {};
        bool ok = false;
        json::Value v = json::parse(gemini_body, ok);
        if (!ok || !v.is_object()) return false;

        const json::Value* parts = nullptr; // candidates[0].content.parts
        const char* finish = "stop";
        if (const json::Value* cands = v.find("candidates"); cands && cands->is_array() && !cands->arr.empty())
        {
            const json::Value& c0 = cands->arr.front();
            if (const json::Value* cont = c0.find("content"))
                if (const json::Value* p = cont->find("parts"); p && p->is_array()) parts = p;
            // Gemini finishReason -> OpenAI finish_reason.
            const std::string_view fr = c0.str_or("finishReason", "STOP");
            finish = fr == "MAX_TOKENS" ? "length"
                   : (fr == "SAFETY" || fr == "RECITATION" || fr == "BLOCKLIST" ||
                      fr == "PROHIBITED_CONTENT") ? "content_filter"
                   : "stop";
        }

        long long total = -1;
        if (const json::Value* m = v.find("usageMetadata"))
        {
            const auto n = [m](std::string_view k) { return json_scan::count(m->num_or(k)); };
            usage = detail::gemini_usage(n("promptTokenCount"), n("candidatesTokenCount"),
                                         n("thoughtsTokenCount"), n("cachedContentTokenCount"),
                                         n("toolUsePromptTokenCount"));
            total = n("totalTokenCount");
        }
        detail::as_written(usage);
        if (total <= 0) total = usage.in + usage.out;

        if (out.capacity() < gemini_body.size() + 256) out.reserve(gemini_body.size() + 256);
        char created[24];
        openai::Envelope e(out, openai::Shape::Completion, "chatcmpl-llmbridge",
                           openai::decimal(created, detail::now_secs()), v.str_or("modelVersion"));
        e.choice();
        out += "\"role\":\"assistant\",\"content\":\"";
        if (parts)
            for (const auto& part : parts->arr) out += part.str_or("text");
        out += '"';
        e.end_choice(finish).close(&usage, false, total);
        return true;
    }

    std::string gemini_to_openai_response(std::string_view gemini_body)
    {
        std::string out;
        openai::Usage u;
        gemini_to_openai_response(gemini_body, out, u);
        return out;
    }
} // namespace llmbridge::provider
