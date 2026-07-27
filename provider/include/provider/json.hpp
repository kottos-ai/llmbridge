// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Minimal, zero-dependency JSON for the llmbridge provider-translation layer.
//
// Deliberately hand-rolled (no library) — a tiny, allocation-light parser (the
// DOM does allocate its node vectors, but string values are zero-copy views into
// the input): a recursive-descent parser into an ordered DOM, plus a
// string-append builder with escaping. Scope is "enough to translate chat
// completion request/response bodies between provider dialects" — objects,
// arrays, strings, numbers, bools, null, and the common escape sequences. Not a
// general-purpose JSON library; it is fast and correct for the shapes we move.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llmbridge::json
{
    class Value
    {
    public:
        enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

        Type type = Type::Null;
        bool boolean = false;
        // For String, `sv` is the RAW span between the quotes (still JSON-escaped) —
        // a view into the parsed input, never decoded, never owned. For Number, it's
        // the raw number text. This makes the DOM zero-copy: a passthrough value is
        // emitted verbatim, since the input's escaping is exactly the output's.
        std::string_view sv;
        std::vector<Value> arr;                                  // Array elements
        std::vector<std::pair<std::string_view, Value>> obj;     // Object members, insertion order

        // Value is self-referential (arr/obj hold Value), so its special members are
        // declared here and defaulted out-of-line (just below the class). Defined
        // inline, clang + libstdc++ eagerly instantiate the recursive vector/pair
        // traits on an *incomplete* Value and hard-error; GCC defers, so it only
        // breaks the clang CI leg. Out-of-line = instantiated once Value is complete.
        Value();
        Value(const Value&);
        Value(Value&&) noexcept;
        Value& operator=(const Value&);
        Value& operator=(Value&&) noexcept;
        ~Value();

        [[nodiscard]] bool is_object() const { return type == Type::Object; }
        [[nodiscard]] bool is_array() const { return type == Type::Array; }
        [[nodiscard]] bool is_string() const { return type == Type::String; }

        // Object lookup; returns nullptr if absent or not an object.
        [[nodiscard]] const Value* find(std::string_view key) const
        {
            if (type != Type::Object) return nullptr;
            for (const auto& [k, v] : obj)
                if (k == key) return &v;
            return nullptr;
        }

        // A string member's raw (still-escaped) span, or `def`. Returns a view —
        // `def` and the parsed input must outlive the result.
        [[nodiscard]] std::string_view str_or(std::string_view key, std::string_view def = "") const
        {
            const Value* v = find(key);
            return (v && v->type == Type::String) ? v->sv : def;
        }
        // A number member's raw text (e.g. "256", "0.7"), or `def`.
        [[nodiscard]] std::string_view num_or(std::string_view key, std::string_view def = "") const
        {
            const Value* v = find(key);
            return (v && v->type == Type::Number) ? v->sv : def;
        }
    };

    // Out-of-line so the recursive vector<Value>/vector<pair<…,Value>> special
    // members are only instantiated here, where Value is a complete type.
    inline Value::Value() = default;
    inline Value::Value(const Value&) = default;
    inline Value::Value(Value&&) noexcept = default;
    inline Value& Value::operator=(const Value&) = default;
    inline Value& Value::operator=(Value&&) noexcept = default;
    inline Value::~Value() = default;

    namespace detail
    {
        // Recursion guard. Client-controlled bodies are parsed in translate mode,
        // so an input like "[[[[[..." must fail cleanly instead of overflowing the
        // stack. Chat-completion payloads nest well under 10 deep; 64 is generous
        // headroom while capping recursion at a safe, small stack footprint.
        inline constexpr int kMaxDepth = 64;

        struct Parser
        {
            std::string_view s;
            size_t i = 0;
            bool ok = true;
            int depth = 0; // current object/array nesting; bounded by kMaxDepth

            void ws()
            {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
            }

            // Return the RAW span between the quotes (assumes s[i]=='"'), still
            // JSON-escaped, as a view into the input — never decoded, never copied.
            // We scan only for the real closing quote, skipping escape pairs (\X).
            std::string_view parse_string()
            {
                ++i; // opening quote
                const size_t start = i;
                while (i < s.size())
                {
                    const char c = s[i];
                    if (c == '"') { std::string_view sv = s.substr(start, i - start); ++i; return sv; }
                    if (c == '\\') { i += 2; continue; } // skip the escaped char
                    ++i;
                }
                ok = false; // unterminated string
                return s.substr(start);
            }

            Value parse_value()
            {
                ws();
                if (i >= s.size()) { ok = false; return {}; }
                char c = s[i];
                if (c == '"') { Value v; v.type = Value::Type::String; v.sv = parse_string(); return v; }
                if (c == '{' || c == '[')
                {
                    if (depth >= kMaxDepth) { ok = false; return {}; } // too deep — refuse to recurse
                    ++depth;
                    Value v = (c == '{') ? parse_object() : parse_array();
                    --depth;
                    return v;
                }
                if (c == 't' || c == 'f') return parse_bool();
                if (c == 'n')
                {
                    if (s.compare(i, 4, "null") == 0) i += 4;
                    else ok = false;
                    return Value{}; // Null
                }
                return parse_number();
            }

            Value parse_bool()
            {
                Value v; v.type = Value::Type::Bool;
                if (s.compare(i, 4, "true") == 0) { v.boolean = true; i += 4; }
                else if (s.compare(i, 5, "false") == 0) { v.boolean = false; i += 5; }
                else ok = false;
                return v;
            }

            Value parse_number()
            {
                size_t start = i;
                while (i < s.size())
                {
                    char c = s[i];
                    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') ++i;
                    else break;
                }
                Value v; v.type = Value::Type::Number;
                v.sv = s.substr(start, i - start);
                if (v.sv.empty()) ok = false;
                return v;
            }

            Value parse_array()
            {
                Value v; v.type = Value::Type::Array;
                ++i; // [
                ws();
                if (i < s.size() && s[i] == ']') { ++i; return v; }
                while (i < s.size())
                {
                    v.arr.push_back(parse_value());
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; return v; }
                    ok = false; return v;
                }
                ok = false; return v;
            }

            Value parse_object()
            {
                Value v; v.type = Value::Type::Object;
                ++i; // {
                ws();
                if (i < s.size() && s[i] == '}') { ++i; return v; }
                while (i < s.size())
                {
                    ws();
                    if (i >= s.size() || s[i] != '"') { ok = false; return v; }
                    std::string_view key = parse_string();
                    ws();
                    if (i >= s.size() || s[i] != ':') { ok = false; return v; }
                    ++i; // :
                    v.obj.emplace_back(key, parse_value());
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; return v; }
                    ok = false; return v;
                }
                ok = false; return v;
            }
        };
    } // namespace detail

    // Parse a JSON document. On failure, sets ok=false and returns whatever was
    // parsed so far (callers should check ok).
    inline Value parse(std::string_view text, bool& ok)
    {
        detail::Parser p{text};
        Value v = p.parse_value();
        ok = p.ok;
        return v;
    }

    // Append a RAW (already JSON-escaped) span as a quoted string literal — the
    // zero-copy passthrough path. The bytes came from valid JSON input, so the
    // escaping is already correct; emit verbatim, no decode/re-encode.
    inline void append_raw_string(std::string& out, std::string_view raw)
    {
        out += '"';
        out += raw;
        out += '"';
    }

    // Append `raw` (a DECODED string) as a JSON string literal, escaping as needed.
    // Bulk-copies runs of chars that don't need escaping (the common case), only
    // escaping the special ones individually — so most content is a few memcpys.
    inline void append_escaped(std::string& out, std::string_view raw)
    {
        out += '"';
        const size_t n = raw.size();
        size_t start = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(raw[i]);
            if (c != '"' && c != '\\' && c >= 0x20) continue; // plain char; keep scanning
            out.append(raw.data() + start, i - start);        // flush the plain run
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default: // other control char < 0x20 -> \u00XX
                {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                }
            }
            start = i + 1;
        }
        out.append(raw.data() + start, n - start); // trailing plain run
        out += '"';
    }
} // namespace llmbridge::json
