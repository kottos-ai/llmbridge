// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// DOM-free readers over JSON text: the top-level walk behind model_of and the
// gateway's body facts, and the key-anchored scans over a response's bytes.

#include <cstddef>
#include <cstring>
#include <string_view>

namespace llmbridge::provider::json_scan
{
    inline constexpr size_t kNpos = std::string_view::npos;

    inline bool is_ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    inline size_t skip_ws(std::string_view b, size_t i) noexcept
    {
        while (i < b.size() && is_ws(b[i])) ++i;
        return i;
    }

    // The quote closing a string whose contents start at `i`: the first quote
    // behind an even run of backslashes.
    inline size_t string_close(std::string_view b, size_t i) noexcept
    {
        const char* const base = b.data();
        for (size_t from = i; from < b.size();)
        {
            const void* q = std::memchr(base + from, '"', b.size() - from);
            if (!q) break;
            const auto at = static_cast<size_t>(static_cast<const char*>(q) - base);
            size_t k = at;
            while (k > i && base[k - 1] == '\\') --k;
            if (((at - k) & 1) == 0) return at;
            from = at + 1;
        }
        return kNpos;
    }

    // Past one value starting at `i`, or kNpos when it does not end inside `b`. A
    // literal or number ends at the next structural byte or whitespace.
    inline size_t skip_value(std::string_view b, size_t i) noexcept
    {
        if (i >= b.size()) return kNpos;
        if (b[i] == '"')
        {
            const size_t close = string_close(b, i + 1);
            return close == kNpos ? kNpos : close + 1;
        }
        if (b[i] == '{' || b[i] == '[')
        {
            int depth = 0;
            while (i < b.size())
            {
                const char c = b[i];
                if (c == '"')
                {
                    const size_t close = string_close(b, i + 1);
                    if (close == kNpos) return kNpos;
                    i = close + 1;
                    continue;
                }
                if (c == '{' || c == '[') ++depth;
                else if ((c == '}' || c == ']') && --depth == 0) return i + 1;
                ++i;
            }
            return kNpos;
        }
        const size_t from = i;
        while (i < b.size() && b[i] != ',' && b[i] != '}' && b[i] != ']' && !is_ws(b[i])) ++i;
        return i == from ? kNpos : i;
    }

    /// Calls `f(key, value)` for each member of the object at the front of `obj`
    /// (raw key, raw value) until `f` returns false. False when `obj` is not an
    /// object or stops parsing before its closing brace.
    template <class F>
    bool for_each_member(std::string_view obj, F&& f) noexcept
    {
        size_t i = skip_ws(obj, 0);
        if (i >= obj.size() || obj[i] != '{') return false;
        i = skip_ws(obj, i + 1);
        if (i < obj.size() && obj[i] == '}') return true;
        while (true)
        {
            if (i >= obj.size() || obj[i] != '"') return false;
            const size_t kb = ++i;
            i = string_close(obj, kb);
            if (i == kNpos) return false;
            const std::string_view k = obj.substr(kb, i - kb);
            i = skip_ws(obj, i + 1);
            if (i >= obj.size() || obj[i] != ':') return false;
            i = skip_ws(obj, i + 1);
            const size_t vb = i;
            i = skip_value(obj, i);
            if (i == kNpos) return false;
            if (!f(k, obj.substr(vb, i - vb))) return true;
            i = skip_ws(obj, i);
            if (i < obj.size() && obj[i] == '}') return true;
            if (i >= obj.size() || obj[i] != ',') return false;
            i = skip_ws(obj, i + 1);
        }
    }

    /// The first top-level `key`, stopping there; empty when absent or unwalkable.
    inline std::string_view first_member(std::string_view body, std::string_view key) noexcept
    {
        std::string_view out;
        (void)for_each_member(body, [&](std::string_view k, std::string_view v) {
            if (k != key) return true;
            out = v;
            return false;
        });
        return out;
    }

    /// Where the value of the key `"name"` found at `at` begins, or kNpos when that
    /// match is not a key: its quote must follow `{` or `,` and its colon must follow
    /// it. A quote inside a string is escaped, so prompt text cannot forge one.
    inline size_t key_value_at(std::string_view b, size_t at, size_t len) noexcept
    {
        size_t p = at;
        while (p > 0 && is_ws(b[p - 1])) --p;
        if (p == 0 || (b[p - 1] != '{' && b[p - 1] != ',')) return kNpos;
        const size_t c = skip_ws(b, at + len);
        if (c >= b.size() || b[c] != ':') return kNpos;
        const size_t v = skip_ws(b, c + 1);
        return v < b.size() ? v : kNpos;
    }

    /// Each value of the key `quoted` (`"name"`, quotes included) in `b`, in order,
    /// until `f(value_begin)` returns false.
    template <class F>
    void each_key(std::string_view b, std::string_view quoted, F&& f) noexcept
    {
        for (size_t from = 0; from + quoted.size() <= b.size();)
        {
            const void* p = ::memmem(b.data() + from, b.size() - from, quoted.data(), quoted.size());
            if (!p) return;
            const auto at = static_cast<size_t>(static_cast<const char*>(p) - b.data());
            from = at + 1;
            if (const size_t v = key_value_at(b, at, quoted.size()); v != kNpos && !f(v)) return;
        }
    }

    /// A string value without escapes, unquoted; empty for anything else.
    inline std::string_view plain_string(std::string_view raw) noexcept
    {
        if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return {};
        const std::string_view in = raw.substr(1, raw.size() - 2);
        return in.find('\\') == kNpos ? in : std::string_view{};
    }

    /// A count: a non-negative integer of at most 18 digits, else -1. A longer one
    /// would overflow, and a fraction or exponent is not a count.
    inline long long count(std::string_view raw) noexcept
    {
        if (raw.empty() || raw.size() > 18) return -1;
        long long v = 0;
        for (const char c : raw)
        {
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    }
} // namespace llmbridge::provider::json_scan
