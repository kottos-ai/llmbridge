// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Minimal, zero-dependency JSON for the llmbridge provider-translation layer.
//
// Deliberately hand-rolled, no library: a tiny, allocation-light parser (the
// DOM does allocate its node vectors, but string values are zero-copy views into
// the input): a recursive-descent parser into an ordered DOM, plus a
// string-append builder with escaping. Scope is "enough to translate chat
// completion request/response bodies between provider dialects": objects,
// arrays, strings, numbers, bools, null, and the common escape sequences. Not a
// general-purpose JSON library; it is fast and correct for the shapes we move.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llmbridge::provider::json
{
    class Value
    {
    public:
        enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

        Type type = Type::Null;
        bool boolean = false;
        // For String, `sv` is the RAW span between the quotes, still JSON-escaped.
        // A view into the parsed input, never decoded, never owned. For Number, it's
        // the raw number text. This makes the DOM zero-copy: a passthrough value is
        // emitted verbatim, since the input's escaping is exactly the output's.
        // For Array and Object, `sv` is the RAW span INCLUDING the surrounding
        // brackets/braces, set by the parser so a subtree can be forwarded byte for
        // byte. Tool calling needs this: a tool's `parameters` is an arbitrary JSON
        // Schema we must pass through unaltered, and re-serialising from the DOM
        // would risk changing it (number formatting, escape forms, key order).
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

        // A string member's raw (still-escaped) span, or `def`. Returns a view.
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

    // Out-of-line so the recursive vector<Value>/vector<pair<...,Value>> special
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

            // The complete set RFC 8259 §7 permits after a backslash.
            static constexpr bool is_escape_char(char c) noexcept
            {
                return c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' ||
                       c == 'n' || c == 'r' || c == 't' || c == 'u';
            }
            static constexpr bool is_hex(char c) noexcept
            {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            }

            // Return the RAW span between the quotes (assumes s[i]=='"'), still
            // JSON-escaped, as a view into the input: never decoded, never copied.
            // We scan only for the real closing quote, skipping escape pairs (\X).
            //
            // STRICTNESS IS LOAD-BEARING HERE. This span is re-emitted VERBATIM on the
            // passthrough path (see the comment on Value::sv), so whatever this scanner
            // accepts ends up in the bytes we hand the client. An earlier revision
            // skipped both checks below, and the measured result was: a provider string
            // containing a raw newline was copied straight through, and the client got
            // a 200 OK whose body Python's json.loads (and therefore the OpenAI SDK)
            // rejects with "Invalid control character". We laundered malformed provider
            // output into a malformed client response and called it success.
            //
            // So refuse, per the fail-closed policy, instead of sanitise-and-forward:
            // a caller that cannot re-serialise its input unchanged has no business
            // passing it on.
            std::string_view parse_string()
            {
                ++i; // opening quote
                const size_t start = i;
                while (i < s.size())
                {
                    const unsigned char c = static_cast<unsigned char>(s[i]);
                    if (c == '"') { std::string_view sv = s.substr(start, i - start); ++i; return sv; }
                    if (c < 0x20) break; // raw control character. RFC 8259 §7 forbids it
                    if (c == '\\')
                    {
                        if (i + 1 >= s.size()) break;
                        const char e = s[i + 1];
                        if (!is_escape_char(e)) break; // e.g. "\q", which would re-emit invalid
                        if (e == 'u')
                        {
                            if (i + 5 >= s.size()) break;
                            if (!is_hex(s[i + 2]) || !is_hex(s[i + 3]) ||
                                !is_hex(s[i + 4]) || !is_hex(s[i + 5]))
                                break; // "\uZZZZ": same problem, one level down
                            i += 6;
                            continue;
                        }
                        i += 2;
                        continue;
                    }
                    ++i;
                }
                ok = false; // unterminated, raw control character, or bad escape
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
                    if (depth >= kMaxDepth) { ok = false; return {}; } // too deep: refuse to recurse
                    const size_t start = i; // for the raw-subtree span
                    ++depth;
                    Value v = (c == '{') ? parse_object() : parse_array();
                    --depth;
                    v.sv = s.substr(start, i - start); // includes the braces/brackets
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

            /// RFC 8259 grammar: `-? int frac? exp?`, walked in that order.
            ///
            /// It used to consume any run of [0-9+-.eE] and call the result a Number,
            /// which accepted `1e`, `--1`, `1.2.3`, `+1`, `.5` and `00`, none of them
            /// JSON. The consequence was not an injection (the character set is closed,
            /// so a span can never carry a quote or a brace and cannot break out of the
            /// JSON it is re-emitted into) but every consumer had to re-validate: a
            /// `strtod` on `1e` fails, and one that forgets the check gets 0.
            ///
            /// Leading zeros are refused because RFC 8259 refuses them, and because a
            /// reader who writes `08` usually means octal and never gets it.
            Value parse_number()
            {
                const size_t start = i;
                const auto digit = [&] { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
                const auto digits = [&] {
                    const size_t from = i;
                    while (digit()) ++i;
                    return i > from;
                };

                if (i < s.size() && s[i] == '-') ++i;   // optional sign, minus only
                if (!digit()) { ok = false; return {}; }
                if (s[i] == '0') ++i;                   // a leading zero stands alone
                else digits();

                if (i < s.size() && s[i] == '.')        // frac: at least one digit
                {
                    ++i;
                    if (!digits()) { ok = false; return {}; }
                }
                if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) // exp: sign optional
                {
                    ++i;
                    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                    if (!digits()) { ok = false; return {}; }
                }

                Value v;
                v.type = Value::Type::Number;
                v.sv = s.substr(start, i - start);
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

    // Append a RAW (already JSON-escaped) span as a quoted string literal. The
    // zero-copy passthrough path. The bytes came from valid JSON input, so the
    // escaping is already correct; emit verbatim, no decode/re-encode.
    // Emit `text` as a JSON string literal (with quotes), escaping what must be
    // escaped. Use when the SOURCE IS NOT already JSON-escaped, e.g. turning a raw
    // JSON subtree into OpenAI's `arguments`, which is a *string* containing JSON.
    inline void append_escaped_string(std::string& out, std::string_view text)
    {
        out += '"';
        for (const char c : text)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        // Control characters must be \u-escaped or the output is
                        // invalid JSON that some parsers accept and others reject.
                        static const char* kHex = "0123456789abcdef";
                        out += "\\u00";
                        out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                        out += kHex[static_cast<unsigned char>(c) & 0xF];
                    }
                    else out += c;
            }
        }
        out += '"';
    }

    // Decode a raw (still-escaped) string span into plain bytes. The inverse of the
    // above, needed because OpenAI carries tool arguments as a JSON string while
    // Anthropic carries them as a JSON object: to cross that boundary the escaped
    // text has to become real JSON.
    //
    // \uXXXX is decoded to UTF-8; a lone surrogate is passed through as U+FFFD
    // instead of emitting invalid UTF-8, since the output goes to a provider that
    // will reject a malformed body.
    inline std::string unescape_string(std::string_view raw)
    {
        std::string out;
        out.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i] != '\\' || i + 1 >= raw.size()) { out += raw[i]; continue; }
            switch (raw[++i])
            {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u':
                {
                    if (i + 4 >= raw.size()) return out;
                    unsigned cp = 0;
                    for (int k = 1; k <= 4; ++k)
                    {
                        const char h = raw[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return out;
                    }
                    i += 4;
                    // Surrogate pair -> one code point.
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < raw.size() && raw[i + 1] == '\\' &&
                        raw[i + 2] == 'u')
                    {
                        unsigned lo = 0;
                        bool okpair = true;
                        for (int k = 3; k <= 6; ++k)
                        {
                            const char h = raw[i + k];
                            lo <<= 4;
                            if (h >= '0' && h <= '9') lo |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= static_cast<unsigned>(h - 'A' + 10);
                            else { okpair = false; break; }
                        }
                        if (okpair && lo >= 0xDC00 && lo <= 0xDFFF)
                        {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; // lone surrogate
                    // UTF-8 encode.
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800)
                    {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    else if (cp < 0x10000)
                    {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    else
                    {
                        out += static_cast<char>(0xF0 | (cp >> 18));
                        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += raw[i]; break;
            }
        }
        return out;
    }

    inline void append_raw_string(std::string& out, std::string_view raw)
    {
        out += '"';
        out += raw;
        out += '"';
    }

    // Append `raw` (a DECODED string) as a JSON string literal, escaping as needed.
    // Bulk-copies runs of chars that don't need escaping (the common case), only
    // escaping the special ones individually, so most content is a few memcpys.
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
} // namespace llmbridge::provider::json
