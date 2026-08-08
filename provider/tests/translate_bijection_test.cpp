// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Bijection / round-trip tests for request translation.
//
// "OpenAI -> provider -> back should return the original message": the request
// translators must be LOSSLESS on the covered chat surface (model, system,
// user/assistant turns, max_tokens/temperature/top_p). We verify that by
// reducing both the original OpenAI request and the translated provider request
// to the same canonical form and asserting equality; i.e. the inverse mapping
// exists and recovers the original.
//
// The hard cases are LARGE bodies and nasty escaping, because the zero-copy DOM
// references raw still-escaped spans: any mis-scan of an escaped quote/backslash
// or any truncation would corrupt the recovered content and fail here.

#include "provider/json.hpp"
#include "provider/translate.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <vector>

using llmbridge::provider::json::Value;
using llmbridge::provider::json::parse;
using llmbridge::provider::openai_to_anthropic_request;
using llmbridge::provider::openai_to_gemini_request;
using llmbridge::provider::openai_to_cohere_request;

namespace
{
    // Pointer-stable backing store: the view-based DOM references its input, so the
    // bytes must outlive every Value we parse during a test.
    std::deque<std::string> g_backing;

    Value P(std::string s)
    {
        g_backing.push_back(std::move(s));
        bool ok = false;
        Value v = parse(g_backing.back(), ok);
        EXPECT_TRUE(ok) << "unparseable: " << g_backing.back().substr(0, 200);
        return v;
    }

    struct Turn
    {
        std::string role, content;
        bool operator==(const Turn&) const = default;
    };

    // Canonical chat form. Content is the RAW (still-escaped) bytes, concatenated
    // the way both the translator and a reader would, so equality is byte-exact.
    struct Canon
    {
        std::string model, system;
        std::vector<Turn> turns;
        std::string max_tokens, temperature, top_p;
        bool operator==(const Canon&) const = default;
    };

    // A content field is either a string or an array of {type:"text",text:...}
    // parts; flatten to the concatenated raw text (no surrounding quotes).
    std::string content_str(const Value* c)
    {
        std::string out;
        if (!c) return out;
        if (c->is_string()) { out.append(c->sv); return out; }
        if (c->is_array())
            for (const auto& p : c->arr)
                if (p.str_or("type") == "text") out.append(p.str_or("text"));
        return out;
    }

    Canon from_openai(const Value& v)
    {
        Canon c;
        c.model = std::string(v.str_or("model"));
        c.max_tokens = std::string(v.num_or("max_tokens"));
        c.temperature = std::string(v.num_or("temperature"));
        c.top_p = std::string(v.num_or("top_p"));
        bool has_sys = false;
        if (const Value* m = v.find("messages"); m && m->is_array())
            for (const auto& msg : m->arr)
            {
                const auto role = msg.str_or("role");
                if (role == "system")
                {
                    if (has_sys) c.system += "\\n";
                    c.system += content_str(msg.find("content"));
                    has_sys = true;
                }
                else
                {
                    c.turns.push_back({std::string(role), content_str(msg.find("content"))});
                }
            }
        return c;
    }

    Canon from_anthropic(const Value& v)
    {
        Canon c;
        c.model = std::string(v.str_or("model"));
        c.system = std::string(v.str_or("system"));
        c.max_tokens = std::string(v.num_or("max_tokens"));
        c.temperature = std::string(v.num_or("temperature"));
        c.top_p = std::string(v.num_or("top_p"));
        if (const Value* m = v.find("messages"); m && m->is_array())
            for (const auto& msg : m->arr)
                c.turns.push_back({std::string(msg.str_or("role")), content_str(msg.find("content"))});
        return c;
    }

    // Cohere v2 keeps OpenAI-style turns (system stays in the message list) and
    // remaps top_p -> "p"; recover the same canonical form.
    Canon from_cohere(const Value& v)
    {
        Canon c;
        c.model = std::string(v.str_or("model"));
        c.max_tokens = std::string(v.num_or("max_tokens"));
        c.temperature = std::string(v.num_or("temperature"));
        c.top_p = std::string(v.num_or("p"));
        bool has_sys = false;
        if (const Value* m = v.find("messages"); m && m->is_array())
            for (const auto& msg : m->arr)
            {
                const auto role = msg.str_or("role");
                if (role == "system")
                {
                    if (has_sys) c.system += "\\n";
                    c.system += content_str(msg.find("content"));
                    has_sys = true;
                }
                else
                {
                    c.turns.push_back({std::string(role), content_str(msg.find("content"))});
                }
            }
        return c;
    }

    // Gemini: role assistant<->model, system -> systemInstruction, no model field
    // in the body (model lives in the URL). Recover the canonical form sans model.
    Canon from_gemini(const Value& v)
    {
        Canon c;
        if (const Value* si = v.find("systemInstruction"))
            if (const Value* parts = si->find("parts"); parts && parts->is_array())
                for (const auto& p : parts->arr) c.system += p.str_or("text");
        if (const Value* gc = v.find("generationConfig"))
        {
            c.max_tokens = std::string(gc->num_or("maxOutputTokens"));
            c.temperature = std::string(gc->num_or("temperature"));
            c.top_p = std::string(gc->num_or("topP"));
        }
        if (const Value* cs = v.find("contents"); cs && cs->is_array())
            for (const auto& turn : cs->arr)
            {
                std::string role(turn.str_or("role"));
                if (role == "model") role = "assistant";
                std::string text;
                if (const Value* parts = turn.find("parts"); parts && parts->is_array())
                    for (const auto& p : parts->arr) text += p.str_or("text");
                c.turns.push_back({role, text});
            }
        return c;
    }

    // Build a fully-specified OpenAI request (all optional fields set, so no
    // default-injection ambiguity) embedding `payload` as the content of a system
    // message and three user/assistant turns. `payload` must be valid escaped JSON
    // string content.
    std::string build_openai(const std::string& payload)
    {
        // Plain literals (no raw strings) so a payload like  )"  can't collide with
        // a delimiter. Each turn is prefixed distinctly so reordering is detectable.
        auto msg = [](const char* role, const std::string& content) {
            return std::string("{\"role\":\"") + role + "\",\"content\":\"" + content + "\"}";
        };
        std::string b = "{\"model\":\"gpt-4o\",\"max_tokens\":512,\"temperature\":0.7,\"top_p\":0.9,\"messages\":[";
        b += msg("system", payload) + ",";
        b += msg("user", "u1 " + payload) + ",";
        b += msg("assistant", "a1 " + payload) + ",";
        b += msg("user", "u2 " + payload);
        b += "]}";
        return b;
    }

    std::string repeat(const std::string& unit, size_t times)
    {
        std::string out;
        out.reserve(unit.size() * times);
        for (size_t i = 0; i < times; ++i) out += unit;
        return out;
    }

    // (name, escaped JSON-string payload). Each is a distinct edge case for the
    // raw-span escape scanner.
    struct Payload { const char* name; std::string body; };

    std::vector<Payload> payloads()
    {
        std::vector<Payload> v = {
            {"plain", "hello world this is a perfectly ordinary message"},
            {"escaped_quotes", R"(she said \"hello there\" then \"goodbye\" loudly)"},
            {"backslashes", R"(windows path C:\\Users\\admin\\file.txt and unc \\\\server\\share)"},
            {"newlines", R"(line one\nline two\nline three\nline four\nfinal line)"},
            {"tabs_cr", R"(col1\tcol2\tcol3\r\nrow2a\trow2b)"},
            {"unicode_escapes", R"(café naïve 中文 ¡hola! — dash)"},
            {"forward_slashes", R"(http://example.com/path?q=1&r=2 and a\/b\/c)"},
            {"json_inside_string", R"({\"nested\":{\"k\":[1,2,3]},\"s\":\"v\"} as literal text)"},
            {"all_escapes_mixed", R"(q=\" b=\\ n=\n t=\t r=\r u=é done)"},
            {"emoji_literal_utf8", "rocket \xF0\x9F\x9A\x80 sushi \xF0\x9F\x8D\xA3 heart \xE2\x9D\xA4 done"},
            {"accented_literal_utf8", "caf\xC3\xA9 na\xC3\xAFve \xE4\xB8\xAD\xE6\x96\x87 stra\xC3\x9F" "e"},
            {"only_escaped_quote", R"(\")"},
            {"trailing_backslash_escaped", R"(ends with two backslashes \\\\)"},
            {"long_10k_plain", repeat("lorem ipsum dolor sit amet ", 400)},
            {"long_10k_escaped", repeat(R"(he said \"x\" then\nmoved on\t)", 300)},
            {"very_long_100k", repeat(R"(chunk \"q\" \\ \n é ABCDEFG 0123456789 )", 1800)},
            {"adjacent_escapes", R"(\\\"\\\"\\\"\n\n\t\tABC)"},
            {"quote_heavy", repeat(R"(\")", 500)},
        };
        return v;
    }

    class Bijection : public ::testing::TestWithParam<Payload> {};
} // namespace

// OpenAI -> Anthropic -> recover == original (the user's example direction).
TEST_P(Bijection, OpenAIToAnthropicIsLossless)
{
    const std::string oai = build_openai(GetParam().body);
    const std::string anth = openai_to_anthropic_request(oai);
    ASSERT_FALSE(anth.empty()) << "translation failed for payload: " << GetParam().name;
    EXPECT_EQ(from_openai(P(oai)), from_anthropic(P(anth)))
        << "round-trip lost data for payload: " << GetParam().name;
}

// OpenAI -> Cohere -> recover == original (Cohere keeps turns, remaps top_p->p).
TEST_P(Bijection, OpenAIToCohereIsLossless)
{
    const std::string oai = build_openai(GetParam().body);
    const std::string coh = openai_to_cohere_request(oai);
    ASSERT_FALSE(coh.empty()) << "translation failed for payload: " << GetParam().name;
    EXPECT_EQ(from_openai(P(oai)), from_cohere(P(coh)))
        << "round-trip lost data for payload: " << GetParam().name;
}

// OpenAI -> Gemini -> recover == original (model lives in the URL, so compare
// everything except model).
TEST_P(Bijection, OpenAIToGeminiIsLossless)
{
    const std::string oai = build_openai(GetParam().body);
    const std::string gem = openai_to_gemini_request(oai);
    ASSERT_FALSE(gem.empty()) << "translation failed for payload: " << GetParam().name;
    Canon a = from_openai(P(oai));
    Canon b = from_gemini(P(gem));
    a.model.clear(); // Gemini body carries no model field
    EXPECT_EQ(a, b) << "round-trip lost data for payload: " << GetParam().name;
}

INSTANTIATE_TEST_SUITE_P(Payloads, Bijection, ::testing::ValuesIn(payloads()),
                         [](const testing::TestParamInfo<Payload>& i) { return i.param.name; });

// A genuinely large, many-turn conversation survives intact (not just repeated
// single payloads): 60 turns, each with multi-KB escaped content.
TEST(BijectionBig, ManyTurnsLargeContentAnthropic)
{
    std::string body = R"({"model":"gpt-4o","max_tokens":4096,"temperature":0.2,"top_p":0.95,"messages":[)";
    body += R"({"role":"system","content":")" + repeat(R"(you are precise\nand \"careful\"\t)", 50) + R"("})";
    for (int i = 0; i < 60; ++i)
    {
        const char* role = (i % 2 == 0) ? "user" : "assistant";
        std::string content = repeat(R"(turn-)", 1) + std::to_string(i) +
                              repeat(R"( payload \"q\" \\ \n é \t lorem ipsum )", 80);
        body += std::string(R"(,{"role":")") + role + R"(","content":")" + content + R"("})";
    }
    body += "]}";

    const std::string anth = openai_to_anthropic_request(body);
    ASSERT_FALSE(anth.empty());
    Canon a = from_openai(P(body));
    Canon b = from_anthropic(P(anth));
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.turns.size(), 60u);
    EXPECT_GT(anth.size(), 150000u) << "expected a large translated body";
}

// Sanity: a corruption in the input is actually detectable by this harness (guards
// against a tautological comparison that would pass on anything).
TEST(BijectionBig, DetectsContentMismatch)
{
    Canon a = from_openai(P(build_openai("original content here")));
    Canon b = from_anthropic(P(openai_to_anthropic_request(build_openai("DIFFERENT content here"))));
    EXPECT_NE(a, b);
}
