// Tests for the hand-rolled JSON parser/builder (provider/json.hpp).

#include "provider/json.hpp"

#include <gtest/gtest.h>

#include <string>

using kottos::json::Value;
using kottos::json::parse;
using kottos::json::append_escaped;

namespace
{
    Value P(const std::string& s)
    {
        bool ok = false;
        Value v = parse(s, ok);
        EXPECT_TRUE(ok) << "failed to parse: " << s;
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

TEST(Json, StringEscapes)
{
    Value v = P(R"({"s":"a\"b\\c\nd\te"})");
    EXPECT_EQ(v.str_or("s"), "a\"b\\c\nd\te");
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
    // A literal multibyte char is just bytes — preserved as-is.
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
    EXPECT_EQ(d->str, "e");
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

TEST(JsonBuilder, EscapesRoundTrip)
{
    std::string out;
    const std::string raw = "line1\nline2\t\"quoted\" \\back\\";
    append_escaped(out, raw);
    // The escaped form must parse back to the original string.
    bool ok = false;
    Value v = parse("{\"s\":" + out + "}", ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(v.str_or("s"), raw);
}

TEST(JsonBuilder, EscapesControlChars)
{
    std::string out;
    std::string raw;
    raw += '\x01';
    append_escaped(out, raw);
    EXPECT_NE(out.find("\\u0001"), std::string::npos);
}
