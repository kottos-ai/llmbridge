// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Minimal, zero-dependency JSON for the llmbridge provider-translation layer.
//
// Deliberately hand-rolled, no library: a tiny, allocation-light parser (one arena
// per document holds every node, and string values are zero-copy views into the
// input): a recursive-descent parser into an ordered DOM, plus a
// string-append builder with escaping. Scope is "enough to translate chat
// completion request/response bodies between provider dialects": objects,
// arrays, strings, numbers, bools, null, and the common escape sequences. Not a
// general-purpose JSON library; it is fast and correct for the shapes we move.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llmbridge::provider::json
{
    class Value;
    struct Member;

    namespace detail
    {
        // One document, one allocation chain.
        //
        // Every object and array used to own a std::vector, which is two allocations
        // per node once growth is counted. Parsing a 287 KB agent request made 7,192
        // of them and 1.54 MB of node storage, and cost 1.07 ms of the 1.58 ms the
        // whole translation took.
        class BlockPool
        {
        public:
            std::unique_ptr<char[]> take(size_t& size)
            {
                for (size_t k = 0; k < _free.size(); ++k)
                    if (_free[k].second >= size)
                    {
                        auto block = std::move(_free[k].first);
                        size = _free[k].second;
                        _held -= size;
                        _free.erase(_free.begin() + static_cast<long>(k));
                        return block;
                    }
                return std::unique_ptr<char[]>(new char[size]);
            }

            void give(std::unique_ptr<char[]> block, size_t size)
            {
                // Bounded, so one pathological document cannot pin memory for the
                // life of the thread.
                if (_held + size > kMaxHeld) return;
                _held += size;
                _free.emplace_back(std::move(block), size);
            }

            ~BlockPool() = default;

        private:
            static constexpr size_t kMaxHeld = size_t{4} << 20;
            std::vector<std::pair<std::unique_ptr<char[]>, size_t>> _free;
            size_t _held = 0;
        };

        inline BlockPool& block_pool() noexcept
        {
            thread_local BlockPool p;
            return p;
        }

        class Arena
        {
        public:
            // `hint` is the document's byte count.
            explicit Arena(size_t hint = 0) noexcept
            {
                if (hint) _next = hint < 4096 ? 4096 : (hint > (size_t{1} << 20) ? (size_t{1} << 20) : hint);
            }
            Arena(const Arena&) = delete;
            Arena& operator=(const Arena&) = delete;

            ~Arena()
            {
                for (auto& [block, size] : _blocks) block_pool().give(std::move(block), size);
            }

            // Raw space for `n` T, uninitialized: the caller constructs into it. Sizes
            // are rounded to 16 so the bump pointer stays aligned for anything we put
            // in here, and `new char[]` already returns memory aligned that far.
            template <class T>
            T* alloc_n(size_t n)
            {
                static_assert(alignof(T) <= 16, "arena aligns to 16");
                if (n == 0) return nullptr;
                const size_t want = (n * sizeof(T) + 15) & ~static_cast<size_t>(15);
                if (want > _left) grow(want);
                char* const p = _cur;
                _cur += want;
                _left -= want;
                return reinterpret_cast<T*>(p);
            }

        private:
            void grow(size_t at_least)
            {
                // Blocks double up to a cap, so a big document does not pay a block
                // per node and a small one does not reserve a megabyte.
                size_t sz = _next > at_least ? _next : at_least;
                auto block = block_pool().take(sz); // sz becomes the block's real size
                _cur = block.get();
                _left = sz;
                _blocks.emplace_back(std::move(block), sz);
                if (_next < (size_t{1} << 20)) _next *= 2;
            }

            std::vector<std::pair<std::unique_ptr<char[]>, size_t>> _blocks;
            char* _cur = nullptr;
            size_t _left = 0;
            size_t _next = 4096;
        };
    } // namespace detail

    // A run of nodes owned by the document's arena. Read-only by construction: the
    // parser fills it once and nothing downstream mutates a parsed DOM.
    template <class T>
    class Span
    {
    public:
        Span() = default;
        Span(const T* p, uint32_t n) noexcept : _p(p), _n(n) {}

        [[nodiscard]] const T* begin() const noexcept { return _p; }
        [[nodiscard]] const T* end() const noexcept { return _p + _n; }
        [[nodiscard]] size_t size() const noexcept { return _n; }
        [[nodiscard]] bool empty() const noexcept { return _n == 0; }
        const T& operator[](size_t k) const noexcept { return _p[k]; }
        const T& front() const noexcept { return _p[0]; }
        const T& back() const noexcept { return _p[_n - 1]; }

    private:
        const T* _p = nullptr;
        uint32_t _n = 0;
    };

    class Value
    {
    public:
        enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

        Type type = Type::Null;
        bool boolean = false;
        // For String, `sv` is the raw span between the quotes, still JSON-escaped.
        // A view into the parsed input, never decoded, never owned. For Number, it's
        // the raw number text. This makes the DOM zero-copy: a passthrough value is
        // emitted verbatim, since the input's escaping is exactly the output's.
        // For Array and Object, `sv` is the raw span including the surrounding
        // brackets/braces, set by the parser so a subtree can be forwarded byte for
        // byte. Tool calling needs this: a tool's `parameters` is an arbitrary JSON
        // Schema we must pass through unaltered, and re-serialising from the DOM
        // would risk changing it (number formatting, escape forms, key order).
        std::string_view sv;
        Span<Value> arr;      // Array elements
        Span<Member> obj;     // Object members, insertion order

        // Defined out-of-line, below, where Value and Member are complete: the type
        // is self-referential, and clang instantiates eagerly where GCC defers, so
        // an inline definition here breaks only the clang CI leg.
        Value() noexcept = default;
        Value(Value&&) noexcept;
        Value& operator=(Value&&) noexcept;
        ~Value();
        // Deleted, not defaulted.
        Value(const Value&) = delete;
        Value& operator=(const Value&) = delete;

        [[nodiscard]] bool is_object() const { return type == Type::Object; }
        [[nodiscard]] bool is_array() const { return type == Type::Array; }
        [[nodiscard]] bool is_string() const { return type == Type::String; }

        // Object lookup; returns nullptr if absent or not an object.
        [[nodiscard]] const Value* find(std::string_view key) const;

        // A string member's raw (still-escaped) span, or `def`. Returns a view.
        // `def` and the parsed input must outlive the result.
        [[nodiscard]] std::string_view str_or(std::string_view key, std::string_view def = "") const;
        // A number member's raw text (e.g. "256", "0.7"), or `def`.
        [[nodiscard]] std::string_view num_or(std::string_view key, std::string_view def = "") const;

    private:
        friend Value parse(std::string_view, bool&);
        // Set on the root only, and only by parse(). Every node below it points into
        // this arena, so the root outliving them is the whole contract of the DOM.
        detail::Arena* _arena = nullptr;
    };

    // One object member. A struct, not std::pair, so that `Member*` can appear
    // in Value above while Value is still incomplete; structured bindings over it
    // read the same at the call sites.
    struct Member
    {
        std::string_view key;
        Value value;
    };

    inline Value::~Value() { delete _arena; }

    inline Value::Value(Value&& o) noexcept
        : type(o.type), boolean(o.boolean), sv(o.sv), arr(o.arr), obj(o.obj), _arena(o._arena)
    {
        o._arena = nullptr; // the arena has exactly one owner at all times
    }

    inline Value& Value::operator=(Value&& o) noexcept
    {
        if (this != &o)
        {
            delete _arena;
            type = o.type; boolean = o.boolean; sv = o.sv; arr = o.arr; obj = o.obj;
            _arena = o._arena;
            o._arena = nullptr;
        }
        return *this;
    }

    inline const Value* Value::find(std::string_view key) const
    {
        if (type != Type::Object) return nullptr;
        for (const auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }

    inline std::string_view Value::str_or(std::string_view key, std::string_view def) const
    {
        const Value* v = find(key);
        return (v && v->type == Type::String) ? v->sv : def;
    }

    inline std::string_view Value::num_or(std::string_view key, std::string_view def) const
    {
        const Value* v = find(key);
        return (v && v->type == Type::Number) ? v->sv : def;
    }

    namespace detail
    {
        // Recursion guard. Client-controlled bodies are parsed in translate mode,
        // so an input like "[[[[[..." must fail cleanly instead of overflowing the
        // stack. Chat-completion payloads nest well under 10 deep; 64 is generous
        // headroom while capping recursion at a safe, small stack footprint.
        inline constexpr int kMaxDepth = 64;

        // The parser's scratch stacks, kept alive between documents for the same
        // reason the arena blocks are: they grow to the width of the widest array in
        // the document (a megabyte on an agent request). `busy` guards the one
        // case that would corrupt them, a parse starting while another is running on
        // this thread.
        struct Scratch
        {
            std::vector<Value> values;
            std::vector<Member> members;
            bool busy = false;
        };

        inline Scratch& scratch() noexcept
        {
            thread_local Scratch s;
            return s;
        }

        struct Parser
        {
            std::string_view s;
            Arena& a;
            std::vector<Value>& vstack;
            std::vector<Member>& mstack;
            size_t i = 0;
            bool ok = true;
            int depth = 0; // current object/array nesting; bounded by kMaxDepth

            // Children are parsed onto these stacks and copied into the arena in one
            // exactly-sized run when the closing bracket arrives, because the count is
            // not known before then.
            Span<Value> commit_values(size_t mark)
            {
                const size_t n = vstack.size() - mark;
                if (n == 0) return {};
                Value* p = a.alloc_n<Value>(n);
                for (size_t k = 0; k < n; ++k) new (p + k) Value(std::move(vstack[mark + k]));
                vstack.resize(mark);
                return Span<Value>(p, static_cast<uint32_t>(n));
            }

            Span<Member> commit_members(size_t mark)
            {
                const size_t n = mstack.size() - mark;
                if (n == 0) return {};
                Member* p = a.alloc_n<Member>(n);
                for (size_t k = 0; k < n; ++k) new (p + k) Member{mstack[mark + k].key,
                                                                  std::move(mstack[mark + k].value)};
                mstack.resize(mark);
                return Span<Member>(p, static_cast<uint32_t>(n));
            }

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

            // Return the raw span between the quotes (assumes s[i]=='"'), still
            // JSON-escaped, as a view into the input: never decoded, never copied.
            // We scan only for the real closing quote, skipping escape pairs (\X).
            //
            // Strictness is LOAD-BEARING here. This span is re-emitted verbatim on the
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

            // Every exit truncates the stack back to this level's mark, the failing
            // ones included: a level that returned without doing so would leave its
            // half-parsed children to be committed as its parent's.
            Value parse_array()
            {
                Value v; v.type = Value::Type::Array;
                ++i; // [
                ws();
                if (i < s.size() && s[i] == ']') { ++i; return v; }
                const size_t mark = vstack.size();
                while (i < s.size())
                {
                    vstack.push_back(parse_value());
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; v.arr = commit_values(mark); return v; }
                    ok = false; v.arr = commit_values(mark); return v;
                }
                ok = false; v.arr = commit_values(mark); return v;
            }

            Value parse_object()
            {
                Value v; v.type = Value::Type::Object;
                ++i; // {
                ws();
                if (i < s.size() && s[i] == '}') { ++i; return v; }
                const size_t mark = mstack.size();
                while (i < s.size())
                {
                    ws();
                    if (i >= s.size() || s[i] != '"') { ok = false; v.obj = commit_members(mark); return v; }
                    std::string_view key = parse_string();
                    ws();
                    if (i >= s.size() || s[i] != ':') { ok = false; v.obj = commit_members(mark); return v; }
                    ++i; // :
                    mstack.push_back(Member{key, parse_value()});
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; v.obj = commit_members(mark); return v; }
                    ok = false; v.obj = commit_members(mark); return v;
                }
                ok = false; v.obj = commit_members(mark); return v;
            }
        };
    } // namespace detail

    // Parse a JSON document. On failure, sets ok=false and returns whatever was
    // parsed so far (callers should check ok).
    inline Value parse(std::string_view text, bool& ok)
    {
        // The arena is handed to the root even when the parse fails, because a failed
        // parse still returns whatever it built and those nodes live in here.
        auto* arena = new detail::Arena(text.size());
        detail::Scratch& sc = detail::scratch();
        const bool reentrant = sc.busy;
        std::vector<Value> own_values;
        std::vector<Member> own_members;
        sc.busy = true;
        detail::Parser p{text, *arena,
                         reentrant ? own_values : sc.values,
                         reentrant ? own_members : sc.members};
        Value v = p.parse_value();
        ok = p.ok;
        if (!reentrant)
        {
            // Emptied, not shrunk: the capacity is the point of keeping them.
            sc.values.clear();
            sc.members.clear();
            sc.busy = false;
        }
        v._arena = arena;
        return v;
    }

    // Append a raw (already JSON-escaped) span as a quoted string literal. The
    // zero-copy passthrough path. The bytes came from valid JSON input, so the
    // escaping is already correct; emit verbatim, no decode/re-encode.
    // Emit `text` as a JSON string literal (with quotes), escaping what must be
    // escaped. Use when the source is not already JSON-escaped, e.g. turning a raw
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

    // Append `raw` (a decoded string) as a JSON string literal, escaping as needed.
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
