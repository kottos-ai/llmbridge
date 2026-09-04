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
#include "openai_common.hpp" // detail::created_now / to_ll
#include "provider/json.hpp"

namespace llmbridge::provider
{
    using detail::append_text;

    // ── Cohere (Chat API v2, /v2/chat) ──────────────────────────────────────

    std::string openai_to_cohere_request(std::string_view openai_body)
    {
        bool ok = false;
        json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};

        // Cohere v2 keeps OpenAI-style system/user/assistant turns; the body
        // differences are top_p -> "p" and the response shape.
        std::string messages = "[";
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
                if (!first) messages += ',';
                first = false;
                messages += "{\"role\":";
                json::append_raw_string(messages, role);
                messages += ",\"content\":\"";
                if (!append_text(messages, content)) return {};
                messages += "\"}";
            }
        }
        messages += ']';

        std::string out = "{\"model\":";
        json::append_raw_string(out, v.str_or("model", "command-r-plus"));
        out += ",\"messages\":";
        out += messages;
        if (std::string_view mt = v.num_or("max_tokens"); !mt.empty()) { out += ",\"max_tokens\":"; out += mt; }
        if (std::string_view t = v.num_or("temperature"); !t.empty()) { out += ",\"temperature\":"; out += t; }
        if (std::string_view p = v.num_or("top_p"); !p.empty()) { out += ",\"p\":"; out += p; } // Cohere: top_p is "p"
        out += "}";
        return out;
    }

    std::string cohere_to_openai_response(std::string_view cohere_body)
    {
        bool ok = false;
        json::Value v = json::parse(cohere_body, ok);
        if (!ok || !v.is_object()) return {};

        // Cohere finish_reason -> OpenAI finish_reason.
        const std::string_view fr = v.str_or("finish_reason", "COMPLETE");
        const char* finish = fr == "MAX_TOKENS" ? "length" : fr == "TOOL_CALL" ? "tool_calls" : "stop";

        long long in_tok = 0, out_tok = 0, cached_tok = 0;
        if (const json::Value* u = v.find("usage"))
            if (const json::Value* t = u->find("tokens"))
            {
                in_tok = detail::to_ll(t->num_or("input_tokens", "0"));
                out_tok = detail::to_ll(t->num_or("output_tokens", "0"));
                cached_tok = detail::to_ll(t->num_or("cache_read_input_tokens", "0"));
            }

        std::string out = "{\"id\":";
        json::append_raw_string(out, v.str_or("id", "chatcmpl-llmbridge"));
        out += ",\"object\":\"chat.completion\",\"created\":" + detail::created_now() + ",\"model\":";
        json::append_raw_string(out, v.str_or("model"));
        out += ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"";
        if (const json::Value* msg = v.find("message"))
            if (const json::Value* c = msg->find("content"); c && c->is_array())
                for (const auto& blk : c->arr)
                    if (blk.str_or("type") == "text") out += blk.str_or("text");
        out += "\"},\"finish_reason\":\"";
        out += finish;
        out += "\"}],\"usage\":{\"prompt_tokens\":" + std::to_string(in_tok) +
               ",\"completion_tokens\":" + std::to_string(out_tok) +
               ",\"total_tokens\":" + std::to_string(in_tok + out_tok);
        if (cached_tok > 0)
            out += ",\"prompt_tokens_details\":{\"cached_tokens\":" +
                   std::to_string(cached_tok) + "}";
        out += "}}";
        return out;
    }
} // namespace llmbridge::provider
