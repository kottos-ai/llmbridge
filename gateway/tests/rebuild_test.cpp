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

#include "gateway/gateway.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

// The knob's whole point is that the pages are mapped before a request arrives:
// with it off, the reserved scratch is address space and nothing more.
TEST(Rebuild, PrefaultMapsTheScratchPages)
{
    const size_t want = 4u << 20;
    llmbridge::Gateway off(0, "127.0.0.1", 1);
    // An empty string points into its own object, so one resident page is the floor.
    EXPECT_LE(off.prefault_resident_bytes_for_test(), 4096u);
    llmbridge::Gateway on(0, "127.0.0.1", 1);
    on.set_prefault_bytes(want);
    EXPECT_GE(on.prefault_resident_bytes_for_test(), want);
}

// A retired buffer keeps its pages for the next connection, and the counter says
// when a build had to grow the scratch. Both are the rotation the tape exposed.
TEST(Rebuild, ARetiredBufferIsHandedToTheNextConnection)
{
    llmbridge::Gateway gw(0, "127.0.0.1", 1);
    llmbridge::Connection dying;
    dying.is_client = false;
    dying.wbuf.assign(1u << 20, 'k'); // a request, credential included, still in it
    const char* pages = dying.wbuf.data();
    gw.retire_wbuf(&dying);
    EXPECT_EQ(gw.warm_bufs_for_test(), 1u);
    EXPECT_TRUE(dying.wbuf.empty());
    llmbridge::Connection born;
    born.is_client = false;
    gw.adopt_warm(&born);
    EXPECT_EQ(gw.warm_bufs_for_test(), 0u);
    EXPECT_EQ(born.wbuf.data(), pages);
    EXPECT_GE(born.wbuf.capacity(), 1u << 20);
    EXPECT_TRUE(born.wbuf.empty());
    EXPECT_EQ(std::count(born.wbuf.data(), born.wbuf.data() + (1u << 20), 'k'), 0)
        << "the credential must not survive into the next connection";
    EXPECT_EQ(gw.stats().warm_reuses, 1u);
    // Too small to matter, a client's buffer, and a full list are all left alone.
    llmbridge::Connection small;
    small.is_client = false;
    small.wbuf.assign(1024, 'x');
    gw.retire_wbuf(&small);
    llmbridge::Connection client;
    client.wbuf.assign(1u << 20, 'x');
    gw.retire_wbuf(&client);
    EXPECT_EQ(gw.warm_bufs_for_test(), 0u);
    for (int i = 0; i < 6; ++i)
    {
        llmbridge::Connection d;
        d.is_client = false;
        d.wbuf.assign(1u << 17, 'x');
        gw.retire_wbuf(&d);
    }
    EXPECT_EQ(gw.warm_bufs_for_test(), llmbridge::Gateway::kWarmBufs);
}

TEST(Rebuild, AColdBuildIsCounted)
{
    llmbridge::Gateway gw(0, "127.0.0.1", 1);
    gw.note_build(1u << 20);
    EXPECT_EQ(gw.stats().cold_builds, 1u);
    gw.set_prefault_bytes(4u << 20);
    gw.prefault_resident_bytes_for_test();
    gw.note_build(1u << 20);
    EXPECT_EQ(gw.stats().cold_builds, 1u) << "a prefaulted scratch holds it without growing";
}
