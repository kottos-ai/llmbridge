// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "provider/openai.hpp"

#include <charconv>
#include <string_view>

#include "json_scan.hpp"
#include "openai_common.hpp"

namespace llmbridge::provider::openai
{
    namespace
    {
        using namespace json_scan;

        // Counts under the names each venue gives them, before any mapping.
        struct Raw
        {
            long long prompt = -1, completion = -1, cached = -1, cache_write = -1;
            long long audio_in = -1, audio_out = -1, reasoning = -1, accepted = -1, rejected = -1;
            long long input = -1, output = -1, cache_read = -1, cache_creation = -1;
            long long w5m = -1, w1h = -1;
            long long g_prompt = -1, g_candidates = -1, g_thoughts = -1, g_cached = -1, g_tool = -1;
        };

        struct Key
        {
            std::string_view parent, name; // parent empty: a direct member
            long long Raw::*field;
        };

        // `"usage"`: OpenAI chat, the Responses API and Anthropic share the key.
        constexpr Key kUsageKeys[] = {
            {"", "prompt_tokens", &Raw::prompt},
            {"", "completion_tokens", &Raw::completion},
            {"prompt_tokens_details", "cached_tokens", &Raw::cached},
            {"prompt_tokens_details", "cache_write_tokens", &Raw::cache_write},
            {"prompt_tokens_details", "audio_tokens", &Raw::audio_in},
            {"completion_tokens_details", "audio_tokens", &Raw::audio_out},
            {"completion_tokens_details", "reasoning_tokens", &Raw::reasoning},
            {"completion_tokens_details", "accepted_prediction_tokens", &Raw::accepted},
            {"completion_tokens_details", "rejected_prediction_tokens", &Raw::rejected},
            {"", "input_tokens", &Raw::input},
            {"", "output_tokens", &Raw::output},
            {"input_tokens_details", "cached_tokens", &Raw::cached},
            {"", "cache_read_input_tokens", &Raw::cache_read},
            {"", "cache_creation_input_tokens", &Raw::cache_creation},
            {"cache_creation", "ephemeral_5m_input_tokens", &Raw::w5m},
            {"cache_creation", "ephemeral_1h_input_tokens", &Raw::w1h},
        };
        // `"usageMetadata"`: Gemini.
        constexpr Key kGeminiKeys[] = {
            {"", "promptTokenCount", &Raw::g_prompt},
            {"", "candidatesTokenCount", &Raw::g_candidates},
            {"", "thoughtsTokenCount", &Raw::g_thoughts},
            {"", "cachedContentTokenCount", &Raw::g_cached},
            {"", "toolUsePromptTokenCount", &Raw::g_tool},
        };

        template <size_t N>
        void read_members(std::string_view obj, std::string_view parent, const Key (&keys)[N],
                          Raw& r) noexcept
        {
            (void)for_each_member(obj, [&](std::string_view k, std::string_view v) {
                if (!v.empty() && v.front() == '{')
                {
                    if (parent.empty()) read_members(v, k, keys, r);
                    return true;
                }
                for (const Key& key : keys)
                    if (key.name == k && key.parent == parent)
                    {
                        r.*key.field = count(v);
                        break;
                    }
                return true;
            });
        }

        Usage from_usage(const Raw& r) noexcept
        {
            Usage u;
            if (r.prompt >= 0 || r.completion >= 0) // OpenAI chat completions
            {
                u.in = r.prompt;
                u.out = r.completion;
                u.cached = r.cached;
                if (r.cache_write > 0) u.cache_write = r.cache_write;
                u.audio_in = r.audio_in;
                u.audio_out = r.audio_out;
                u.reasoning = r.reasoning;
                u.accepted_prediction = r.accepted;
                u.rejected_prediction = r.rejected;
                return u;
            }
            // The Responses API names its totals as Anthropic does, with neither cache
            // field, and nests its cache read; a bare output count is message_delta's.
            if (r.input < 0 || (r.cache_read < 0 && r.cache_creation < 0 &&
                                (r.cached >= 0 || r.cache_write > 0)))
            {
                u.in = r.input;
                u.out = r.output;
                u.cached = r.input >= 0 && r.cached < 0 ? 0 : r.cached;
                if (r.cache_write > 0) u.cache_write = r.cache_write;
                return u;
            }
            return detail::anthropic_usage(r.input, r.output, r.cache_read, r.cache_creation,
                                           r.w5m, r.w1h);
        }

        Usage read_usage(std::string_view obj, bool gemini) noexcept
        {
            Raw r;
            if (gemini)
            {
                read_members(obj, {}, kGeminiKeys, r);
                return detail::gemini_usage(r.g_prompt, r.g_candidates, r.g_thoughts,
                                            r.g_cached, r.g_tool);
            }
            read_members(obj, {}, kUsageKeys, r);
            return from_usage(r);
        }

        constexpr std::string_view kUsage = "\"usage\"";
        constexpr std::string_view kMetadata = "\"usageMetadata\"";

        // Each `"usage` in `s` from `from` on, the two keys in one search.
        size_t next_usage(std::string_view s, size_t from) noexcept
        {
            if (from >= s.size()) return kNpos;
            const void* p = ::memmem(s.data() + from, s.size() - from, "\"usage", 6);
            return p ? static_cast<size_t>(static_cast<const char*>(p) - s.data()) : kNpos;
        }
    } // namespace

    namespace
    {
        void append_count(std::string& out, long long v)
        {
            char buf[24];
            const auto r = std::to_chars(buf, buf + sizeof buf, v < 0 ? 0 : v);
            out.append(buf, static_cast<size_t>(r.ptr - buf));
        }
    } // namespace

    std::string_view decimal(char (&buf)[24], long long v) noexcept
    {
        const auto r = std::to_chars(buf, buf + sizeof buf, v);
        return {buf, static_cast<size_t>(r.ptr - buf)};
    }

    void Envelope::close(const Usage* u, bool usage_null, long long total)
    {
        _out += ']';
        if (!u)
        {
            _out += usage_null ? ",\"usage\":null}" : "}";
            return;
        }
        const long long in = u->in < 0 ? 0 : u->in, out = u->out < 0 ? 0 : u->out;
        _out += ",\"usage\":{\"prompt_tokens\":";
        append_count(_out, in);
        _out += ",\"completion_tokens\":";
        append_count(_out, out);
        _out += ",\"total_tokens\":";
        append_count(_out, total < 0 ? in + out : total);
        if (u->cached > 0)
        {
            _out += ",\"prompt_tokens_details\":{\"cached_tokens\":";
            append_count(_out, u->cached);
            _out += '}';
        }
        if (u->reasoning >= 0)
        {
            _out += ",\"completion_tokens_details\":{\"reasoning_tokens\":";
            append_count(_out, u->reasoning);
            _out += '}';
        }
        _out += "}}";
    }

    void write_error(std::string& out, std::string_view message, std::string_view type)
    {
        out += "{\"error\":{\"message\":\"";
        detail::append_sanitized(out, message);
        out += "\",\"type\":\"";
        detail::append_sanitized(out, type);
        out += "\",\"code\":null}}";
    }

    Usage scan_usage(std::string_view body, size_t window) noexcept
    {
        const std::string_view tail =
            (window && body.size() > window) ? body.substr(body.size() - window) : body;
        // The last complete object under either key: a model's own nested objects,
        // tool input among them, come before the usage block in every venue's reply.
        std::string_view last;
        bool gemini = false;
        for (size_t at = next_usage(tail, 0); at != kNpos; at = next_usage(tail, at + 1))
        {
            const std::string_view rest = tail.substr(at);
            const bool g = rest.substr(0, kMetadata.size()) == kMetadata;
            if (!g && rest.substr(0, kUsage.size()) != kUsage) continue;
            const size_t len = g ? kMetadata.size() : kUsage.size();
            const size_t v = key_value_at(tail, at, len);
            if (v == kNpos || tail[v] != '{') continue;
            const size_t e = skip_value(tail, v);
            if (e == kNpos) continue;
            last = tail.substr(v, e - v);
            gemini = g;
            at = e - 1;
        }
        return last.empty() ? Usage{} : read_usage(last, gemini);
    }

    std::string_view scan_string(std::string_view bytes, std::string_view quoted,
                                 bool last) noexcept
    {
        std::string_view out;
        each_key(bytes, quoted, [&](size_t v) {
            const size_t e = skip_value(bytes, v);
            out = e == kNpos ? std::string_view{} : plain_string(bytes.substr(v, e - v));
            return last;
        });
        return out;
    }

    std::string_view scan_error_type(std::string_view body) noexcept
    {
        constexpr size_t kErrorWindow = 512; // `message`, the long field, comes after these
        const std::string_view head = body.substr(0, kErrorWindow);
        std::string_view code, type;
        each_key(head, "\"error\"", [&](size_t v) {
            if (head[v] != '{') return true;
            (void)for_each_member(head.substr(v), [&](std::string_view k, std::string_view raw) {
                if (k == "code") code = plain_string(raw);
                else if (k == "type") type = plain_string(raw);
                return true;
            });
            return false;
        });
        return code.empty() ? type : code;
    }

    void StreamUsage::merge(const Usage& u) noexcept
    {
        for (auto f : {&Usage::in, &Usage::cached, &Usage::cache_write, &Usage::cache_write_5m,
                       &Usage::cache_write_1h})
            if (_u.*f < 0) _u.*f = u.*f;
        for (auto f : {&Usage::out, &Usage::reasoning, &Usage::audio_in, &Usage::audio_out,
                       &Usage::accepted_prediction, &Usage::rejected_prediction,
                       &Usage::tool_prompt})
            if (u.*f >= 0) _u.*f = u.*f;
    }

    void StreamUsage::feed(std::string_view bytes)
    {
        // Bytes kept ahead of the next search: the `,` or `{` before a key split
        // across reads, with the indentation of a pretty-printed body.
        constexpr size_t kLead = 64;
        constexpr size_t kMaxObject = 64 * 1024; // an unclosed usage object past this is dropped
        _carry.append(bytes);
        const std::string_view s = _carry;
        size_t next = s.size() >= kUsage.size() ? s.size() - (kUsage.size() - 1) : 0;
        size_t keep = kNpos;
        for (size_t from = _seen; from < s.size();)
        {
            const size_t at = next_usage(s, from);
            if (at == kNpos) break;
            from = at + 1;
            const std::string_view rest = s.substr(at);
            const bool gemini = rest.substr(0, kMetadata.size()) == kMetadata;
            const size_t len = gemini ? kMetadata.size() : kUsage.size();
            const bool partial = rest.size() < kMetadata.size() &&
                                 (kMetadata.substr(0, rest.size()) == rest ||
                                  kUsage.substr(0, rest.size()) == rest);
            if (partial) { keep = at; next = at; break; }
            if (!gemini && rest.substr(0, len) != kUsage) continue;
            size_t v = skip_ws(s, at + len);
            if (v < s.size() && s[v] == ':') v = skip_ws(s, v + 1);
            if (v >= s.size()) { keep = at; next = at; break; }
            if (s[v] != '{' || key_value_at(s, at, len) != v) continue;
            const size_t e = skip_value(s, v);
            if (e == kNpos)
            {
                if (s.size() - at > kMaxObject) continue;
                keep = at; next = at; break;
            }
            merge(read_usage(s.substr(v, e - v), gemini));
            from = e;
            if (next < e) next = e;
        }
        if (keep == kNpos) keep = next;
        keep = keep > kLead ? keep - kLead : 0;
        _carry.erase(0, keep);
        _seen = next - keep;
    }
} // namespace llmbridge::provider::openai
