// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "provider/openai.hpp"

#include <charconv>
#include <cstring>
#include <string_view>

#include "json_scan.hpp"
#include "openai_common.hpp"

namespace llmbridge::provider::openai
{
    namespace
    {
        using namespace json_scan;

        // Counts under the names each venue gives them, before any mapping.
        // One of Anthropic's `usage.iterations` sums, in its own names.
        struct Attempts
        {
            long long fresh = -1, out = -1, read = -1, write = -1, n = 0;
            std::string_view model; // the first entry's

            void add(long long a_fresh, long long a_out, long long a_read, long long a_write,
                     std::string_view a_model) noexcept
            {
                const auto plus = [](long long& sum, long long v) {
                    if (v >= 0) sum = (sum > 0 ? sum : 0) + v;
                };
                plus(fresh, a_fresh);
                plus(out, a_out);
                plus(read, a_read);
                plus(write, a_write);
                if (n++ == 0) model = a_model;
            }
        };

        struct Raw
        {
            long long prompt = -1, completion = -1, cached = -1, cache_write = -1;
            long long audio_in = -1, audio_out = -1, reasoning = -1, accepted = -1, rejected = -1;
            long long image_in = -1;
            long long input = -1, output = -1, cache_read = -1, cache_creation = -1;
            long long w5m = -1, w1h = -1;
            long long web_search = -1, web_fetch = -1, code_exec = -1, cost = -1;
            long long g_prompt = -1, g_candidates = -1, g_thoughts = -1, g_cached = -1, g_tool = -1;
            long long g_total = -1, g_audio_in = -1, g_audio_out = -1;
            std::string_view speed, geo;
            Attempts compaction, declined; // declined: `message` entries, before a fallback
            bool fallback = false;
        };

        struct Key
        {
            std::string_view parent, name; // parent empty: a direct member
            long long Raw::*field;
            long long (*parse)(std::string_view) noexcept = count;
        };

        // `"usage"`: OpenAI chat, the Responses API and Anthropic share the key.
        constexpr Key kUsageKeys[] = {
            {"", "prompt_tokens", &Raw::prompt},
            {"", "completion_tokens", &Raw::completion},
            {"", "cached_tokens", &Raw::cached}, // Together, on some models
            {"prompt_tokens_details", "cached_tokens", &Raw::cached},
            {"prompt_tokens_details", "cache_write_tokens", &Raw::cache_write},
            {"prompt_tokens_details", "audio_tokens", &Raw::audio_in},
            {"prompt_tokens_details", "image_tokens", &Raw::image_in},
            {"completion_tokens_details", "audio_tokens", &Raw::audio_out},
            {"completion_tokens_details", "reasoning_tokens", &Raw::reasoning},
            {"completion_tokens_details", "accepted_prediction_tokens", &Raw::accepted},
            {"completion_tokens_details", "rejected_prediction_tokens", &Raw::rejected},
            {"", "input_tokens", &Raw::input},
            {"", "output_tokens", &Raw::output},
            {"input_tokens_details", "cached_tokens", &Raw::cached},
            {"input_tokens_details", "cache_write_tokens", &Raw::cache_write}, // Bedrock Mantle
            {"output_tokens_details", "reasoning_tokens", &Raw::reasoning}, // Responses
            {"output_tokens_details", "thinking_tokens", &Raw::reasoning},  // Anthropic
            {"", "cache_read_input_tokens", &Raw::cache_read},
            {"", "cache_creation_input_tokens", &Raw::cache_creation},
            {"cache_creation", "ephemeral_5m_input_tokens", &Raw::w5m},
            {"cache_creation", "ephemeral_1h_input_tokens", &Raw::w1h},
            {"server_tool_use", "web_search_requests", &Raw::web_search},
            {"server_tool_use", "web_fetch_requests", &Raw::web_fetch},
            {"server_tool_use", "code_execution_requests", &Raw::code_exec},
            {"", "estimated_cost", &Raw::cost, picodollars}, // DeepInfra
        };
        // `"usageMetadata"`: Gemini.
        constexpr Key kGeminiKeys[] = {
            {"", "promptTokenCount", &Raw::g_prompt},
            {"", "candidatesTokenCount", &Raw::g_candidates},
            {"", "thoughtsTokenCount", &Raw::g_thoughts},
            {"", "cachedContentTokenCount", &Raw::g_cached},
            {"", "toolUsePromptTokenCount", &Raw::g_tool},
            {"", "totalTokenCount", &Raw::g_total},
        };

        // The AUDIO entry of a Gemini `*TokensDetails` array, `[{"modality":"AUDIO",
        // "tokenCount":N},...]`; -1 when it names none.
        long long modality_count(std::string_view arr, std::string_view modality) noexcept
        {
            long long n = -1;
            (void)for_each_element(arr, [&](std::string_view e) {
                std::string_view m;
                long long c = -1;
                (void)for_each_member(e, [&](std::string_view k, std::string_view v) {
                    if (k == "modality") m = plain_string(v);
                    else if (k == "tokenCount") c = count(v);
                    return true;
                });
                if (m == modality && c >= 0) n = (n > 0 ? n : 0) + c;
                return true;
            });
            return n;
        }

        // Each entry of `usage.iterations`: a compaction pass, a declined attempt, or
        // the `fallback_message` attempt the top-level counts describe.
        void read_iterations(std::string_view arr, Raw& r) noexcept
        {
            (void)for_each_element(arr, [&](std::string_view e) {
                std::string_view type, model;
                long long fresh = -1, out = -1, read = -1, write = -1;
                (void)for_each_member(e, [&](std::string_view k, std::string_view v) {
                    if (k == "type") type = plain_string(v);
                    else if (k == "model") model = plain_string(v);
                    else if (k == "input_tokens") fresh = count(v);
                    else if (k == "output_tokens") out = count(v);
                    else if (k == "cache_read_input_tokens") read = count(v);
                    else if (k == "cache_creation_input_tokens") write = count(v);
                    return true;
                });
                if (type == "compaction") r.compaction.add(fresh, out, read, write, model);
                else if (type == "message") r.declined.add(fresh, out, read, write, model);
                else if (type == "fallback_message") r.fallback = true;
                return true;
            });
        }

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
                if (parent.empty() && !v.empty())
                {
                    if (v.front() == '[')
                    {
                        if (k == "iterations") read_iterations(v, r);
                        else if (k == "promptTokensDetails") r.g_audio_in = modality_count(v, "AUDIO");
                        else if (k == "candidatesTokensDetails") r.g_audio_out = modality_count(v, "AUDIO");
                        return true;
                    }
                    if (v.front() == '"')
                    {
                        if (k == "speed") r.speed = plain_string(v);
                        else if (k == "inference_geo") r.geo = plain_string(v);
                        return true;
                    }
                }
                for (const Key& key : keys)
                    if (key.name == k && key.parent == parent)
                    {
                        r.*key.field = key.parse(v);
                        break;
                    }
                return true;
            });
        }

        // What every shape may carry beside its token counts.
        void extras(const Raw& r, Usage& u) noexcept
        {
            u.image_in = r.image_in;
            u.web_search = r.web_search;
            u.web_fetch = r.web_fetch;
            u.code_exec = r.code_exec;
            u.venue_cost_pdollars = r.cost;
            u.speed.set(r.speed);
            u.inference_geo.set(r.geo);
            if (const Attempts& c = r.compaction; c.n)
            {
                const Usage t = detail::anthropic_usage(c.fresh, c.out, c.read, c.write, -1, -1);
                u.compaction_in = t.in;
                u.compaction_out = t.out > 0 ? t.out : 0;
                u.compaction_cached = t.cached;
            }
            // Without a fallback, `message` entries are the served model's own and the
            // top-level counts already hold them.
            if (const Attempts& d = r.declined; d.n && r.fallback)
            {
                const Usage t = detail::anthropic_usage(d.fresh, d.out, d.read, d.write, -1, -1);
                u.prior_in = t.in;
                u.prior_out = t.out > 0 ? t.out : 0;
                u.prior_cached = t.cached;
                u.prior_cache_write = t.cache_write;
                u.prior_attempts = d.n;
                u.prior_model.set(d.model);
            }
        }

        Usage counts(const Raw& r) noexcept
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
                u.reasoning = r.reasoning;
                return u;
            }
            u = detail::anthropic_usage(r.input, r.output, r.cache_read, r.cache_creation, r.w5m,
                                        r.w1h);
            u.reasoning = r.reasoning; // part of output_tokens, as on OpenAI
            return u;
        }

        Usage from_usage(const Raw& r) noexcept
        {
            Usage u = counts(r);
            extras(r, u);
            return u;
        }

        Usage read_usage(std::string_view obj, bool gemini) noexcept
        {
            Raw r;
            if (gemini)
            {
                read_members(obj, {}, kGeminiKeys, r);
                Usage u = detail::gemini_usage(r.g_prompt, r.g_candidates, r.g_thoughts, r.g_cached, r.g_tool,
                                               r.g_total);
                u.audio_in = r.g_audio_in;
                u.audio_out = r.g_audio_out;
                return u;
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
        // Google's name is `status`, after its `message`: read only when the window named none.
        constexpr size_t kStatusWindow = 8192;
        const auto read = [](std::string_view head, bool status) {
            std::string_view code, type;
            each_key(head, "\"error\"", [&](size_t v) {
                if (head[v] != '{') return true;
                (void)for_each_member(head.substr(v), [&](std::string_view k, std::string_view raw) {
                    if (k == "code") code = plain_string(raw); // Google's is a number: none
                    else if (k == (status ? "status" : "type")) type = plain_string(raw);
                    return true;
                });
                return false;
            });
            return code.empty() ? type : code;
        };
        const std::string_view named = read(body.substr(0, kErrorWindow), false);
        return named.empty() && body.size() > 0 ? read(body.substr(0, kStatusWindow), true) : named;
    }

    Usage usage_of(std::string_view object) noexcept { return read_usage(object, false); }

    void merge_stated(Usage& into, const Usage& u) noexcept
    {
        // The prompt counts only grow: Anthropic restates them cumulatively on
        // message_delta once server tools have run, and a restatement that leaves a
        // count out reads as zero there, so the larger one is the true one.
        for (auto f : {&Usage::in, &Usage::cached, &Usage::cache_write, &Usage::image_in})
            if (u.*f > into.*f) into.*f = u.*f;
        for (auto f : {&Usage::cache_write_5m, &Usage::cache_write_1h}) // message_start's only
            if (into.*f < 0) into.*f = u.*f;
        for (auto f : {&Usage::speed, &Usage::inference_geo})
            if (!(into.*f).n) into.*f = u.*f;
        for (auto f : {&Usage::out, &Usage::reasoning, &Usage::audio_in, &Usage::audio_out,
                       &Usage::accepted_prediction, &Usage::rejected_prediction,
                       &Usage::tool_prompt, &Usage::web_search, &Usage::web_fetch,
                       &Usage::code_exec, &Usage::venue_cost_pdollars, &Usage::compaction_in,
                       &Usage::compaction_out, &Usage::compaction_cached})
            if (u.*f >= 0) into.*f = u.*f;
        if (u.prior_attempts >= 0) // the final message_delta's iterations, all of them
        {
            into.prior_in = u.prior_in;
            into.prior_out = u.prior_out;
            into.prior_cached = u.prior_cached;
            into.prior_cache_write = u.prior_cache_write;
            into.prior_attempts = u.prior_attempts;
            into.prior_model = u.prior_model;
        }
    }

    namespace
    {
        // An item's type names the call, `"web_search_call"`; an event's names it
        // `"response.web_search_call.completed"`. A quote inside prompt text is escaped,
        // so the quote before either name cannot be forged.
        constexpr std::string_view kItem = "_search_call\"";
        constexpr std::string_view kEvent = "_search_call.completed\"";

        // f(begin, end, web) for each call named in `bytes`, one search for both tools.
        template <class F>
        void each_search_call(std::string_view bytes, bool events, F&& f) noexcept
        {
            const std::string_view lit = events ? kEvent : kItem;
            const std::string_view web = events ? "\"response.web" : "\"web";
            const std::string_view file = events ? "\"response.file" : "\"file";
            for (size_t from = 0; from < bytes.size();)
            {
                const void* p = ::memmem(bytes.data() + from, bytes.size() - from, lit.data(), lit.size());
                if (!p) return;
                const auto at = static_cast<size_t>(static_cast<const char*>(p) - bytes.data());
                from = at + 1;
                const std::string_view head = bytes.substr(0, at);
                if (head.ends_with(web)) f(at - web.size(), at + lit.size(), true);
                else if (head.ends_with(file)) f(at - file.size(), at + lit.size(), false);
            }
        }

        void add_call(Usage& u, bool web) noexcept
        {
            long long& n = web ? u.web_search : u.file_search;
            n = (n > 0 ? n : 0) + 1;
        }
    } // namespace

    void count_search_calls(std::string_view bytes, bool events, Usage& u) noexcept
    {
        each_search_call(bytes, events, [&](size_t, size_t, bool web) { add_call(u, web); });
    }

    void StreamUsage::count_calls(std::string_view bytes) noexcept
    {
        count_search_calls(bytes, true, _u);
        // A name split across two reads: only a match that starts in the kept tail and
        // ends in this read, the others being counted once already.
        if (_tail_n)
        {
            char joint[2 * kTail];
            const size_t lead = bytes.size() < kTail ? bytes.size() : kTail;
            std::memcpy(joint, _tail, _tail_n);
            std::memcpy(joint + _tail_n, bytes.data(), lead);
            each_search_call({joint, _tail_n + lead}, true, [&](size_t at, size_t end, bool web) {
                if (at < _tail_n && end > _tail_n) add_call(_u, web);
            });
        }
        if (bytes.size() >= kTail)
        {
            std::memcpy(_tail, bytes.data() + bytes.size() - kTail, kTail);
            _tail_n = kTail;
        }
        else
        {
            const size_t keep = _tail_n + bytes.size() > kTail ? kTail - bytes.size() : _tail_n;
            std::memmove(_tail, _tail + _tail_n - keep, keep);
            std::memcpy(_tail + keep, bytes.data(), bytes.size());
            _tail_n = keep + bytes.size();
        }
    }

    void StreamUsage::feed(std::string_view bytes)
    {
        // Bytes kept ahead of the next search: the `,` or `{` before a key split
        // across reads, with the indentation of a pretty-printed body.
        constexpr size_t kLead = 64;
        constexpr size_t kMaxObject = 64 * 1024; // an unclosed usage object past this is dropped
        if (_calls) count_calls(bytes);
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
            merge_stated(_u, read_usage(s.substr(v, e - v), gemini));
            from = e;
            if (next < e) next = e;
        }
        if (keep == kNpos) keep = next;
        keep = keep > kLead ? keep - kLead : 0;
        _carry.erase(0, keep);
        _seen = next - keep;
    }
} // namespace llmbridge::provider::openai
