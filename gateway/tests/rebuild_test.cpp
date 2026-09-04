// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The byte-forward rebuild keeps its destination's allocation across requests.
// A request that grows a little each turn used to reallocate, and fault in, a fresh
// copy of itself every time; the fault run was 2.6 ms of a 3 ms request leg on the
// live tenant. These pin the capacity contract, and the content, on both sides.

#include "request.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    std::string request_of(size_t body_bytes)
    {
        const std::string body(body_bytes, 'x');
        return "POST /v1/messages HTTP/1.1\r\nHost: client\r\nContent-Type: application/json\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    size_t head_len(const std::string& msg) { return msg.find("\r\n\r\n") + 4; }
} // namespace

TEST(Rebuild, ASlightlyLargerRequestReusesTheBuffer)
{
    const std::vector<std::string> strip;
    std::string into;
    const std::string first = request_of(1'000'000);
    ASSERT_TRUE(llmbridge::detail::request_without(first, head_len(first), strip, "venue", "", {}, into));
    const char* where = into.data();
    const size_t cap = into.capacity();
    EXPECT_GE(cap, into.size() + into.size() / 8 - 200) << "headroom of an eighth is reserved";

    const std::string second = request_of(1'050'000); // 5% larger: inside the headroom
    ASSERT_TRUE(llmbridge::detail::request_without(second, head_len(second), strip, "venue", "", {}, into));
    EXPECT_EQ(into.data(), where) << "no reallocation for growth inside the headroom";
    EXPECT_EQ(into.capacity(), cap);
    EXPECT_EQ(into.substr(into.size() - 1'050'000), std::string(1'050'000, 'x')) << "the body is verbatim";
    EXPECT_NE(into.find("\r\nHost: venue\r\n"), std::string::npos) << "the venue's Host replaces the client's";
    EXPECT_EQ(into.find("Host: client"), std::string::npos);
    EXPECT_NE(into.find("Content-Length: 1050000\r\n"), std::string::npos) << "the length describes the body sent";
}

TEST(Rebuild, ARefusedRequestLeavesTheBufferEmpty)
{
    const std::vector<std::string> strip;
    std::string into;
    const std::string ok = request_of(64);
    ASSERT_TRUE(llmbridge::detail::request_without(ok, head_len(ok), strip, "venue", "", {}, into));
    ASSERT_FALSE(into.empty());
    // An absolute-form target cannot take a base path prefix, so the rebuild refuses.
    const std::string absolute = "POST http://elsewhere/v1 HTTP/1.1\r\nContent-Length: 2\r\n\r\nxx";
    EXPECT_FALSE(llmbridge::detail::request_without(absolute, head_len(absolute), strip, "venue", "/base", {}, into));
    EXPECT_TRUE(into.empty());
}

// The three other producers of an upstream body get the same treatment: the
// buffer form must yield the bytes of the value form and keep its storage.

static std::string big_openai(size_t turns)
{
    std::string b = R"({"model":"claude-sonnet-4-5","max_tokens":64,"stream":true,)"
                    R"("stream_options":{"include_usage":true},"messages":[)"
                    R"({"role":"system","content":"be brief"})";
    for (size_t i = 0; i < turns; ++i)
    {
        b += R"(,{"role":"user","content":")" + std::string(1000, 'u') + "\"}";
        b += R"(,{"role":"assistant","content":")" + std::string(1000, 'a') + "\"}";
    }
    return b + "]}";
}

TEST(Rebuild, AnthropicBufferFormMatchesValueForm)
{
    std::string buf;
    const std::string small = big_openai(20);
    bool want_v = false, want_b = false;
    ASSERT_TRUE(llmbridge::provider::openai_to_anthropic_request(small, buf, &want_b));
    EXPECT_EQ(buf, llmbridge::provider::openai_to_anthropic_request(small, &want_v));
    EXPECT_EQ(want_v, want_b);
    const char* before = buf.data();
    const size_t cap = buf.capacity();
    const std::string bigger = big_openai(21);
    ASSERT_TRUE(llmbridge::provider::openai_to_anthropic_request(bigger, buf));
    EXPECT_EQ(buf, llmbridge::provider::openai_to_anthropic_request(bigger));
    EXPECT_EQ(buf.data(), before);
    EXPECT_EQ(buf.capacity(), cap);
    EXPECT_FALSE(llmbridge::provider::openai_to_anthropic_request("not json", buf));
    EXPECT_TRUE(buf.empty());
}

TEST(Rebuild, BedrockBufferFormMatchesValueForm)
{
    std::string buf, model_b, model_v;
    const std::string body = big_openai(5);
    ASSERT_TRUE(llmbridge::provider::openai_to_bedrock_request(body, model_b, buf));
    EXPECT_EQ(buf, llmbridge::provider::openai_to_bedrock_request(body, model_v));
    EXPECT_EQ(model_b, model_v);
    EXPECT_EQ(model_b, "claude-sonnet-4-5");
}

TEST(Rebuild, OverridesBufferFormMatchesValueForm)
{
    std::string buf;
    const std::string body = big_openai(20);
    std::string_view had_b, had_v;
    ASSERT_TRUE(llmbridge::provider::apply_overrides(body, "claude-haiku-4-5", "auto", &had_b, buf));
    EXPECT_EQ(buf, llmbridge::provider::apply_overrides(body, "claude-haiku-4-5", "auto", &had_v));
    EXPECT_EQ(had_b, had_v);
    const char* before = buf.data();
    ASSERT_TRUE(llmbridge::provider::apply_overrides(big_openai(21), "claude-haiku-4-5", "", nullptr, buf));
    EXPECT_EQ(buf.data(), before);
    ASSERT_TRUE(llmbridge::provider::apply_overrides(body, "", "", nullptr, buf));
    EXPECT_EQ(buf, body);
    EXPECT_FALSE(llmbridge::provider::apply_overrides(body, "bad\"model", "", nullptr, buf));
}

TEST(Rebuild, HttpRequestFramingKeepsItsBuffer)
{
    std::string buf;
    llmbridge::detail::build_http_request("POST /v1/messages HTTP/1.1", "{}", "h",
                                           "x-api-key: k\r\n", buf);
    EXPECT_EQ(buf, "POST /v1/messages HTTP/1.1\r\nHost: h\r\nx-api-key: k\r\n"
                   "Content-Type: application/json\r\nConnection: keep-alive\r\n"
                   "Content-Length: 2\r\n\r\n{}");
    const char* before = buf.data();
    llmbridge::detail::build_http_request("POST /v1/messages HTTP/1.1", "{\"a\":1}", "h",
                                           "x-api-key: k\r\n", buf);
    EXPECT_EQ(buf.data(), before);
}
