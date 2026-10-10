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

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "json_scan.hpp"
#include "provider/json.hpp"

namespace llmbridge::provider
{
    namespace
    {
        using json_scan::for_each_member;

        // Keys of one object: none escaped, none repeated, at most kMaxKeys of them.
        // Fed one key at a time, so the single pass can tally while it reads values.
        class KeyTally
        {
          public:
            void add(std::string_view k) noexcept
            {
                if (_verdict != KeyCheck::Ok) return;
                if (k.find('\\') != std::string_view::npos) _verdict = KeyCheck::EscapedKey;
                else if (_n == kMaxKeys) _verdict = KeyCheck::TooManyKeys;
                else _keys[_n++] = k;
            }
            [[nodiscard]] bool settled() const noexcept { return _verdict != KeyCheck::Ok; }
            KeyCheck result() noexcept
            {
                if (_verdict != KeyCheck::Ok) return _verdict;
                std::sort(_keys.begin(), _keys.begin() + static_cast<std::ptrdiff_t>(_n));
                for (size_t j = 1; j < _n; ++j)
                    if (_keys[j] == _keys[j - 1]) return KeyCheck::DuplicateKey;
                return KeyCheck::Ok;
            }

          private:
            static constexpr size_t kMaxKeys = 256;
            std::array<std::string_view, kMaxKeys> _keys;
            size_t _n = 0;
            KeyCheck _verdict = KeyCheck::Ok;
        };

        KeyCheck check_keys(std::string_view obj) noexcept
        {
            KeyTally t;
            (void)for_each_member(obj, [&](std::string_view k, std::string_view) {
                t.add(k);
                return !t.settled();
            });
            return t.result();
        }
    } // namespace

    TopLevelFacts top_level_facts(std::string_view body) noexcept
    {
        // First occurrence of each key, as the single-key readers below report it.
        std::string_view model, stream, options;
        bool have_model = false, have_stream = false, have_options = false;
        KeyTally t;
        (void)for_each_member(body, [&](std::string_view k, std::string_view v) {
            t.add(k);
            if (k == "model" && !have_model) { model = v; have_model = true; }
            else if (k == "stream" && !have_stream) { stream = v; have_stream = true; }
            else if (k == "stream_options" && !have_options) { options = v; have_options = true; }
            return true;
        });
        TopLevelFacts f;
        f.model = json_scan::plain_string(model);
        f.stream = stream == "true";
        f.include_usage = json_scan::first_member(options, "include_usage") == "true";
        f.keys = t.result();
        // stream_options is read one level down, so its keys carry the same risk.
        if (f.keys == KeyCheck::Ok) f.keys = check_keys(options);
        return f;
    }

    std::string_view reply_model(std::string_view json) noexcept
    {
        // Anthropic's first stream event names the model one level down, inside the
        // message it starts; every other reply names it at the top level.
        if (json_scan::first_member(json, "type") == "\"message_start\"")
            return json_scan::plain_string(
                json_scan::first_member(json_scan::first_member(json, "message"), "model"));
        return model_of(json);
    }

    size_t top_level_member_count(std::string_view body) noexcept
    {
        size_t n = 0;
        (void)for_each_member(body, [&](std::string_view, std::string_view) {
            ++n;
            return true;
        });
        return n;
    }

    // Each of these is one field of the single pass, so no reader can disagree with
    // the gateway about the same body; model_of agrees by sharing its reader.
    KeyCheck top_level_key_check(std::string_view body) noexcept
    {
        return top_level_facts(body).keys;
    }

    // The one reader that stops early: a client that puts `model` first pays for the
    // bytes before it and nothing after.
    std::string_view model_of(std::string_view body) noexcept
    {
        return json_scan::plain_string(json_scan::first_member(body, "model"));
    }

    bool wants_stream(std::string_view body) noexcept
    {
        return top_level_facts(body).stream;
    }

    bool stream_usage_of(std::string_view body) noexcept
    {
        return top_level_facts(body).include_usage;
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

    bool apply_overrides(std::string_view openai_body, std::string_view model,
                         std::string_view service_tier, std::string_view* had_tier,
                         std::string& out)
    {
        if (had_tier) *had_tier = {};
        out.clear();
        if (model.empty() && service_tier.empty()) { out.assign(openai_body); return true; }
        for (const std::string_view v : {model, service_tier})
            for (const char ch : v)
            {
                const auto u = static_cast<unsigned char>(ch);
                if (u < 0x20 || u == 0x7F || ch == '"' || ch == '\\') return false;
            }
        bool ok = false;
        const json::Value v = json::parse(openai_body, ok);
        if (!ok || !v.is_object()) return false;

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
            if (!m || !m->is_string() || !in_body(m->sv)) return false;
            edits[n++] = {static_cast<size_t>(m->sv.data() - base), m->sv.size(),
                          std::string(model)};
        }
        if (!service_tier.empty())
        {
            if (const json::Value* t = v.find("service_tier"))
            {
                if (!t->is_string() || !in_body(t->sv)) return false;
                if (had_tier) *had_tier = t->sv;
                edits[n++] = {static_cast<size_t>(t->sv.data() - base), t->sv.size(),
                              std::string(service_tier)};
            }
            else
            {
                if (!in_body(v.sv) || v.sv.empty() || v.sv.front() != '{') return false;
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

        {
            const size_t need = openai_body.size() + model.size() + service_tier.size() + 24;
            if (out.capacity() < need) out.reserve(need + need / 8);
        }
        size_t cursor = 0;
        for (size_t i = 0; i < n; ++i)
        {
            out.append(openai_body.substr(cursor, edits[i].at - cursor));
            out.append(edits[i].text);
            cursor = edits[i].at + edits[i].drop;
        }
        out.append(openai_body.substr(cursor));
        return true;
    }

    std::string apply_overrides(std::string_view openai_body, std::string_view model,
                                std::string_view service_tier, std::string_view* had_tier)
    {
        std::string out;
        apply_overrides(openai_body, model, service_tier, had_tier, out);
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

        // Both spans are an untrusted upstream's; write_error escapes control bytes.
        std::string out;
        openai::write_error(out, message.empty() ? "upstream provider error" : message, type);
        return out;
    }
} // namespace llmbridge::provider
