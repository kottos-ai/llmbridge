// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// OpenAI <-> Cohere Chat v2, whole bodies only.

#include "provider/translate.hpp"

#include <string>
#include <string_view>

#include "content.hpp"
#include "json_scan.hpp"
#include "messages.hpp"
#include "openai_common.hpp" // detail::now_secs / as_written
#include "provider/json.hpp"

namespace llmbridge::provider
{
    using detail::append_text;

    // ── Cohere (Chat API v2, /v2/chat) ──────────────────────────────────────

    namespace
    {
        // Cohere v2 keeps OpenAI-style system/user/assistant turns. No tool hooks:
        // Cohere tool calls and results are not translated, so they are refused.
        struct CohereTurns
        {
            std::string& out;
            bool first = true;

            bool put(std::string_view role, const json::Value* content)
            {
                if (!first) out += ',';
                first = false;
                out += role;
                if (!append_text(out, content)) return false;
                out += "\"}";
                return true;
            }
            bool system(const json::Value* c) { return put(R"({"role":"system","content":")", c); }
            bool turn(bool assistant, const json::Value* c)
            {
                return put(assistant ? R"({"role":"assistant","content":")"
                                     : R"({"role":"user","content":")", c);
            }
        };
    } // namespace

    std::string openai_to_cohere_request(std::string_view openai_body)
    {
        bool ok = false;
        const json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};
        // No `messages` array is not a chat request, and no model names nothing to run.
        const json::Value* msgs = v.find("messages");
        const std::string_view model = v.str_or("model");
        if (!msgs || !msgs->is_array() || model.empty() || detail::declares_tools(v)) return {};

        std::string out;
        out.reserve(openai_body.size() + 256);
        out = "{\"model\":";
        json::append_raw_string(out, model);
        out += ",\"messages\":[";
        CohereTurns turns{out};
        if (!detail::walk_messages(*msgs, turns)) return {};
        out += ']';
        if (std::string_view mt = detail::max_tokens_of(v); !mt.empty()) { out += ",\"max_tokens\":"; out += mt; }
        if (std::string_view t = v.num_or("temperature"); !t.empty()) { out += ",\"temperature\":"; out += t; }
        if (std::string_view p = v.num_or("top_p"); !p.empty()) { out += ",\"p\":"; out += p; } // Cohere: top_p is "p"
        out += "}";
        return out;
    }

    bool cohere_to_openai_response(std::string_view cohere_body, std::string& out,
                                   openai::Usage& usage)
    {
        out.clear();
        usage = {};
        bool ok = false;
        json::Value v = json::parse(cohere_body, ok);
        if (!ok || !v.is_object()) return false;

        // Cohere finish_reason -> OpenAI finish_reason.
        const std::string_view fr = v.str_or("finish_reason", "COMPLETE");
        const char* finish = fr == "MAX_TOKENS" ? "length" : fr == "TOOL_CALL" ? "tool_calls" : "stop";

        if (const json::Value* u = v.find("usage"))
            if (const json::Value* t = u->find("tokens"))
            {
                usage.in = json_scan::count(t->num_or("input_tokens"));
                usage.out = json_scan::count(t->num_or("output_tokens"));
                usage.cached = json_scan::count(t->num_or("cache_read_input_tokens"));
            }
        detail::as_written(usage);

        if (out.capacity() < cohere_body.size() + 256) out.reserve(cohere_body.size() + 256);
        char created[24];
        openai::Envelope e(out, openai::Shape::Completion, v.str_or("id", "chatcmpl-llmbridge"),
                           openai::decimal(created, detail::now_secs()), v.str_or("model"));
        e.choice();
        out += "\"role\":\"assistant\",\"content\":\"";
        if (const json::Value* msg = v.find("message"))
            if (const json::Value* c = msg->find("content"); c && c->is_array())
                for (const auto& blk : c->arr)
                    if (blk.str_or("type") == "text") out += blk.str_or("text");
        out += '"';
        e.end_choice(finish).close(&usage);
        return true;
    }

    std::string cohere_to_openai_response(std::string_view cohere_body)
    {
        std::string out;
        openai::Usage u;
        cohere_to_openai_response(cohere_body, out, u);
        return out;
    }
} // namespace llmbridge::provider
