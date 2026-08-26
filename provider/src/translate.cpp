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
        // string, or an array of {type:"text",text:...} parts, to `out`, without
        // surrounding quotes. Zero-copy passthrough: the input's escaping is exactly
        // the output's, so nothing is decoded or re-escaped; the bytes are viewed
        // straight out of the request/response buffer.
        // False when the content holds a part this translator cannot carry, which is
        // anything that is not text: an image, audio, a PDF.
        //
        // It used to skip those parts and keep the text, and that is worse than not
        // supporting them. A vision request became "what is in this image?" with no
        // image attached, the provider answered confidently about nothing, the caller
        // was billed, and the status was 200. Dropping part of a request and
        // forwarding the rest is the sanitise-and-forward this codebase refuses
        // everywhere else; the caller has to be told, and the gateway names the part
        // in the error it returns.
        [[nodiscard]] bool append_text(std::string& out, const json::Value* c)
        {
            if (!c) return true;
            if (c->is_string()) { out += c->sv; return true; }
            if (c->is_array())
                for (const auto& part : c->arr)
                {
                    const std::string_view type = part.str_or("type");
                    if (type == "text") { out += part.str_or("text"); continue; }
                    // A part with no type at all is as unusable as an unknown one:
                    // guessing it is text is the same silent edit.
                    return false;
                }
            return true;
        }
    } // namespace


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
                    if (!append_text(system, content)) return {};
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
                messages += ",\"content\":\"";
                if (!append_text(messages, content)) return {};
                messages += "\"}";
            }
            if (in_tool_results) messages += "]}"; // close a trailing tool_result turn
        }
        messages += ']';

        const std::string_view model = v.str_or("model", "claude-3-5-sonnet-latest");
        if (model_out) model_out->assign(model);
        std::string out = "{";
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
    } // namespace

    namespace
    {
        // Past one JSON value, without building it. npos on malformed input, which
        // the caller reads as "no model": a body we cannot walk is one whose
        // top-level keys we do not know.
        //
        // By value shape, because the three cases end differently: a string at its
        // closing quote, a container at depth zero, a scalar at the delimiter after
        // it. One fused loop got the string wrong and it took a test to see.
        size_t skip_value(std::string_view b, size_t i) noexcept
        {
            constexpr size_t kBad = std::string_view::npos;
            if (i >= b.size()) return kBad;
            if (b[i] == '"')
            {
                for (++i; i < b.size(); ++i)
                {
                    if (b[i] == '\\') { ++i; continue; }
                    if (b[i] == '"') return i + 1;
                }
                return kBad;
            }
            if (b[i] == '{' || b[i] == '[')
            {
                int depth = 0;
                bool in_str = false;
                for (; i < b.size(); ++i)
                {
                    const char c = b[i];
                    if (in_str)
                    {
                        if (c == '\\') ++i;
                        else if (c == '"') in_str = false;
                        continue;
                    }
                    if (c == '"') in_str = true;
                    else if (c == '{' || c == '[') ++depth;
                    else if ((c == '}' || c == ']') && --depth == 0) return i + 1;
                }
                return kBad;
            }
            while (i < b.size() && b[i] != ',' && b[i] != '}') ++i; // number, bool, null
            return i;
        }

        size_t skip_ws(std::string_view b, size_t i) noexcept
        {
            while (i < b.size() && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r'))
                ++i;
            return i;
        }
    } // namespace

    namespace
    {
        std::string_view top_level_value(std::string_view body, std::string_view key) noexcept
        {
            size_t i = skip_ws(body, 0);
            if (i >= body.size() || body[i] != '{') return {};
            ++i;
            while (true)
            {
                i = skip_ws(body, i);
                if (i >= body.size() || body[i] != '"') return {}; // '}' included: absent
                const size_t kb = ++i;
                while (i < body.size() && body[i] != '"')
                {
                    if (body[i] == '\\') ++i;
                    ++i;
                }
                if (i >= body.size()) return {};
                const std::string_view k = body.substr(kb, i - kb);
                i = skip_ws(body, i + 1);
                if (i >= body.size() || body[i] != ':') return {};
                i = skip_ws(body, i + 1);
                const size_t vb = i;
                i = skip_value(body, i);
                if (i == std::string_view::npos) return {};
                if (k == key) return body.substr(vb, i - vb);
                i = skip_ws(body, i);
                if (i >= body.size() || body[i] != ',') return {};
                ++i;
            }
        }
    } // namespace

    std::string_view model_of(std::string_view body) noexcept
    {
        const std::string_view v = top_level_value(body, "model");
        // A string, and one we can compare byte for byte against a configured name.
        // An escape means it was never one of those, and unescaping here would need an
        // allocation on a path that has none.
        if (v.size() < 2 || v.front() != '"' || v.back() != '"') return {};
        const std::string_view inner = v.substr(1, v.size() - 2);
        return inner.find('\\') == std::string_view::npos ? inner : std::string_view{};
    }

    bool wants_stream(std::string_view body) noexcept
    {
        // Reject cheaply before walking.
        if (body.find("\"stream\"") == std::string_view::npos) return false;
        return top_level_value(body, "stream") == "true";
    }

    std::string rewrite_model(std::string_view openai_body, std::string_view model)
    {
        if (model.empty()) return {};
        for (const char ch : model)
        {
            const auto u = static_cast<unsigned char>(ch);
            if (u < 0x20 || u == 0x7F || ch == '"' || ch == '\\') return {};
        }
        bool ok = false;
        const json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};
        const json::Value* m = v.find("model");
        if (!m || !m->is_string()) return {};

        // The parser's string view points into the body, so its offsets are the exact
        // span to replace, quotes excluded. No searching, and no chance of hitting a
        // "model" that lives inside a prompt.
        const char* base = openai_body.data();
        if (m->sv.data() < base || m->sv.data() + m->sv.size() > base + openai_body.size())
            return {};
        const size_t at = static_cast<size_t>(m->sv.data() - base);

        std::string out;
        out.reserve(openai_body.size() + model.size());
        out.append(openai_body.substr(0, at));
        out.append(model);
        out.append(openai_body.substr(at + m->sv.size()));
        return out;
    }

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

        // Both spans come from an untrusted upstream body, so sanitize on the way
        // out: the same rule the SSE passthrough follows. Otherwise a provider could
        // make our own error envelope unparseable to a strict client.
        //
        // This is now defence in depth instead of the primary mitigation: the
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
