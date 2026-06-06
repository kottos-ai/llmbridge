#pragma once

// Minimal, zero-dependency JSON for the Kottos provider-translation layer.
//
// Deliberately hand-rolled (no library) — a tiny zero-alloc parser, like the
// rest of Kottos: a recursive-descent parser into an ordered DOM, plus a
// string-append builder with escaping. Scope is "enough to translate chat
// completion request/response bodies between provider dialects" — objects,
// arrays, strings, numbers, bools, null, and the common escape sequences. Not a
// general-purpose JSON library; it is fast and correct for the shapes we move.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kottos::json
{
    class Value
    {
    public:
        enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

        Type type = Type::Null;
        bool boolean = false;
        std::string str;                                   // String (decoded) or Number (raw text)
        std::vector<Value> arr;                            // Array elements
        std::vector<std::pair<std::string, Value>> obj;    // Object members, insertion order

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

        // Convenience: a string member's decoded value, or a default.
        [[nodiscard]] std::string str_or(std::string_view key, std::string_view def = "") const
        {
            const Value* v = find(key);
            return (v && v->type == Type::String) ? v->str : std::string(def);
        }
        // Convenience: a number member's raw text (e.g. "256", "0.7"), or a default.
        [[nodiscard]] std::string num_or(std::string_view key, std::string_view def = "") const
        {
            const Value* v = find(key);
            return (v && v->type == Type::Number) ? v->str : std::string(def);
        }
    };

    namespace detail
    {
        struct Parser
        {
            std::string_view s;
            size_t i = 0;
            bool ok = true;

            void ws()
            {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
            }

            // Decode a JSON string (assumes s[i]=='"'); advances past the closing quote.
            std::string parse_string()
            {
                std::string out;
                ++i; // opening quote
                while (i < s.size())
                {
                    char c = s[i++];
                    if (c == '"') return out;
                    if (c == '\\' && i < s.size())
                    {
                        char e = s[i++];
                        switch (e)
                        {
                            case '"': out += '"'; break;
                            case '\\': out += '\\'; break;
                            case '/': out += '/'; break;
                            case 'n': out += '\n'; break;
                            case 't': out += '\t'; break;
                            case 'r': out += '\r'; break;
                            case 'b': out += '\b'; break;
                            case 'f': out += '\f'; break;
                            case 'u':
                                // Pass the \uXXXX through verbatim — we never need to
                                // interpret code points to shuttle text between dialects.
                                out += "\\u";
                                for (int k = 0; k < 4 && i < s.size(); ++k) out += s[i++];
                                break;
                            default: out += e; break;
                        }
                    }
                    else
                    {
                        out += c;
                    }
                }
                ok = false; // unterminated string
                return out;
            }

            Value parse_value()
            {
                ws();
                if (i >= s.size()) { ok = false; return {}; }
                char c = s[i];
                if (c == '"') { Value v; v.type = Value::Type::String; v.str = parse_string(); return v; }
                if (c == '{') return parse_object();
                if (c == '[') return parse_array();
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
                v.str = std::string(s.substr(start, i - start));
                if (v.str.empty()) ok = false;
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
                    std::string key = parse_string();
                    ws();
                    if (i >= s.size() || s[i] != ':') { ok = false; return v; }
                    ++i; // :
                    v.obj.emplace_back(std::move(key), parse_value());
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

    // Append `raw` as a JSON string literal (with surrounding quotes + escaping).
    inline void append_escaped(std::string& out, std::string_view raw)
    {
        out += '"';
        for (char c : raw)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        static const char* hex = "0123456789abcdef";
                        out += "\\u00";
                        out += hex[(c >> 4) & 0xF];
                        out += hex[c & 0xF];
                    }
                    else out += c;
            }
        }
        out += '"';
    }
} // namespace kottos::json
