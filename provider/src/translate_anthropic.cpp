// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// OpenAI <-> Anthropic Messages, whole bodies: tools, the request walk that
// Bedrock shares, and the response. The streamed half is sse.cpp.

#include "provider/translate.hpp"

#include <string>
#include <string_view>

#include "content.hpp"
#include "openai_common.hpp" // detail::created_now / anthropic_finish_reason
#include "provider/json.hpp"

namespace llmbridge::provider
{
    using detail::append_text;
    using detail::append_content;
    using detail::part_cache_control;

    namespace
    {
        // ── Tool calling: OpenAI <-> Anthropic ──────────────────────────────
        //
        // The two dialects disagree in three places, and each one is a real
        // conversion instead of a rename:
        //
        //  1. Tool declaration
        //       OpenAI     {"type":"function","function":{name,description,parameters}}
        //       Anthropic  {name,description,input_schema}
        //     `parameters`/`input_schema` is an arbitrary JSON Schema: forwarded as a
        //     Raw span, never rebuilt, so we cannot corrupt a customer's schema.
        //
        //  2. The assistant'S call
        //       OpenAI     tool_calls[].function.arguments  -> a JSON *string*
        //       Anthropic  content[].input                  -> a JSON *object*
        //     So crossing this boundary means unescaping a string into JSON one way
        //     and escaping JSON into a string the other. This is the fiddly part and
        //     the reason json.hpp grew unescape_string/append_escaped_string.
        //
        //  3. The result
        //       OpenAI     a message with role:"tool" + tool_call_id
        //       Anthropic  a user message whose content is a tool_result block
        //     Consecutive OpenAI tool messages merge into one Anthropic user turn.
        //     Not because the API demands it: measured against the live API, two
        //     consecutive user turns return 200, and an earlier version of this
        //     comment claimed otherwise. We merge because a parallel tool call is
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
                // A breakpoint on a tool caches the definitions above it, which is
                // the other prefix worth caching in an agent loop.
                std::string_view cc;
                if (const json::Value* fcc = fn->find("cache_control"); fcc && fcc->is_object())
                    cc = fcc->sv;
                else if (const json::Value* tcc = tl.find("cache_control"); tcc && tcc->is_object())
                    cc = tcc->sv;
                if (!cc.empty())
                {
                    out += ",\"cache_control\":";
                    out.append(cc);
                }
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

    namespace
    {
    /// Both Messages bodies, because they differ in exactly two fields and a second
    /// copy of the message walk would be a second place for tool results, vision and
    /// system-prompt handling to drift.
    ///
    /// Bedrock puts the model in the path, so its body must not carry one, and it
    /// wants `anthropic_version` in the JSON where Anthropic wants it in a header.
    /// Everything between those two is identical.
    std::string messages_request(std::string_view openai_body, bool bedrock,
                                 std::string* model_out, bool* wants_stream_usage = nullptr)
    {
        bool ok = false;
        json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};

        // Read off the DOM we already have.
        if (wants_stream_usage)
        {
            const json::Value* so = v.find("stream_options");
            const json::Value* iu = so && so->is_object() ? so->find("include_usage") : nullptr;
            *wants_stream_usage = iu && iu->type == json::Value::Type::Bool && iu->boolean;
        }

        std::string system;             // collected system content (raw), joined by \n
        std::string_view system_cache;  // its `cache_control`, if a part carried one
        bool has_system = false;
        std::string messages = "["; // anthropic user/assistant turns
        messages.reserve(openai_body.size() + 256);
        bool first = true;
        // A body carrying no `messages` array is not a chat request. Refuse instead of guessing.
        const json::Value* msgs = v.find("messages");
        if (!msgs || !msgs->is_array()) return {};
        if (msgs)
        {
            bool in_tool_results = false; // merging consecutive OpenAI tool messages
            for (const auto& m : msgs->arr)
            {
                const std::string_view role = m.str_or("role");
                const json::Value* content = m.find("content");
                if (role == "system")
                {
                    if (has_system) system += "\\n"; // escaped newline in the output
                    if (!append_text(system, content)) return {};
                    // Anthropic's `system` takes a string or an array of blocks, and
                    // only the array form carries a breakpoint.
                    if (content && content->is_array())
                        for (const auto& part : content->arr)
                        {
                            std::string_view cc;
                            if (!part_cache_control(part, cc)) return {};
                            if (!cc.empty()) system_cache = cc;
                        }
                    has_system = true;
                    continue;
                }

                // OpenAI role:"tool" -> an Anthropic user turn holding tool_result
                // blocks. Consecutive tool messages merge into one turn: Anthropic
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
                    if (!append_text(messages, content)) return {};
                    messages += "\"}";
                    continue;
                }
                if (in_tool_results) { messages += "]}"; in_tool_results = false; }

                // An assistant turn carrying tool_calls becomes an Anthropic
                // assistant turn whose content is an array: optional text, then one
                // tool_use block per call.
                const json::Value* tcs = m.find("tool_calls");
                if (role == "assistant" && tcs && tcs->is_array() && !tcs->arr.empty())
                {
                    if (!first) messages += ',';
                    first = false;
                    messages += R"({"role":"assistant","content":[)";
                    bool any = false;
                    std::string text;
                    if (!append_text(text, content)) return {};
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
                        //
                        // Parsed before it is spliced. These bytes come from the
                        // client, and appending them raw let a caller close our
                        // object and append its own top-level members: an
                        // `arguments` of `{}}]},{"role":"user",...}],"model":"theirs"`
                        // produced a syntactically valid Anthropic body we did not
                        // write. Non-JSON was worse in a quieter way: `"input":not
                        // json` is a malformed body we then sent upstream, which is
                        // sanitise-and-forward with the sanitising left out.
                        //
                        // rewrite_model refuses the same class three functions down;
                        // this is the same rule applied to the same kind of input.
                        const std::string args = json::unescape_string(fn->str_or("arguments"));
                        if (!args.empty())
                        {
                            bool arg_ok = false;
                            const json::Value parsed = json::parse(args, arg_ok);
                            if (!arg_ok || !parsed.is_object()) return {};
                            // And nothing after it. The parser stops at the end of
                            // the first value, so `{}}]},{...}` parses as a valid
                            // empty object with the payload trailing behind it, and
                            // checking only "is it an object" accepts exactly the
                            // injection this refuses. `sv` spans the object's own
                            // braces, so comparing it against the input is what
                            // makes the whole string have to be that object.
                            size_t end = args.size();
                            while (end > 0 && (args[end - 1] == ' ' || args[end - 1] == '\t' ||
                                               args[end - 1] == '\n' || args[end - 1] == '\r'))
                                --end;
                            if (parsed.sv.size() != end) return {};
                        }
                        messages += ",\"input\":";
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
                messages += ",\"content\":";
                if (!append_content(messages, content)) return {};
                messages += "}";
            }
            if (in_tool_results) messages += "]}"; // close a trailing tool_result turn
        }
        messages += ']';

        const std::string_view model = v.str_or("model", "claude-3-5-sonnet-latest");
        if (model_out) model_out->assign(model);
        std::string out = "{";
        out.reserve(messages.size() + system.size() + 512); // same reason as `messages`
        if (bedrock)
        {
            // Not a version we choose: Bedrock rejects a Messages body without it,
            // and this literal is the only value its Anthropic models accept.
            out += "\"anthropic_version\":\"bedrock-2023-05-31\"";
        }
        else
        {
            out += "\"model\":";
            json::append_raw_string(out, model);
        }
        // Anthropic requires max_tokens; default if the OpenAI request omitted it.
        out += ",\"max_tokens\":";
        out += v.num_or("max_tokens", "1024");
        if (has_system)
        {
            if (system_cache.empty())
            {
                out += ",\"system\":\"";
                out += system;
                out += '"';
            }
            else
            {
                out += ",\"system\":[{\"type\":\"text\",\"text\":\"";
                out += system;
                out += "\",\"cache_control\":";
                out.append(system_cache);
                out += "}]";
            }
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
    } // namespace

    std::string openai_to_anthropic_request(std::string_view openai_body,
                                            bool* wants_stream_usage)
    {
        return messages_request(openai_body, false, nullptr, wants_stream_usage);
    }

    std::string openai_to_bedrock_request(std::string_view openai_body,
                                          std::string& model_out)
    {
        return messages_request(openai_body, true, &model_out);
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
            // prompt_tokens is the whole prompt (OpenAI's convention): fresh input plus
            // the cache read and write legs Anthropic reports separately. cached is the
            // read subset. Matches scan_usage and the streaming translator.
            out_tok = detail::to_ll(u->num_or("output_tokens", "0"));
            cached_tok = detail::to_ll(u->num_or("cache_read_input_tokens", "0"));
            in_tok = detail::to_ll(u->num_or("input_tokens", "0")) + cached_tok +
                     detail::to_ll(u->num_or("cache_creation_input_tokens", "0"));
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
} // namespace llmbridge::provider
