// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "provider/translate.hpp"

#include <charconv>
#include <string>
#include <string_view>

#include "openai_common.hpp" // detail::created_now / anthropic_finish_reason
#include "provider/json.hpp"

namespace llmbridge::provider
{
    namespace
    {
        // Append the raw (already JSON-escaped) text of a content field: a plain
        // string, or an array of {type:"text",text:...} parts, to `out`, WITHOUT
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


    namespace
    {
        // ── Tool calling: OpenAI <-> Anthropic ──────────────────────────────
        //
        // The two dialects disagree in three places, and each one is a real
        // conversion instead of a rename:
        //
        //  1. TOOL DECLARATION
        //       OpenAI     {"type":"function","function":{name,description,parameters}}
        //       Anthropic  {name,description,input_schema}
        //     `parameters`/`input_schema` is an arbitrary JSON Schema: forwarded as a
        //     RAW SPAN, never rebuilt, so we cannot corrupt a customer's schema.
        //
        //  2. THE ASSISTANT'S CALL
        //       OpenAI     tool_calls[].function.arguments  -> a JSON *string*
        //       Anthropic  content[].input                  -> a JSON *object*
        //     So crossing this boundary means unescaping a string into JSON one way
        //     and escaping JSON into a string the other. This is the fiddly part and
        //     the reason json.hpp grew unescape_string/append_escaped_string.
        //
        //  3. THE RESULT
        //       OpenAI     a message with role:"tool" + tool_call_id
        //       Anthropic  a USER message whose content is a tool_result block
        //     Consecutive OpenAI tool messages merge into ONE Anthropic user turn.
        //     NOT because the API demands it: measured against the live API, two
        //     consecutive user turns return 200, and an earlier version of this
        //     comment claimed otherwise. We merge because a parallel tool call IS
        //     semantically one turn of results, so this produces the canonical shape
        //     the model was trained on instead of leaning on provider-side
        //     turn-combining we do not control.
        //
        // Anything malformed is dropped instead of guessed at: a half-translated
        // tool call would make the provider fail in a way the client cannot read.

        // OpenAI tool_choice -> Anthropic tool_choice. Returns "" when nothing
        // should be emitted (OpenAI's default, or "none" which we express by
        // omitting tools entirely. Anthropic has no exact equivalent).
        std::string anthropic_tool_choice(const json::Value* tc)
        {
            if (!tc) return {};
            if (tc->is_string())
            {
                const std::string_view s = tc->sv;
                if (s == "auto") return R"({"type":"auto"})";
                if (s == "required") return R"({"type":"any"})";
                return {}; // "none" -> caller omits tools
            }
            if (tc->is_object())
            {
                // {"type":"function","function":{"name":"x"}}
                if (const json::Value* f = tc->find("function"))
                {
                    const std::string_view n = f->str_or("name");
                    if (!n.empty())
                    {
                        std::string out = R"({"type":"tool","name":)";
                        json::append_raw_string(out, n);
                        out += '}';
                        return out;
                    }
                }
            }
            return {};
        }

        // OpenAI tools[] -> Anthropic tools[]. Empty if nothing usable.
        std::string anthropic_tools(const json::Value* tools)
        {
            if (!tools || !tools->is_array() || tools->arr.empty()) return {};
            std::string out = "[";
            bool first = true;
            for (const auto& tl : tools->arr)
            {
                const json::Value* fn = tl.find("function");
                if (!fn || !fn->is_object()) continue; // only type:"function" exists today
                const std::string_view name = fn->str_or("name");
                if (name.empty()) continue; // unusable without a name
                if (!first) out += ',';
                first = false;
                out += "{\"name\":";
                json::append_raw_string(out, name);
                if (const std::string_view d = fn->str_or("description"); !d.empty())
                {
                    out += ",\"description\":";
                    json::append_raw_string(out, d);
                }
                // The schema, byte for byte. Absent -> the empty object, which is
                // what Anthropic requires for a no-argument tool.
                out += ",\"input_schema\":";
                const json::Value* params = fn->find("parameters");
                if (params && (params->is_object() || params->is_array()) && !params->sv.empty())
                    out.append(params->sv);
                else
                    out += R"({"type":"object","properties":{}})";
                out += '}';
            }
            out += ']';
            return first ? std::string{} : out;
        }

        // Anthropic content blocks -> OpenAI tool_calls[]. Empty if none.
        std::string openai_tool_calls(const json::Value* content)
        {
            if (!content || !content->is_array()) return {};
            std::string out = "[";
            bool first = true;
            for (const auto& blk : content->arr)
            {
                if (blk.str_or("type") != "tool_use") continue;
                if (!first) out += ',';
                first = false;
                out += "{\"id\":";
                json::append_raw_string(out, blk.str_or("id"));
                out += ",\"type\":\"function\",\"function\":{\"name\":";
                json::append_raw_string(out, blk.str_or("name"));
                // input (object) -> arguments (string containing that JSON).
                out += ",\"arguments\":";
                const json::Value* in = blk.find("input");
                json::append_escaped_string(out, (in && !in->sv.empty()) ? in->sv : std::string_view{"{}"});
                out += "}}";
            }
            out += ']';
            return first ? std::string{} : out;
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
            bool in_tool_results = false; // merging consecutive OpenAI tool messages
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

                // OpenAI role:"tool" -> an Anthropic USER turn holding tool_result
                // blocks. Consecutive tool messages MERGE into one turn: Anthropic
                // rejects two user turns in a row, and a parallel tool call produces
                // exactly that shape.
                if (role == "tool")
                {
                    if (!in_tool_results)
                    {
                        if (!first) messages += ',';
                        first = false;
                        messages += R"({"role":"user","content":[)";
                        in_tool_results = true;
                    } // else: keep appending into the open turn
                    else
                    {
                        messages += ',';
                    }
                    messages += R"({"type":"tool_result","tool_use_id":)";
                    json::append_raw_string(messages, m.str_or("tool_call_id"));
                    messages += R"(,"content":")";
                    append_text(messages, content);
                    messages += "\"}";
                    continue;
                }
                if (in_tool_results) { messages += "]}"; in_tool_results = false; }

                // An assistant turn carrying tool_calls becomes an Anthropic
                // assistant turn whose content is an ARRAY: optional text, then one
                // tool_use block per call.
                const json::Value* tcs = m.find("tool_calls");
                if (role == "assistant" && tcs && tcs->is_array() && !tcs->arr.empty())
                {
                    if (!first) messages += ',';
                    first = false;
                    messages += R"({"role":"assistant","content":[)";
                    bool any = false;
                    std::string text;
                    append_text(text, content);
                    if (!text.empty())
                    {
                        messages += R"({"type":"text","text":")";
                        messages += text;
                        messages += "\"}";
                        any = true;
                    }
                    for (const auto& call : tcs->arr)
                    {
                        const json::Value* fn = call.find("function");
                        if (!fn) continue;
                        const std::string_view name = fn->str_or("name");
                        if (name.empty()) continue; // unusable; drop instead of guess
                        if (any) messages += ',';
                        any = true;
                        messages += R"({"type":"tool_use","id":)";
                        json::append_raw_string(messages, call.str_or("id"));
                        messages += ",\"name\":";
                        json::append_raw_string(messages, name);
                        // arguments (a JSON *string*) -> input (a JSON *object*).
                        messages += ",\"input\":";
                        const std::string args = json::unescape_string(fn->str_or("arguments"));
                        messages += args.empty() ? "{}" : args;
                        messages += '}';
                    }
                    messages += "]}";
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
            if (in_tool_results) messages += "]}"; // close a trailing tool_result turn
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
        // Pass streaming through: Anthropic uses the same `stream` flag, so an
        // OpenAI `stream:true` request becomes an Anthropic SSE response.
        if (const json::Value* s = v.find("stream"); s && s->type == json::Value::Type::Bool && s->boolean)
            out += ",\"stream\":true";
        // Tools. tool_choice:"none" means "do not call tools", which Anthropic
        // expresses by there being none, so we omit the whole tools block.
        const json::Value* tc = v.find("tool_choice");
        const bool choice_none = tc && tc->is_string() && tc->sv == "none";
        if (!choice_none)
        {
            if (const std::string tools = anthropic_tools(v.find("tools")); !tools.empty())
            {
                out += ",\"tools\":";
                out += tools;
                if (const std::string ch = anthropic_tool_choice(tc); !ch.empty())
                {
                    out += ",\"tool_choice\":";
                    out += ch;
                }
            }
        }
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
        const char* finish = detail::anthropic_finish_reason(sr);

        long long in_tok = 0, out_tok = 0, cached_tok = 0;
        if (const json::Value* u = v.find("usage"))
        {
            in_tok = detail::to_ll(u->num_or("input_tokens", "0"));
            out_tok = detail::to_ll(u->num_or("output_tokens", "0"));
            cached_tok = detail::to_ll(u->num_or("cache_read_input_tokens", "0"));
        }

        std::string out = "{\"id\":";
        json::append_raw_string(out, v.str_or("id", "chatcmpl-llmbridge"));
        out += ",\"object\":\"chat.completion\",\"created\":" + detail::created_now() + ",\"model\":";
        json::append_raw_string(out, v.str_or("model"));
        out += ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":";
        // content: array of blocks; emit the text of text-blocks verbatim.
        const json::Value* c = v.find("content");
        const std::string tool_calls = openai_tool_calls(c);
        {
            std::string text;
            if (c && c->is_array())
                for (const auto& blk : c->arr)
                    if (blk.str_or("type") == "text") text += blk.str_or("text");
            // OpenAI sets content to NULL (not "") on a pure tool call. SDKs branch
            // on that, so emitting "" would look like an empty answer instead of a
            // call.
            if (text.empty() && !tool_calls.empty()) out += "null";
            else { out += '"'; out += text; out += '"'; }
        }
        if (!tool_calls.empty())
        {
            out += ",\"tool_calls\":";
            out += tool_calls;
        }
        out += "},\"finish_reason\":\"";
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

    std::string upstream_error_to_openai(std::string_view body, std::string_view fallback_type)
    {
        // Pull {type,message} from the shapes providers actually send:
        //   Anthropic: {"type":"error","error":{"type":"...","message":"..."}}
        //   OpenAI-ish/Gemini: {"error":{"message":"...","type"|"status":"..."}}
        // Anything unrecognised still yields a valid envelope (never empty), so the
        // caller can always relay the upstream status instead of masking it as 502.
        std::string_view type = fallback_type;
        std::string_view message;
        bool ok = false;
        json::Value v = json::parse(body, ok);
        if (ok && v.is_object())
        {
            const json::Value* e = v.find("error");
            if (e && e->is_object())
            {
                if (std::string_view t = e->str_or("type"); !t.empty()) type = t;
                else if (std::string_view s = e->str_or("status"); !s.empty()) type = s;
                message = e->str_or("message");
            }
            else if (e && e->is_string()) // {"error":"some text"}
            {
                message = e->sv;
            }
            if (message.empty()) message = v.str_or("message");
        }

        // Both spans come from an UNTRUSTED upstream body, so sanitize on the way
        // out: the same rule the SSE passthrough follows. Otherwise a provider could
        // make our own error envelope unparseable to a strict client.
        //
        // This is now DEFENCE IN DEPTH instead of the primary mitigation: the
        // parser rejects raw control bytes in strings outright (RFC 8259 §7), so a
        // body carrying one no longer parses and `message`/`type` come back empty,
        // yielding the generic envelope below. An earlier version of this comment
        // said "our parser is lenient about control bytes"; that was true when it
        // was written and is not any more. Kept because it costs nothing on an
        // error path and the invariant it guards (no raw control byte in an
        // envelope we emit) is the one that must not regress.
        std::string out = "{\"error\":{\"message\":\"";
        if (message.empty()) out += "upstream provider error";
        else detail::append_sanitized(out, message);
        out += "\",\"type\":\"";
        detail::append_sanitized(out, type);
        out += "\",\"code\":null}}";
        return out;
    }

    bool openai_wants_stream_usage(std::string_view openai_body)
    {
        // Fail fast: the field is absent from virtually every request, and a
        // substring miss costs one scan instead of a full parse.
        if (openai_body.find("include_usage") == std::string_view::npos) return false;
        bool ok = false;
        json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return false;
        const json::Value* so = v.find("stream_options");
        if (!so || !so->is_object()) return false;
        const json::Value* iu = so->find("include_usage");
        return iu && iu->type == json::Value::Type::Bool && iu->boolean;
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
            in_tok = detail::to_ll(u->num_or("promptTokenCount", "0"));
            out_tok = detail::to_ll(u->num_or("candidatesTokenCount", "0"));
            total = detail::to_ll(u->num_or("totalTokenCount", "0"));
        }
        if (total == 0) total = in_tok + out_tok;

        std::string out = "{\"id\":\"chatcmpl-llmbridge\",\"object\":\"chat.completion\",\"created\":"
                          + detail::created_now() + ",\"model\":";
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
