// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Bounded substring scans over provider bytes: usage counts, an error type, one JSON
// string. Inline because they run per chunk on a stream and per request on the
// tail of a body, and nothing in the build inlines across translation units.

#include <cstddef>
#include <cstring>
#include <string_view>

namespace llmbridge::detail
{
    // Pull `prompt_tokens` / `completion_tokens` out of a translated OpenAI body.
    //
    // Bounded on purpose: `usage` is the last object in the response we build, so
    // this searches only the tail instead of scanning a body that may be many KB
    // of completion text. On no match the headers are simply omitted. The
    // existing rule everywhere in this file is to omit instead of report a
    // number we did not measure.
    struct BodyUsage
    {
        long long in = -1, out = -1, cached = -1, cache_write = -1;
        // The write split by entry lifetime. Priced differently (1.25x the input
        // rate at Anthropic's five minutes, 2x at one hour).
        long long cache_write_5m = -1, cache_write_1h = -1;
    };

    /// Bytes of a response worth searching for a usage block.
    ///
    /// Sized by what must fit, not by feel. A full OpenAI usage chunk carries
    /// prompt/completion/total plus `prompt_tokens_details` and
    /// `completion_tokens_details`, and on a stream it is followed by
    /// `data: [DONE]`, so the block can sit ~600 bytes from the end. 2 KiB clears
    /// that with room for fields providers keep adding, and is still a bounded
    /// tail, never a full-body scan.
    constexpr size_t kUsageWindow = 2048;

    /// The string value of `key` at or after `from`, empty when the key is absent,
    /// its value is null or a number, or the string runs past the end of `head`.
    /// Substring search for the JSON scanners below, backed by memmem.
    [[nodiscard]] inline size_t find_fast(std::string_view hay, std::string_view needle,
                                   size_t from = 0) noexcept
    {
        if (from > hay.size()) return std::string_view::npos;
        if (needle.empty()) return from;
        const void* p = ::memmem(hay.data() + from, hay.size() - from, needle.data(), needle.size());
        return p ? static_cast<size_t>(static_cast<const char*>(p) - hay.data())
                 : std::string_view::npos;
    }

    /// Last occurrence, as repeated forward finds. Hits are rare in this use (a
    /// usage key appears once or twice), so the loop runs once or twice.
    [[nodiscard]] inline size_t rfind_fast(std::string_view hay, std::string_view needle) noexcept
    {
        size_t last = std::string_view::npos;
        for (size_t at = find_fast(hay, needle); at != std::string_view::npos;
             at = find_fast(hay, needle, at + 1))
            last = at;
        return last;
    }

    inline std::string_view json_string_at(std::string_view head, std::string_view key,
                                    size_t from = 0) noexcept
    {
        const size_t k = find_fast(head, key, from);
        if (k == std::string_view::npos) return {};
        size_t i = k + key.size();
        while (i < head.size() && (head[i] == ':' || head[i] == ' ')) ++i;
        if (i >= head.size() || head[i] != '"') return {};
        const size_t b = ++i;
        while (i < head.size() && head[i] != '"' && head[i] != '\\') ++i;
        return i < head.size() && head[i] == '"' ? head.substr(b, i - b)
                                                 : std::string_view{};
    }

    /// Bytes of an error body worth searching. A provider states the type near
    /// the front, and the field that can be long is `message`.
    constexpr size_t kErrorWindow = 512;

    /// What the venue called this failure, taken from its own error body.
    inline std::string_view scan_error_type(std::string_view body) noexcept
    {
        const std::string_view head =
            body.size() > kErrorWindow ? body.substr(0, kErrorWindow) : body;
        const size_t e = find_fast(head, "\"error\"");
        if (e == std::string_view::npos) return {};
        const std::string_view code = json_string_at(head, "\"code\"", e);
        return code.empty() ? json_string_at(head, "\"type\"", e) : code;
    }

    /// `window` 0 = search all of `body`, for a caller that already bounded it.
    /// Passing a second, smaller window there is how the retained bytes and the
    /// searched bytes drift apart and the counts come back -1.
    inline BodyUsage scan_usage(std::string_view body, size_t window = kUsageWindow) noexcept
    {
        BodyUsage u;
        const std::string_view tail =
            (window && body.size() > window) ? body.substr(body.size() - window) : body;
        // `last` matters for one field only, and it is not a preference: an
        // Anthropic stream states `output_tokens` twice, a placeholder 1 in
        // message_start and the real total in message_delta. Taking the first
        // match reports every answer as one token long.
        const auto num_at = [&tail](std::string_view key, bool last) -> long long {
            const size_t k = last ? rfind_fast(tail, key) : find_fast(tail, key);
            if (k == std::string_view::npos) return -1;
            size_t i = k + key.size();
            while (i < tail.size() && (tail[i] == ':' || tail[i] == ' ')) ++i;
            long long v = 0;
            bool any = false;
            for (; i < tail.size() && tail[i] >= '0' && tail[i] <= '9'; ++i)
            {
                v = v * 10 + (tail[i] - '0');
                any = true;
            }
            return any ? v : -1;
        };
        const auto num_after = [&num_at](std::string_view key) { return num_at(key, false); };
        u.in = num_after("\"prompt_tokens\"");
        u.out = num_after("\"completion_tokens\"");
        u.cached = num_after("\"cached_tokens\"");
        // No cache-write count is read on this branch. OpenAI bills the write on the
        // GPT-5.6 family, but the field it reports it under is not verified here.
        if (u.in >= 0 || u.out >= 0) return u; // OpenAI shape, done

        // Anthropic names the same three things differently, and a byte-forwarded
        // stream is exactly where nothing translates them for us. Claude Code
        // speaks this dialect, so without these its every request records zero
        // tokens and therefore zero cost.
        const long long fresh = num_after("\"input_tokens\"");
        u.out = num_at("\"output_tokens\"", /*last=*/true);
        if (fresh < 0) return u; // no Anthropic usage block either
        // Normalize to the OpenAI convention so `in` and `cached` mean the same thing
        // whatever the venue. OpenAI's `prompt_tokens` is the entire prompt with
        // `cached_tokens` a subset of it; Anthropic's `input_tokens` is the fresh
        // part only, with cache reads and the cache-creation write reported
        // separately. Add them back: `in` is the whole prompt, `cached` is the
        // discounted read subset of it, so `in - cached` is the full-rate part on
        // both. The creation write is part of `in` too and is reported separately in
        // `cache_write`, because it is billed above the input rate and a caller that
        // cannot see it prices a first turn low.
        const long long read = num_after("\"cache_read_input_tokens\"");
        const long long write = num_after("\"cache_creation_input_tokens\"");
        u.cached = read > 0 ? read : 0;
        u.cache_write = write > 0 ? write : 0;
        const long long w5 = num_after("\"ephemeral_5m_input_tokens\"");
        const long long w1 = num_after("\"ephemeral_1h_input_tokens\"");
        if (w5 >= 0 || w1 >= 0)
        {
            u.cache_write_5m = w5 > 0 ? w5 : 0;
            u.cache_write_1h = w1 > 0 ? w1 : 0;
        }
        u.in = fresh + u.cached + u.cache_write;
        return u;
    }

} // namespace llmbridge::detail
