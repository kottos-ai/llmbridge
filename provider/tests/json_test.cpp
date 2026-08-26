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
#include <cstdlib>
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
    // The parser is zero-copy: a string value is the raw span between the quotes,
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
    // Tool schemas are forwarded by span, so the span must be the exact source text
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
    // A rebuilt schema could normalize 1.50 -> 1.5 or drop spacing. Byte-for-byte
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
// The parser's string span is re-emitted verbatim on the passthrough path, so
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

// ── Number grammar (RFC 8259 §6) ─────────────────────────────────────────────
//
// The scanner used to consume any run of [0-9+-.eE] and call it a Number, so `1e`,
// `--1`, `1.2.3`, `+1`, `.5` and `00` all parsed. That was never an injection, since
// the character set is closed and a span cannot carry a quote or a brace, but it
// pushed validation onto every consumer: `strtod("1e")` fails, and a consumer that
// forgets the check silently gets 0.

namespace
{
    /// Named for the thing under test, since `parses(doc)` already exists above and
    /// takes a whole document.
    bool number_parses(const std::string& number)
    {
        bool ok = false;
        (void)llmbridge::provider::json::parse("{\"n\":" + number + "}", ok);
        return ok;
    }
} // namespace

TEST(JsonNumbers, EveryLegalShapeParses)
{
    for (const char* n : {"0", "-0", "1", "-1", "12345", "1.5", "-1.5", "0.5",
                          "1e5", "1E5", "1e+5", "1e-5", "1.5e-5", "1E+5", "1e999"})
        EXPECT_TRUE(number_parses(n)) << "rejected a valid JSON number: " << n;
}

TEST(JsonNumbers, MalformedShapesAreRejected)
{
    // Every one of these parsed before, and each is one a hand-written config or a
    // provider response could plausibly contain.
    for (const char* n : {"1e", "1e+", "1e-", "-", "--1", "1-2", "1.2.3", "+1", ".5",
                          "1.", ".", "-.", "1e5e5", "1e+-5", "1..2", "-e5"})
        EXPECT_FALSE(number_parses(n)) << "accepted a malformed number: " << n;
}

// RFC 8259 forbids a leading zero, and so does this: a reader who writes 08 means
// octal and would not get it.
TEST(JsonNumbers, LeadingZerosAreRejected)
{
    for (const char* n : {"00", "01", "-01", "0123"}) EXPECT_FALSE(number_parses(n)) << n;
    EXPECT_TRUE(number_parses("0"));
    EXPECT_TRUE(number_parses("0.1"));
    EXPECT_TRUE(number_parses("0e1"));
}

// A number ends where its grammar ends, so the character after it must still be
// framed by the object. Trailing junk is a parse error, not a longer number.
TEST(JsonNumbers, ANumberDoesNotSwallowWhatFollowsIt)
{
    bool ok = false;
    const auto v = llmbridge::provider::json::parse(R"({"a":1,"b":2})", ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(v.find("a")->sv, "1");
    EXPECT_EQ(v.find("b")->sv, "2");
    EXPECT_FALSE(number_parses("1x"));
    EXPECT_FALSE(number_parses("1 2"));
}

// Anything that does parse must survive strtod, because that is what every consumer
// does with it. This is the property the old scanner broke.
TEST(JsonNumbers, WhatParsesAlsoConvertsWithStrtod)
{
    for (const char* n : {"0", "-0", "12345", "1.5", "-1.5e-5", "1e999", "0.0001"})
    {
        bool ok = false;
        // Named, not a temporary. The DOM is zero-copy, so `v.sv` points into this
        // buffer; parsing a temporary leaves every view dangling the moment the full
        // expression ends. ASan calls it stack-use-after-scope, and it found this.
        const std::string doc = std::string("{\"n\":") + n + "}";
        const auto v = llmbridge::provider::json::parse(doc, ok);
        ASSERT_TRUE(ok) << n;
        const std::string txt(v.find("n")->sv);
        char* end = nullptr;
        (void)std::strtod(txt.c_str(), &end);
        EXPECT_EQ(*end, '\0') << "parsed as a Number but strtod stopped early: " << n;
    }
}

// ── the arena: what the node storage change can break ───────────────────────
//
// Nodes live in one arena the root owns, and children are parsed onto a scratch
// stack that is shared by every nesting level and truncated back to each level's
// mark. Both of those have a failure mode that ordinary parsing tests would not
// notice: storage shared between documents, and a level committing entries that
// belong to its parent.

TEST(JsonArena, NestedArraysKeepOnlyTheirOwnElements)
{
    // The scratch stack holds the outer array's elements while the inner arrays are
    // being parsed, so a level that committed from the wrong mark would hand the
    // outer array the inner one's entries, or lose its own. Uneven widths, so an
    // off-by-one shows up as a wrong count instead of a coincidence.
    bool ok = false;
    const std::string doc = R"([[1],[2,3],[4,5,6],[[7,8],[9]],10])";
    const auto v = llmbridge::provider::json::parse(doc, ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(v.arr.size(), 5u);
    EXPECT_EQ(v.arr[0].arr.size(), 1u);
    EXPECT_EQ(v.arr[1].arr.size(), 2u);
    EXPECT_EQ(v.arr[2].arr.size(), 3u);
    ASSERT_EQ(v.arr[3].arr.size(), 2u);
    EXPECT_EQ(v.arr[3].arr[0].arr.size(), 2u);
    EXPECT_EQ(v.arr[3].arr[1].arr.size(), 1u);
    EXPECT_EQ(v.arr[4].num_or("", "x"), "x"); // a number, not an array
    EXPECT_EQ(v.arr[2].arr[2].sv, "6");
}

TEST(JsonArena, NestedObjectsKeepOnlyTheirOwnMembers)
{
    bool ok = false;
    const std::string doc = R"({"a":{"x":1},"b":{"y":2,"z":3},"c":4})";
    const auto v = llmbridge::provider::json::parse(doc, ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(v.obj.size(), 3u);
    EXPECT_EQ(v.find("a")->obj.size(), 1u);
    EXPECT_EQ(v.find("b")->obj.size(), 2u);
    EXPECT_EQ(v.find("b")->num_or("z"), "3");
    EXPECT_EQ(v.num_or("c"), "4");
}

TEST(JsonArena, TwoLiveDocumentsDoNotShareStorage)
{
    // Each root owns its own arena. Parsed one after the other and read afterwards,
    // so a shared or recycled buffer would show up as the second document's contents
    // appearing in the first.
    const std::string a = R"({"who":"first","n":[1,2,3]})";
    const std::string b = R"({"who":"second","n":[4,5,6,7]})";
    bool ok1 = false, ok2 = false;
    const auto va = llmbridge::provider::json::parse(a, ok1);
    const auto vb = llmbridge::provider::json::parse(b, ok2);
    ASSERT_TRUE(ok1);
    ASSERT_TRUE(ok2);
    EXPECT_EQ(va.str_or("who"), "first");
    EXPECT_EQ(vb.str_or("who"), "second");
    EXPECT_EQ(va.find("n")->arr.size(), 3u);
    EXPECT_EQ(vb.find("n")->arr.size(), 4u);
    EXPECT_EQ(va.find("n")->arr[0].sv, "1");
    EXPECT_EQ(vb.find("n")->arr[0].sv, "4");
}

TEST(JsonArena, MovingTheRootKeepsEveryNodeReadable)
{
    // The arena moves with the root. If it did not, the moved-to value's children
    // would point into storage the moved-from destructor released.
    const std::string doc = R"({"messages":[{"role":"user","content":"hi"}]})";
    bool ok = false;
    llmbridge::provider::json::Value moved;
    {
        llmbridge::provider::json::Value v = llmbridge::provider::json::parse(doc, ok);
        ASSERT_TRUE(ok);
        moved = std::move(v);
    } // v is destroyed here; the nodes must not be
    ASSERT_EQ(moved.find("messages")->arr.size(), 1u);
    EXPECT_EQ(moved.find("messages")->arr[0].str_or("content"), "hi");
}

TEST(JsonArena, AWideArraySurvivesScratchGrowth)
{
    // Wider than any scratch vector's initial capacity, so the stack reallocates
    // mid-parse and every element must survive being moved.
    std::string doc = "[";
    for (int i = 0; i < 5000; ++i) doc += std::to_string(i) + ",";
    doc.back() = ']';
    bool ok = false;
    const auto v = llmbridge::provider::json::parse(doc, ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(v.arr.size(), 5000u);
    EXPECT_EQ(v.arr[0].sv, "0");
    EXPECT_EQ(v.arr[4999].sv, "4999");
    EXPECT_EQ(v.arr[2500].sv, "2500");
}

TEST(JsonArena, AFailedParseLeavesWhatItBuiltReadable)
{
    // parse() documents that it returns the partial DOM on failure, and the arena
    // goes to the root either way, so reading the partial result must not touch
    // freed storage. ASan is the real assertion here.
    bool ok = true;
    const std::string doc = R"([{"a":1},{"b":2},)";
    const auto v = llmbridge::provider::json::parse(doc, ok);
    EXPECT_FALSE(ok);
    for (const auto& e : v.arr) EXPECT_TRUE(e.is_object());
}

TEST(JsonArena, AFailedInnerArrayDoesNotDonateItsElementsToItsParent)
{
    // The failure path has to truncate the scratch stack exactly like the success
    // path. If it returns without doing so, the inner array's three entries are still
    // sitting above the outer array's mark, and the outer array commits them as its
    // own: one element becomes four, on malformed input, which is the input that
    // arrives from outside.
    bool ok = true;
    const std::string doc = "[[1,2,3";
    const auto v = llmbridge::provider::json::parse(doc, ok);
    EXPECT_FALSE(ok);
    EXPECT_EQ(v.arr.size(), 1u) << "the outer array holds one thing: the inner array";
    ASSERT_EQ(v.arr.size(), 1u);
    EXPECT_TRUE(v.arr[0].is_array());
}

TEST(JsonArena, AFailedInnerObjectDoesNotDonateItsMembersToItsParent)
{
    // Each way an object can fail needs its own case, because each is a separate
    // return and any one of them can forget to truncate: running out of input, a
    // member with no colon, and a member whose name is not a string.
    for (const char* doc : {R"({"a":{"x":1,"y":2)",      // ends mid-object
                            R"({"a":{"x":1,"y" 2}})",     // no colon after "y"
                            R"({"a":{"x":1,2:3}})"})      // a name that is not a string
    {
        bool ok = true;
        const std::string d = doc;
        const auto v = llmbridge::provider::json::parse(d, ok);
        EXPECT_FALSE(ok) << d;
        EXPECT_EQ(v.obj.size(), 1u) << "the outer object holds one member, \"a\": " << d;
    }
}
