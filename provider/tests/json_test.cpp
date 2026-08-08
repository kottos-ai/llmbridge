// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Tests for the hand-rolled JSON parser/builder (provider/json.hpp).

#include "provider/json.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <string>

using llmbridge::provider::json::Value;
using llmbridge::provider::json::parse;
using llmbridge::provider::json::append_escaped;

namespace
{
    // The DOM is view-based: a parsed Value holds string_views into its input, so
    // the backing bytes must outlive it. Stash each input in a pointer-stable store
    // (std::deque never relocates elements) that lives for the whole test binary.
    std::deque<std::string> g_backing;

    Value P(std::string s)
    {
        g_backing.push_back(std::move(s));
        bool ok = false;
        Value v = parse(g_backing.back(), ok);
        EXPECT_TRUE(ok) << "failed to parse: " << g_backing.back();
        return v;
    }
} // namespace

TEST(Json, ParsesFlatObjectStringAndNumber)
{
    Value v = P(R"({"model":"gpt-4","max_tokens":256})");
    ASSERT_TRUE(v.is_object());
    EXPECT_EQ(v.str_or("model"), "gpt-4");
    EXPECT_EQ(v.num_or("max_tokens"), "256");
}

TEST(Json, MissingKeyReturnsDefault)
{
    Value v = P(R"({"a":1})");
    EXPECT_EQ(v.str_or("nope", "def"), "def");
    EXPECT_EQ(v.num_or("nope", "0"), "0");
    EXPECT_EQ(v.find("nope"), nullptr);
}

TEST(Json, NestedObjectLookup)
{
    Value v = P(R"({"usage":{"input_tokens":11,"output_tokens":7}})");
    const Value* u = v.find("usage");
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->num_or("input_tokens"), "11");
    EXPECT_EQ(u->num_or("output_tokens"), "7");
}

TEST(Json, ArrayOfObjects)
{
    Value v = P(R"({"messages":[{"role":"user","content":"hi"},{"role":"assistant","content":"yo"}]})");
    const Value* m = v.find("messages");
    ASSERT_NE(m, nullptr);
    ASSERT_TRUE(m->is_array());
    ASSERT_EQ(m->arr.size(), 2u);
    EXPECT_EQ(m->arr[0].str_or("role"), "user");
    EXPECT_EQ(m->arr[0].str_or("content"), "hi");
    EXPECT_EQ(m->arr[1].str_or("role"), "assistant");
}

TEST(Json, EmptyArrayAndObject)
{
    EXPECT_TRUE(P(R"({"a":[]})").find("a")->is_array());
    EXPECT_EQ(P(R"({"a":[]})").find("a")->arr.size(), 0u);
    EXPECT_TRUE(P(R"({"a":{}})").find("a")->is_object());
}

TEST(Json, StringEscapesPassedThroughRaw)
{
    // The parser is zero-copy: a string value is the RAW span between the quotes,
    // still JSON-escaped, never decoded. The backslash escapes survive verbatim.
    Value v = P(R"({"s":"a\"b\\c\nd\te"})");
    EXPECT_EQ(v.str_or("s"), "a\\\"b\\\\c\\nd\\te");
}

TEST(Json, UnicodeEscapePassedThrough)
{
    // A \uXXXX escape is shuttled verbatim (we never interpret code points).
    // The raw-string input below contains the 6 literal chars: \ u 0 0 e 9.
    Value v = P(R"({"s":"caf\u00e9"})");
    EXPECT_EQ(v.str_or("s"), "caf\\u00e9");
}

TEST(Json, RawUtf8BytesPassThrough)
{
    // A literal multibyte char is just bytes, preserved as-is.
    Value v = P("{\"s\":\"caf\xC3\xA9\"}");
    EXPECT_EQ(v.str_or("s"), "caf\xC3\xA9");
}

TEST(Json, NumberForms)
{
    EXPECT_EQ(P(R"({"n":-12})").num_or("n"), "-12");
    EXPECT_EQ(P(R"({"n":0.7})").num_or("n"), "0.7");
    EXPECT_EQ(P(R"({"n":1e3})").num_or("n"), "1e3");
    EXPECT_EQ(P(R"({"n":-3.5e-2})").num_or("n"), "-3.5e-2");
}

TEST(Json, BoolAndNull)
{
    Value v = P(R"({"a":true,"b":false,"c":null})");
    const Value* a = v.find("a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->type, Value::Type::Bool);
    EXPECT_TRUE(a->boolean);
    EXPECT_FALSE(v.find("b")->boolean);
    EXPECT_EQ(v.find("c")->type, Value::Type::Null);
}

TEST(Json, WhitespaceTolerated)
{
    Value v = P("{  \"a\" : 1 , \"b\" : \"x\" }\n");
    EXPECT_EQ(v.num_or("a"), "1");
    EXPECT_EQ(v.str_or("b"), "x");
}

TEST(Json, DeeplyNested)
{
    Value v = P(R"({"a":{"b":{"c":[{"d":"e"}]}}})");
    const Value* d = v.find("a")->find("b")->find("c")->arr[0].find("d");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->sv, "e");
}

TEST(Json, MalformedSetsNotOk)
{
    bool ok = true;
    parse(R"({"a":})", ok);
    EXPECT_FALSE(ok);
    ok = true;
    parse(R"({"a":"unterminated)", ok);
    EXPECT_FALSE(ok);
    ok = true;
    parse(R"({"a":1,)", ok);
    EXPECT_FALSE(ok);
}

TEST(Json, DeepNestingRejectedNotStackOverflow)
{
    // Recursion-bomb guard: thousands of '[' must fail cleanly (ok=false), not
    // overflow the stack and crash the process. The DOM is unused; we only care
    // that parse() returns and reports failure.
    std::string bomb(8192, '[');
    bool ok = true;
    parse(bomb, ok); // must not crash
    EXPECT_FALSE(ok);

    std::string obj_bomb;
    for (int k = 0; k < 8192; ++k) obj_bomb += R"({"a":)";
    ok = true;
    parse(obj_bomb, ok); // must not crash
    EXPECT_FALSE(ok);
}

TEST(Json, NestingWithinLimitStillParses)
{
    // Just under kMaxDepth (64): must still parse successfully.
    std::string s(60, '[');
    s += '1';
    s += std::string(60, ']');
    bool ok = false;
    parse(s, ok);
    EXPECT_TRUE(ok);
}

TEST(JsonBuilder, EscapedFormIsSelfConsistent)
{
    std::string out;
    const std::string raw = "line1\nline2\t\"quoted\" \\back\\";
    append_escaped(out, raw);
    // The special chars are escaped on the way out.
    EXPECT_NE(out.find("\\n"), std::string::npos);
    EXPECT_NE(out.find("\\t"), std::string::npos);
    EXPECT_NE(out.find("\\\""), std::string::npos);
    EXPECT_NE(out.find("\\\\"), std::string::npos);
    // The parser no longer decodes: re-parsing the escaped literal yields the raw
    // span verbatim, still-escaped: exactly what append_escaped emitted, minus
    // the surrounding quotes. So append_escaped + parse round-trips byte-for-byte.
    const std::string doc = "{\"s\":" + out + "}"; // must outlive the view-based DOM
    bool ok = false;
    Value v = parse(doc, ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(v.str_or("s"), out.substr(1, out.size() - 2));
}

TEST(JsonBuilder, EscapesControlChars)
{
    std::string out;
    std::string raw;
    raw += '\x01';
    append_escaped(out, raw);
    EXPECT_NE(out.find("\\u0001"), std::string::npos);
}

// ── Raw subtree spans + escape/unescape (added for tool calling) ─────────────

TEST(RawSpan, ObjectAndArraySpansIncludeTheirBrackets)
{
    // Tool schemas are forwarded by span, so the span must be the EXACT source text
    // including delimiters; anything else corrupts a customer's JSON Schema.
    const std::string src = R"({"o":{"a":[1,2,{"b":null}]},"arr":[{"x":1}],"s":"str"})";
    bool ok = false;
    const auto v = llmbridge::provider::json::parse(src, ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(v.find("o")->sv, R"({"a":[1,2,{"b":null}]})");
    EXPECT_EQ(v.find("arr")->sv, R"([{"x":1}])");
    EXPECT_EQ(v.find("s")->sv, "str") << "string spans stay quote-free";
}

TEST(RawSpan, PreservesWhitespaceAndNumberFormatting)
{
    // A rebuilt schema could normalise 1.50 -> 1.5 or drop spacing. Byte-for-byte
    // forwarding must not.
    const std::string src = "{\"p\": { \"n\" : 1.50 , \"e\":1e+3 }}";
    bool ok = false;
    const auto v = llmbridge::provider::json::parse(src, ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(v.find("p")->sv, "{ \"n\" : 1.50 , \"e\":1e+3 }");
}

TEST(EscapeRoundTrip, JsonBecomesAStringAndBack)
{
    const std::string original = R"({"city":"Paris","q":"say \"hi\"","n":-1.5})";
    std::string escaped;
    llmbridge::provider::json::append_escaped_string(escaped, original);
    bool ok = false;
    const auto v = llmbridge::provider::json::parse(escaped, ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(v.is_string());
    EXPECT_EQ(llmbridge::provider::json::unescape_string(v.sv), original);
}

TEST(EscapeRoundTrip, ControlCharactersAreEscapedNotEmittedRaw)
{
    std::string out;
    llmbridge::provider::json::append_escaped_string(out, std::string("a\tb\nc\x01""d"));
    // A raw control byte inside a JSON string is invalid JSON that some parsers
    // accept and others reject: exactly the ambiguity to avoid on a provider wire.
    EXPECT_EQ(out.find('\x01'), std::string::npos);
    EXPECT_NE(out.find("\\u0001"), std::string::npos) << out;
    EXPECT_NE(out.find("\\t"), std::string::npos);
    EXPECT_NE(out.find("\\n"), std::string::npos);
    bool ok = false;
    llmbridge::provider::json::parse(out, ok);
    EXPECT_TRUE(ok) << "escaper produced invalid JSON: " << out;
}

TEST(Unescape, DecodesUnicodeIncludingSurrogatePairs)
{
    EXPECT_EQ(llmbridge::provider::json::unescape_string("caf\\u00e9"), "café");
    EXPECT_EQ(llmbridge::provider::json::unescape_string("\\ud83d\\ude00"), "😀"); // U+1F600
    EXPECT_EQ(llmbridge::provider::json::unescape_string("a\\/b"), "a/b");
}

TEST(Unescape, LoneSurrogateBecomesReplacementNotInvalidUtf8)
{
    // Emitting a lone surrogate as UTF-8 would be malformed and the provider would
    // reject the whole body; U+FFFD keeps the request valid and the failure local.
    const std::string out = llmbridge::provider::json::unescape_string("\\ud800");
    EXPECT_EQ(out, "\xEF\xBF\xBD");
}

TEST(Unescape, TruncatedEscapesDoNotReadPastTheEnd)
{
    // Malformed input must terminate, not scan off the end.
    EXPECT_NO_THROW((void)llmbridge::provider::json::unescape_string("abc\\"));
    EXPECT_NO_THROW((void)llmbridge::provider::json::unescape_string("\\u12"));
    EXPECT_NO_THROW((void)llmbridge::provider::json::unescape_string("\\ud83d\\u"));
}

// --- RFC 8259 §7 string strictness (found by the corpus concurrency test) -----
//
// The parser's string span is re-emitted VERBATIM on the passthrough path, so
// anything accepted here reaches the client's bytes. Accepting an illegal string
// therefore does not produce a lenient parse; it produces a 200 OK whose body a
// strict parser rejects. Measured before the fix: a provider answer containing a
// raw newline arrived at the client as `"content":"line1<LF>line2"`, which
// Python's json.loads (and so the OpenAI SDK) refuses with "Invalid control
// character". Refuse at the parse instead.

namespace
{
    bool parses(std::string_view doc)
    {
        bool ok = false;
        (void)llmbridge::provider::json::parse(doc, ok);
        return ok;
    }
} // namespace

TEST(JsonStrictness, RawControlCharactersInStringsAreRejected)
{
    EXPECT_FALSE(parses("{\"t\":\"a\nb\"}")) << "raw newline";
    EXPECT_FALSE(parses("{\"t\":\"a\tb\"}")) << "raw tab";
    EXPECT_FALSE(parses("{\"t\":\"a\rb\"}")) << "raw carriage return";
    EXPECT_FALSE(parses(std::string("{\"t\":\"a\x01" "b\"}"))) << "raw 0x01";
    EXPECT_FALSE(parses(std::string("{\"t\":\"a\x1f" "b\"}"))) << "raw 0x1f";
    // The boundary: 0x20 is a space and perfectly legal.
    EXPECT_TRUE(parses("{\"t\":\"a b\"}"));
}

TEST(JsonStrictness, InvalidEscapesAreRejected)
{
    EXPECT_FALSE(parses(R"({"t":"a\qb"})")) << "\\q is not an escape";
    EXPECT_FALSE(parses(R"({"t":"a\xb"})")) << "\\x is not JSON";
    EXPECT_FALSE(parses(R"({"t":"a\"})")) << "trailing backslash";
    EXPECT_FALSE(parses(R"({"t":"\uZZZZ"})")) << "non-hex \\u";
    EXPECT_FALSE(parses(R"({"t":"\u12"})")) << "truncated \\u";
}

TEST(JsonStrictness, EveryLegalEscapeStillParses)
{
    // The fix must not over-reject: this is the complete RFC 8259 escape set,
    // plus raw UTF-8, which is legal and must stay zero-copy.
    EXPECT_TRUE(parses(R"({"t":"\" \\ \/ \b \f \n \r \t"})"));
    EXPECT_TRUE(parses(R"({"t":"é 中 😀"})"));
    EXPECT_TRUE(parses("{\"t\":\"caf\xc3\xa9 \xe6\x9d\xb1\xe4\xba\xac \xF0\x9F\x98\x80\"}"));
    EXPECT_TRUE(parses(R"({"t":"nothing special at all"})"));
}

TEST(JsonStrictness, IllegalStringInAKeyIsAlsoRejected)
{
    // Keys go through the same scanner and are re-emitted the same way.
    EXPECT_FALSE(parses("{\"a\nb\":1}"));
    EXPECT_FALSE(parses(R"({"a\qb":1})"));
}
