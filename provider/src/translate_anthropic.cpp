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
#include "json_scan.hpp"
#include "messages.hpp"
#include "openai_common.hpp" // detail::now_secs / anthropic_finish_reason
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
        //     and escaping JSON into a string the other.
        //
        //  3. The result
        //       OpenAI     a message with role:"tool" + tool_call_id
        //       Anthropic  a user message whose content is a tool_result block
        //     Consecutive OpenAI tool messages merge into one Anthropic user turn.
        //     Not because the API demands it: measured against the live API, two
        //     consecutive user turns return 200. We merge because a parallel tool
        //     call is semantically one turn of results, the canonical shape the
        //     model was trained on.
        //
        // Anything malformed is dropped instead of guessed at: a half-translated
        // tool call would make the provider fail in a way the client cannot read.

        // OpenAI tool_choice -> Anthropic tool_choice. Nothing for OpenAI's default,
        // and "none" never gets here: append_tools omits the tools for it.
        void append_tool_choice(std::string& out, const json::Value* tc)
        {
            std::string_view type, name;
            if (tc && tc->is_string())
                type = tc->sv == "auto" ? "auto" : tc->sv == "required" ? "any" : "";
            else if (const json::Value* f = tc ? tc->find("function") : nullptr)
                if (name = f->str_or("name"); !name.empty()) type = "tool";
            if (type.empty()) return;
            out += ",\"tool_choice\":{\"type\":\"";
            out += type;
            out += '"';
            if (!name.empty())
            {
                out += ",\"name\":";
                json::append_raw_string(out, name);
            }
            out += '}';
        }

        // OpenAI tools[] -> Anthropic tools[], appended in place; nothing if none is usable.
        void append_tools(std::string& out, const json::Value& body)
        {
            // tool_choice:"none" means "do not call tools", expressed by there being none.
            const json::Value* tc = body.find("tool_choice");
            if (tc && tc->is_string() && tc->sv == "none") return;
            const json::Value* tools = body.find("tools");
            if (!tools || !tools->is_array()) return;
            const size_t at = out.size();
            out += ",\"tools\":[";
            const size_t first = out.size();
            for (const auto& tl : tools->arr)
            {
                const json::Value* fn = tl.find("function");
                if (!fn || !fn->is_object()) continue; // only type:"function" exists today
                const std::string_view name = fn->str_or("name");
                if (name.empty()) continue; // unusable without a name
                if (out.size() != first) out += ',';
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
            if (out.size() == first) { out.resize(at); return; }
            out += ']';
            append_tool_choice(out, tc);
        }

        // arguments (a JSON *string*) -> input (a JSON *object*), decoded straight into
        // `out` and parsed there, once. Parsed before it is sent: these bytes come from
        // the client, and appending them raw let a caller close our object and append
        // its own top-level members (`{}}]},{"role":"user",...}],"model":"theirs"`).
        // parse() refuses anything after the one value; whitespace around it is JSON.
        bool append_arguments(std::string& out, std::string_view raw)
        {
            const size_t at = out.size();
            json::unescape_append(out, raw);
            if (out.size() == at) { out += "{}"; return true; }
            bool ok = false;
            const json::Value input = json::parse(std::string_view(out).substr(at), ok);
            return ok && input.is_object();
        }

        // Every system and developer message, hoisted to the top-level `system` and
        // joined by a newline. Only the block form carries a breakpoint.
        bool append_system(std::string& out, const json::Value& msgs)
        {
            std::string_view cache;
            bool any = false;
            for (const auto& m : msgs.arr)
            {
                if (!detail::is_system_role(m.str_or("role"))) continue;
                any = true;
                if (const json::Value* c = m.find("content"); c && c->is_array())
                    for (const auto& part : c->arr)
                    {
                        std::string_view cc;
                        if (!part_cache_control(part, cc)) return false;
                        if (!cc.empty()) cache = cc;
                    }
            }
            if (!any) return true;
            out += cache.empty() ? ",\"system\":\"" : ",\"system\":[{\"type\":\"text\",\"text\":\"";
            bool first = true;
            for (const auto& m : msgs.arr)
            {
                if (!detail::is_system_role(m.str_or("role"))) continue;
                if (!first) out += "\\n"; // escaped newline in the output
                first = false;
                if (!append_text(out, m.find("content"))) return false;
            }
            out += '"';
            if (!cache.empty())
            {
                out += ",\"cache_control\":";
                out.append(cache);
                out += "}]";
            }
            return true;
        }

        // The turns, for walk_messages. System messages were hoisted by append_system.
        struct AnthropicTurns
        {
            std::string& out;
            bool first = true;
            bool in_results = false; // merging consecutive OpenAI tool messages

            void open(std::string_view head)
            {
                close_results();
                if (!first) out += ',';
                first = false;
                out += head;
            }
            void close_results()
            {
                if (in_results) out += "]}";
                in_results = false;
            }

            bool system(const json::Value*) { return true; }

            bool turn(bool assistant, const json::Value* content)
            {
                open(assistant ? R"({"role":"assistant","content":)" : R"({"role":"user","content":)");
                if (!append_content(out, content)) return false;
                out += '}';
                return true;
            }

            bool tool_result(const json::Value& m, const json::Value* content)
            {
                if (in_results) out += ',';
                else open(R"({"role":"user","content":[)");
                in_results = true;
                out += R"({"type":"tool_result","tool_use_id":)";
                json::append_raw_string(out, m.str_or("tool_call_id"));
                out += R"(,"content":")";
                if (!append_text(out, content)) return false;
                out += "\"}";
                return true;
            }

            // Optional text, then one tool_use block per call.
            bool tool_calls(const json::Value* content, const json::Value& calls)
            {
                open(R"({"role":"assistant","content":[)");
                const size_t mark = out.size();
                out += R"({"type":"text","text":")";
                const size_t text = out.size();
                if (!append_text(out, content)) return false;
                bool any = out.size() != text;
                if (any) out += "\"}";
                else out.resize(mark);
                for (const auto& call : calls.arr)
                {
                    const json::Value* fn = call.find("function");
                    if (!fn) continue;
                    const std::string_view name = fn->str_or("name");
                    if (name.empty()) continue; // unusable; drop instead of guess
                    if (any) out += ',';
                    any = true;
                    out += R"({"type":"tool_use","id":)";
                    json::append_raw_string(out, call.str_or("id"));
                    out += ",\"name\":";
                    json::append_raw_string(out, name);
                    out += ",\"input\":";
                    if (!append_arguments(out, fn->str_or("arguments"))) return false;
                    out += '}';
                }
                out += "]}";
                return true;
            }
        };

        // Anthropic content blocks -> OpenAI tool_calls[]. Empty if none.
        // `,"tool_calls":[...]` for the tool_use blocks; nothing when there are none.
        void append_tool_calls(std::string& out, const json::Value& content)
        {
            bool first = true;
            for (const auto& blk : content.arr)
            {
                if (blk.str_or("type") != "tool_use") continue;
                out += first ? ",\"tool_calls\":[" : ",";
                first = false;
                out += "{\"id\":";
                json::append_raw_string(out, blk.str_or("id"));
                out += ",\"type\":\"function\",\"function\":{\"name\":";
                json::append_raw_string(out, blk.str_or("name"));
                // input (object) -> arguments (string containing that JSON).
                out += ",\"arguments\":";
                const json::Value* in = blk.find("input");
                json::append_escaped(out, (in && !in->sv.empty()) ? in->sv : std::string_view{"{}"});
                out += "}}";
            }
            if (!first) out += ']';
        }

        // ── Anthropic Messages ──────────────────────────────────────────────

        /// Both Messages bodies, because they differ in exactly two fields and a second
        /// copy of the message walk would be a second place for tool results, vision and
        /// system-prompt handling to drift. Bedrock puts the model in the path, so its
        /// body must not carry one, and it wants `anthropic_version` in the JSON where
        /// Anthropic wants it in a header.
        ///
        /// Written front to back into `out`, which keeps its capacity across agent
        /// turns. A message the walk refuses refuses the request whole.
        bool messages_request(std::string_view openai_body, bool bedrock, std::string* model_out,
                              bool* wants_stream_usage, std::string& out)
        {
            out.clear();
            bool ok = false;
            const json::Value v = json::parse(openai_body, ok);
            if (!ok || !v.is_object()) return false;

            if (wants_stream_usage)
            {
                const json::Value* so = v.find("stream_options");
                const json::Value* iu = so && so->is_object() ? so->find("include_usage") : nullptr;
                *wants_stream_usage = iu && iu->type == json::Value::Type::Bool && iu->boolean;
            }

            // A body carrying no `messages` array is not a chat request. Refuse instead of guessing.
            const json::Value* msgs = v.find("messages");
            const std::string_view model = v.str_or("model", "claude-3-5-sonnet-latest");
            if (!msgs || !msgs->is_array()) return false;
            {
                const size_t need = openai_body.size() + 65536;
                if (out.capacity() < need) out.reserve(need + need / 8);
            }
            if (bedrock)
            {
                // Not a version we choose: Bedrock rejects a Messages body without it,
                // and this literal is the only value its Anthropic models accept.
                out = "{\"anthropic_version\":\"bedrock-2023-05-31\"";
            }
            else
            {
                out = "{\"model\":";
                json::append_raw_string(out, model);
            }
            // Anthropic requires max_tokens; default if the OpenAI request omitted it.
            out += ",\"max_tokens\":";
            out += v.num_or("max_tokens", "1024");
            ok = append_system(out, *msgs);
            if (const std::string_view t = v.num_or("temperature"); !t.empty())
            {
                out += ",\"temperature\":";
                out += t;
            }
            if (const std::string_view p = v.num_or("top_p"); !p.empty())
            {
                out += ",\"top_p\":";
                out += p;
            }
            // Anthropic uses the same `stream` flag, so an OpenAI `stream:true` request
            // becomes an Anthropic SSE response.
            if (const json::Value* s = v.find("stream"); s && s->type == json::Value::Type::Bool && s->boolean)
                out += ",\"stream\":true";
            append_tools(out, v);
            out += ",\"messages\":[";
            AnthropicTurns turns{out};
            if (!ok || !detail::walk_messages(*msgs, turns)) { out.clear(); return false; }
            turns.close_results();
            out += "]}";
            if (model_out) model_out->assign(model);
            return true;
        }
    } // namespace

    bool openai_to_anthropic_request(std::string_view openai_body, std::string& out,
                                     bool* wants_stream_usage)
    {
        return messages_request(openai_body, false, nullptr, wants_stream_usage, out);
    }

    std::string openai_to_anthropic_request(std::string_view openai_body,
                                            bool* wants_stream_usage)
    {
        std::string out;
        messages_request(openai_body, false, nullptr, wants_stream_usage, out);
        return out;
    }

    bool openai_to_bedrock_request(std::string_view openai_body, std::string& model_out,
                                   std::string& out)
    {
        return messages_request(openai_body, true, &model_out, nullptr, out);
    }

    std::string openai_to_bedrock_request(std::string_view openai_body,
                                          std::string& model_out)
    {
        std::string out;
        messages_request(openai_body, true, &model_out, nullptr, out);
        return out;
    }

    bool anthropic_to_openai_response(std::string_view anthropic_body, std::string& out,
                                      openai::Usage& usage)
    {
        out.clear();
        usage = {};
        bool ok = false;
        json::Value v = json::parse(anthropic_body, ok);
        if (!ok || !v.is_object()) return false;

        if (const json::Value* m = v.find("usage"))
        {
            const auto n = [](const json::Value* o, std::string_view k) {
                return o ? json_scan::count(o->num_or(k)) : -1;
            };
            const json::Value* cc = m->find("cache_creation");
            usage = detail::anthropic_usage(
                n(m, "input_tokens"), n(m, "output_tokens"), n(m, "cache_read_input_tokens"),
                n(m, "cache_creation_input_tokens"), n(cc, "ephemeral_5m_input_tokens"),
                n(cc, "ephemeral_1h_input_tokens"));
        }
        detail::as_written(usage);

        if (out.capacity() < anthropic_body.size() + 256) out.reserve(anthropic_body.size() + 256);
        char created[24];
        openai::Envelope e(out, openai::Shape::Completion, v.str_or("id", "chatcmpl-llmbridge"),
                           openai::decimal(created, detail::now_secs()), v.str_or("model"));
        e.choice();
        out += "\"role\":\"assistant\",\"content\":";
        // Text blocks verbatim. OpenAI sets content to null, not "", on a pure tool call,
        // and SDKs branch on that.
        const json::Value* c = v.find("content");
        bool text = false, tools = false;
        if (c && c->is_array())
            for (const auto& blk : c->arr)
            {
                const std::string_view t = blk.str_or("type");
                text = text || (t == "text" && !blk.str_or("text").empty());
                tools = tools || t == "tool_use";
            }
        if (!text && tools) out += "null";
        else
        {
            out += '"';
            if (c && c->is_array())
                for (const auto& blk : c->arr)
                    if (blk.str_or("type") == "text") out += blk.str_or("text");
            out += '"';
        }
        if (tools) append_tool_calls(out, *c);
        e.end_choice(detail::anthropic_finish_reason(v.str_or("stop_reason", "end_turn")))
            .close(&usage);
        return true;
    }

    std::string anthropic_to_openai_response(std::string_view anthropic_body)
    {
        std::string out;
        openai::Usage u;
        anthropic_to_openai_response(anthropic_body, out, u);
        return out;
    }
} // namespace llmbridge::provider
