// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Edits and reads on an OpenAI body that do not depend on the venue: the top-level
// walk behind model_of, the model and service-tier rewrites, and the error
// envelope for whatever an upstream sent back.

#include "provider/translate.hpp"

#include <string>
#include <string_view>

#include "openai_common.hpp" // detail::append_sanitized
#include "provider/json.hpp"

namespace llmbridge::provider
{
    namespace
    {
        // Past one JSON value, without building it. npos on malformed input, which
        // the caller reads as "no model": a body we cannot walk is one whose
        // top-level keys we do not know.
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

    std::string upsert_string(std::string_view openai_body, std::string_view key,
                              std::string_view value, std::string_view* had)
    {
        if (had) *had = {};
        if (key.empty() || value.empty()) return {};
        // Both halves land inside quotes in a request body, so both are held to the
        // charset rewrite_model uses. A tier or a key needing an escape is a caller
        // speaking a different schema, not something to guess at.
        for (const std::string_view s : {key, value})
            for (const char ch : s)
            {
                const auto u = static_cast<unsigned char>(ch);
                if (u < 0x20 || u == 0x7F || ch == '"' || ch == '\\') return {};
            }
        bool ok = false;
        const json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};

        const char* base = openai_body.data();
        const auto in_body = [&](std::string_view sv) {
            return sv.data() >= base && sv.data() + sv.size() <= base + openai_body.size();
        };

        if (const json::Value* cur = v.find(key))
        {
            // Present and not a string: refuse. See the header.
            if (!cur->is_string() || !in_body(cur->sv)) return {};
            if (had) *had = cur->sv;
            const size_t at = static_cast<size_t>(cur->sv.data() - base);
            std::string out;
            out.reserve(openai_body.size() + value.size());
            out.append(openai_body.substr(0, at));
            out.append(value);
            out.append(openai_body.substr(at + cur->sv.size()));
            return out;
        }

        // Absent: splice it in right after the object's opening brace, which the
        // parser's own span gives us. Inserting at the front means no scan for
        // the matching close.
        if (!in_body(v.sv) || v.sv.empty() || v.sv.front() != '{') return {};
        const size_t open = static_cast<size_t>(v.sv.data() - base) + 1;
        // An empty object takes no comma; anything else does. Whitespace between the
        // brace and the first member is legal and stays where it is.
        size_t probe = open;
        while (probe < openai_body.size() &&
               (openai_body[probe] == ' ' || openai_body[probe] == '\t' ||
                openai_body[probe] == '\n' || openai_body[probe] == '\r'))
            ++probe;
        const bool empty_object = probe < openai_body.size() && openai_body[probe] == '}';

        std::string out;
        out.reserve(openai_body.size() + key.size() + value.size() + 6);
        out.append(openai_body.substr(0, open));
        out.push_back('"');
        out.append(key);
        out.append("\":\"");
        out.append(value);
        out.push_back('"');
        if (!empty_object) out.push_back(',');
        out.append(openai_body.substr(open));
        return out;
    }

    std::string apply_overrides(std::string_view openai_body, std::string_view model,
                                std::string_view service_tier, std::string_view* had_tier)
    {
        if (had_tier) *had_tier = {};
        if (model.empty() && service_tier.empty()) return std::string(openai_body);
        for (const std::string_view v : {model, service_tier})
            for (const char ch : v)
            {
                const auto u = static_cast<unsigned char>(ch);
                if (u < 0x20 || u == 0x7F || ch == '"' || ch == '\\') return {};
            }
        bool ok = false;
        const json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return {};

        const char* base = openai_body.data();
        const auto in_body = [&](std::string_view sv) {
            return sv.data() >= base && sv.data() + sv.size() <= base + openai_body.size();
        };

        // Each edit as (offset, length-to-drop, text). Collected first, applied in
        // ascending offset, so one pass over the body emits the result.
        struct Edit { size_t at; size_t drop; std::string text; };
        Edit edits[2];
        size_t n = 0;

        if (!model.empty())
        {
            const json::Value* m = v.find("model");
            if (!m || !m->is_string() || !in_body(m->sv)) return {};
            edits[n++] = {static_cast<size_t>(m->sv.data() - base), m->sv.size(),
                          std::string(model)};
        }
        if (!service_tier.empty())
        {
            if (const json::Value* t = v.find("service_tier"))
            {
                if (!t->is_string() || !in_body(t->sv)) return {};
                if (had_tier) *had_tier = t->sv;
                edits[n++] = {static_cast<size_t>(t->sv.data() - base), t->sv.size(),
                              std::string(service_tier)};
            }
            else
            {
                if (!in_body(v.sv) || v.sv.empty() || v.sv.front() != '{') return {};
                const size_t open = static_cast<size_t>(v.sv.data() - base) + 1;
                size_t probe = open;
                while (probe < openai_body.size() &&
                       (openai_body[probe] == ' ' || openai_body[probe] == '\t' ||
                        openai_body[probe] == '\n' || openai_body[probe] == '\r'))
                    ++probe;
                const bool empty_object =
                    probe < openai_body.size() && openai_body[probe] == '}';
                std::string ins = "\"service_tier\":\"";
                ins += service_tier;
                ins += '"';
                if (!empty_object) ins += ',';
                edits[n++] = {open, 0, std::move(ins)};
            }
        }
        if (n == 2 && edits[0].at > edits[1].at) std::swap(edits[0], edits[1]);

        std::string out;
        out.reserve(openai_body.size() + model.size() + service_tier.size() + 24);
        size_t cursor = 0;
        for (size_t i = 0; i < n; ++i)
        {
            out.append(openai_body.substr(cursor, edits[i].at - cursor));
            out.append(edits[i].text);
            cursor = edits[i].at + edits[i].drop;
        }
        out.append(openai_body.substr(cursor));
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
        // yielding the generic envelope below.
        std::string out = "{\"error\":{\"message\":\"";
        if (message.empty()) out += "upstream provider error";
        else detail::append_sanitized(out, message);
        out += "\",\"type\":\"";
        detail::append_sanitized(out, type);
        out += "\",\"code\":null}}";
        return out;
    }
} // namespace llmbridge::provider
