// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Tests for the incremental Anthropic->OpenAI SSE translator (provider/sse.hpp).
// The emitted OpenAI chunks are re-parsed with our own JSON to assert structure,
// and every stream is replayed byte-by-byte to prove fragmentation-robustness.

#include "provider/sse.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "provider/json.hpp"

using llmbridge::provider::AnthropicToOpenAiSse;
using llmbridge::json::parse;
using llmbridge::json::Value;

namespace
{
    // A realistic Anthropic text stream: message_start, a text block with two
    // deltas, message_delta (stop_reason), message_stop. \n\n between events.
    const char* kAnthropicText =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_01\",\"type\":\"message\","
        "\"role\":\"assistant\",\"model\":\"claude-3-5-sonnet-20241022\",\"content\":[],"
        "\"stop_reason\":null,\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}}\n\n"
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\", world\"}}\n\n"
        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":5}}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";

    std::deque<std::string> g_backing; // pointer-stable backing for parsed views

    // Split translated output into the payloads after each "data: " line.
    std::vector<std::string> data_payloads(const std::string& out)
    {
        std::vector<std::string> v;
        size_t i = 0;
        while (i < out.size())
        {
            size_t nl = out.find('\n', i);
            if (nl == std::string::npos) nl = out.size();
            std::string_view line(out.data() + i, nl - i);
            if (line.rfind("data: ", 0) == 0) v.emplace_back(line.substr(6));
            i = nl + 1;
        }
        return v;
    }

    Value P(std::string s)
    {
        g_backing.push_back(std::move(s));
        bool ok = false;
        Value v = parse(g_backing.back(), ok);
        EXPECT_TRUE(ok);
        return v;
    }

    // Feed the whole input in one shot.
    std::string translate_whole(std::string_view in)
    {
        AnthropicToOpenAiSse t;
        std::string out;
        t.feed(in, out);
        t.finish(out);
        return out;
    }

    // Feed the input one byte at a time — worst-case fragmentation.
    std::string translate_byte_by_byte(std::string_view in)
    {
        AnthropicToOpenAiSse t;
        std::string out;
        for (char c : in) t.feed(std::string_view(&c, 1), out);
        t.finish(out);
        return out;
    }
} // namespace

TEST(Sse, TextStreamStructure)
{
    const std::string out = translate_whole(kAnthropicText);
    const auto payloads = data_payloads(out);

    // role chunk, "Hello", ", world", finish chunk, [DONE]
    ASSERT_EQ(payloads.size(), 5u);
    EXPECT_EQ(payloads.back(), "[DONE]");

    // First chunk: assistant role, correct id/model, chunk object.
    Value first = P(payloads[0]);
    EXPECT_EQ(first.str_or("id"), "msg_01");
    EXPECT_EQ(first.str_or("object"), "chat.completion.chunk");
    EXPECT_EQ(first.str_or("model"), "claude-3-5-sonnet-20241022");
    const Value* ch0 = first.find("choices");
    ASSERT_TRUE(ch0 && ch0->is_array() && ch0->arr.size() == 1);
    EXPECT_EQ(ch0->arr[0].find("delta")->str_or("role"), "assistant");

    // Content deltas reassemble to the full message.
    std::string content;
    for (size_t i = 1; i <= 2; ++i)
    {
        Value c = P(payloads[i]);
        content += std::string(c.find("choices")->arr[0].find("delta")->str_or("content"));
    }
    EXPECT_EQ(content, "Hello, world");

    // Finish chunk: empty delta, finish_reason "stop".
    Value fin = P(payloads[3]);
    const Value* fch = fin.find("choices");
    ASSERT_TRUE(fch && fch->is_array() && fch->arr.size() == 1);
    EXPECT_EQ(fch->arr[0].str_or("finish_reason"), "stop");
    const Value* fdelta = fch->arr[0].find("delta");
    ASSERT_TRUE(fdelta && fdelta->is_object());
    EXPECT_TRUE(fdelta->obj.empty()); // no role/content on the finish chunk
}

TEST(Sse, FragmentationIsByteExact)
{
    // Worst-case (1 byte per read) must produce identical output to one-shot.
    EXPECT_EQ(translate_byte_by_byte(kAnthropicText), translate_whole(kAnthropicText));
}

TEST(Sse, MaxTokensMapsToLength)
{
    const char* s =
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"},\"usage\":{\"output_tokens\":1}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    const auto payloads = data_payloads(translate_whole(s));
    // finish chunk + [DONE]
    ASSERT_GE(payloads.size(), 2u);
    Value fin = P(payloads[payloads.size() - 2]);
    EXPECT_EQ(fin.find("choices")->arr[0].str_or("finish_reason"), "length");
}

TEST(Sse, EofWithoutMessageStopStillTerminates)
{
    // Upstream drops after a delta with no message_stop: finish() must still emit
    // a finish chunk + [DONE] so the client stream is well-formed.
    AnthropicToOpenAiSse t;
    std::string out;
    t.feed("data: {\"type\":\"content_block_delta\",\"index\":0,"
           "\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n",
           out);
    t.finish(out);
    const auto payloads = data_payloads(out);
    EXPECT_EQ(payloads.back(), "[DONE]");
    // role+content chunk, finish chunk, [DONE]
    ASSERT_GE(payloads.size(), 3u);
    Value fin = P(payloads[payloads.size() - 2]);
    EXPECT_EQ(fin.find("choices")->arr[0].str_or("finish_reason"), "stop");
}
