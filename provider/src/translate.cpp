// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "provider/translate.hpp"

#include <charconv>
#include <ctime>
#include <string>
#include <string_view>

#include "provider/json.hpp"

namespace llmbridge::provider
{
    namespace
    {
        long long to_ll(std::string_view s)
        {
            long long v = 0;
            std::from_chars(s.data(), s.data() + s.size(), v);
            return v;
        }

        // Current epoch seconds as text for the OpenAI response `created` field. A
        // bare 0 confuses some SDK clients; the response is synthesized "now", which
        // is exactly OpenAI's semantics. (time() is a fast vDSO read on Linux.)
        std::string created_now()
        {
            return std::to_string(static_cast<long long>(std::time(nullptr)));
        }

        // Append the raw (already JSON-escaped) text of a content field — a plain
        // string, or an array of {type:"text",text:...} parts — to `out`, WITHOUT
        // surrounding quotes. Zero-copy passthrough: the input's escaping is exactly
        // the output's, so nothing is decoded or re-escaped; the bytes are viewed
        // straight out of the request/response buffer.
        void append_text(std::string& out, const json::Value* c)
        {
            if (!c) return;
            if (c->is_string()) { out += c->sv; return; }
            if (c->is_array())
                for (const auto& part : c->arr)
                    if (part.str_or("type") == "text") out += part.str_or("text");
        }
    } // namespace

    // ── Anthropic Messages ──────────────────────────────────────────────────

    std::string openai_to_anthropic_request(std::string_view openai_body)
    {
        bool ok = false;
        json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};

        std::string system;        // collected system content (raw), joined by \n
        bool has_system = false;
        std::string messages = "["; // anthropic user/assistant turns
        bool first = true;
        if (const json::Value* msgs = v.find("messages"); msgs && msgs->is_array())
        {
            for (const auto& m : msgs->arr)
            {
                const std::string_view role = m.str_or("role");
                const json::Value* content = m.find("content");
                if (role == "system")
                {
                    if (has_system) system += "\\n"; // escaped newline in the output
                    append_text(system, content);
                    has_system = true;
                    continue;
                }
                if (!first) messages += ',';
                first = false;
                messages += "{\"role\":";
                json::append_raw_string(messages, role);
                messages += ",\"content\":\"";
                append_text(messages, content);
                messages += "\"}";
            }
        }
        messages += ']';

        std::string out = "{\"model\":";
        json::append_raw_string(out, v.str_or("model", "claude-3-5-sonnet-latest"));
        // Anthropic requires max_tokens; default if the OpenAI request omitted it.
        out += ",\"max_tokens\":";
        out += v.num_or("max_tokens", "1024");
        if (has_system)
        {
            out += ",\"system\":\"";
            out += system;
            out += '"';
        }
        if (std::string_view t = v.num_or("temperature"); !t.empty()) { out += ",\"temperature\":"; out += t; }
        if (std::string_view p = v.num_or("top_p"); !p.empty()) { out += ",\"top_p\":"; out += p; }
        out += ",\"messages\":";
        out += messages;
        out += "}";
        return out;
    }

    std::string anthropic_to_openai_response(std::string_view anthropic_body)
    {
        bool ok = false;
        json::Value v = json::parse(anthropic_body, ok);
        if (!ok || !v.is_object()) return {};

        // stop_reason -> OpenAI finish_reason.
        const std::string_view sr = v.str_or("stop_reason", "end_turn");
        const char* finish = sr == "max_tokens" ? "length" : sr == "tool_use" ? "tool_calls" : "stop";

        long long in_tok = 0, out_tok = 0;
        if (const json::Value* u = v.find("usage"))
        {
            in_tok = to_ll(u->num_or("input_tokens", "0"));
            out_tok = to_ll(u->num_or("output_tokens", "0"));
        }

        std::string out = "{\"id\":";
        json::append_raw_string(out, v.str_or("id", "chatcmpl-llmbridge"));
        out += ",\"object\":\"chat.completion\",\"created\":" + created_now() + ",\"model\":";
        json::append_raw_string(out, v.str_or("model"));
        out += ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"";
        // content: array of blocks; emit the text of text-blocks verbatim.
        if (const json::Value* c = v.find("content"); c && c->is_array())
            for (const auto& blk : c->arr)
                if (blk.str_or("type") == "text") out += blk.str_or("text");
        out += "\"},\"finish_reason\":\"";
        out += finish;
        out += "\"}],\"usage\":{\"prompt_tokens\":" + std::to_string(in_tok) +
               ",\"completion_tokens\":" + std::to_string(out_tok) +
               ",\"total_tokens\":" + std::to_string(in_tok + out_tok) + "}}";
        return out;
    }

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
        if (const json::Value* msgs = v.find("messages"); msgs && msgs->is_array())
        {
            for (const auto& m : msgs->arr)
            {
                const std::string_view role = m.str_or("role");
                const json::Value* content = m.find("content");
                if (role == "system")
                {
                    if (has_system) system += "\\n";
                    append_text(system, content);
                    has_system = true;
                    continue;
                }
                if (!first) contents += ',';
                first = false;
                // OpenAI "assistant" -> Gemini "model"; everything else -> "user".
                contents += "{\"role\":";
                contents += role == "assistant" ? "\"model\"" : "\"user\"";
                contents += ",\"parts\":[{\"text\":\"";
                append_text(contents, content);
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
        // generationConfig — only the keys the OpenAI request actually set.
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

    std::string gemini_to_openai_response(std::string_view gemini_body)
    {
        bool ok = false;
        json::Value v = json::parse(gemini_body, ok);
        if (!ok || !v.is_object()) return {};

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

        long long in_tok = 0, out_tok = 0, total = 0;
        if (const json::Value* u = v.find("usageMetadata"))
        {
            in_tok = to_ll(u->num_or("promptTokenCount", "0"));
            out_tok = to_ll(u->num_or("candidatesTokenCount", "0"));
            total = to_ll(u->num_or("totalTokenCount", "0"));
        }
        if (total == 0) total = in_tok + out_tok;

        std::string out = "{\"id\":\"chatcmpl-llmbridge\",\"object\":\"chat.completion\",\"created\":"
                          + created_now() + ",\"model\":";
        json::append_raw_string(out, v.str_or("modelVersion"));
        out += ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"";
        if (parts)
            for (const auto& part : parts->arr) out += part.str_or("text");
        out += "\"},\"finish_reason\":\"";
        out += finish;
        out += "\"}],\"usage\":{\"prompt_tokens\":" + std::to_string(in_tok) +
               ",\"completion_tokens\":" + std::to_string(out_tok) +
               ",\"total_tokens\":" + std::to_string(total) + "}}";
        return out;
    }

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
        if (const json::Value* msgs = v.find("messages"); msgs && msgs->is_array())
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
                append_text(messages, content);
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

        long long in_tok = 0, out_tok = 0;
        if (const json::Value* u = v.find("usage"))
            if (const json::Value* t = u->find("tokens"))
            {
                in_tok = to_ll(t->num_or("input_tokens", "0"));
                out_tok = to_ll(t->num_or("output_tokens", "0"));
            }

        std::string out = "{\"id\":";
        json::append_raw_string(out, v.str_or("id", "chatcmpl-llmbridge"));
        out += ",\"object\":\"chat.completion\",\"created\":" + created_now() + ",\"model\":";
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
               ",\"total_tokens\":" + std::to_string(in_tok + out_tok) + "}}";
        return out;
    }
} // namespace llmbridge::provider
