// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Tests for the OpenAI <-> Anthropic translation (provider/translate.hpp).
// Outputs are re-parsed with our own JSON to assert structure.

#include "provider/translate.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <string>

#include "provider/json.hpp"

using llmbridge::provider::openai_to_anthropic_request;
using llmbridge::provider::anthropic_to_openai_response;
using llmbridge::provider::openai_to_gemini_request;
using llmbridge::provider::gemini_to_openai_response;
using llmbridge::provider::openai_to_cohere_request;
using llmbridge::provider::cohere_to_openai_response;
using llmbridge::json::Value;
using llmbridge::json::parse;

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
        EXPECT_TRUE(ok) << "unparseable translation output: " << g_backing.back();
        return v;
    }
} // namespace

// ── request: OpenAI -> Anthropic ────────────────────────────────────────────
TEST(ReqXlate, ExtractsSystemAndKeepsTurns)
{
    std::string in = R"({"model":"gpt-4o","max_tokens":256,"messages":[
        {"role":"system","content":"be terse"},
        {"role":"user","content":"hi"},
        {"role":"assistant","content":"hello"},
        {"role":"user","content":"bye"}]})";
    Value out = P(openai_to_anthropic_request(in));
    EXPECT_EQ(out.str_or("system"), "be terse");        // system pulled to top level
    EXPECT_EQ(out.num_or("max_tokens"), "256");
    const Value* m = out.find("messages");
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->arr.size(), 3u);                        // system removed from messages
    EXPECT_EQ(m->arr[0].str_or("role"), "user");
    EXPECT_EQ(m->arr[0].str_or("content"), "hi");
    EXPECT_EQ(m->arr[2].str_or("content"), "bye");
}

TEST(ReqXlate, DefaultsMaxTokensWhenAbsent)
{
    std::string in = R"({"model":"gpt-4o","messages":[{"role":"user","content":"hi"}]})";
    Value out = P(openai_to_anthropic_request(in));
    EXPECT_EQ(out.num_or("max_tokens"), "1024"); // Anthropic requires it
}

TEST(ReqXlate, NoSystemKeyWhenNoSystemMessage)
{
    std::string in = R"({"model":"m","messages":[{"role":"user","content":"hi"}]})";
    Value out = P(openai_to_anthropic_request(in));
    EXPECT_EQ(out.find("system"), nullptr);
}

TEST(ReqXlate, PassesTemperatureAndTopP)
{
    std::string in = R"({"model":"m","temperature":0.3,"top_p":0.9,"messages":[{"role":"user","content":"x"}]})";
    Value out = P(openai_to_anthropic_request(in));
    EXPECT_EQ(out.num_or("temperature"), "0.3");
    EXPECT_EQ(out.num_or("top_p"), "0.9");
}

TEST(ReqXlate, ConcatenatesMultipleSystemMessages)
{
    std::string in = R"({"model":"m","messages":[
        {"role":"system","content":"a"},
        {"role":"system","content":"b"},
        {"role":"user","content":"hi"}]})";
    Value out = P(openai_to_anthropic_request(in));
    // Joined with an escaped newline in the JSON; the raw span is the 4 bytes a \ n b,
    // i.e. the wire string "a\nb" (a real newline once a client decodes it).
    EXPECT_EQ(out.str_or("system"), "a\\nb");
}

TEST(ReqXlate, ContentAsArrayOfTextParts)
{
    std::string in = R"({"model":"m","messages":[
        {"role":"user","content":[{"type":"text","text":"foo"},{"type":"text","text":"bar"}]}]})";
    Value out = P(openai_to_anthropic_request(in));
    EXPECT_EQ(out.find("messages")->arr[0].str_or("content"), "foobar");
}

TEST(ReqXlate, MalformedReturnsEmpty)
{
    EXPECT_TRUE(openai_to_anthropic_request("not json").empty());
    EXPECT_TRUE(openai_to_anthropic_request(R"(["array","not","object"])").empty());
}

TEST(ReqXlate, PreservesModelString)
{
    Value out = P(openai_to_anthropic_request(R"({"model":"claude-x","messages":[]})"));
    EXPECT_EQ(out.str_or("model"), "claude-x");
}

// ── response: Anthropic -> OpenAI ───────────────────────────────────────────
TEST(RespXlate, JoinsTextBlocksToContent)
{
    std::string in = R"({"id":"msg_1","model":"claude-x","content":[
        {"type":"text","text":"hello "},{"type":"text","text":"world"}],
        "stop_reason":"end_turn","usage":{"input_tokens":8,"output_tokens":3}})";
    Value out = P(anthropic_to_openai_response(in));
    EXPECT_EQ(out.str_or("object"), "chat.completion");
    const Value* ch = out.find("choices");
    ASSERT_NE(ch, nullptr);
    ASSERT_EQ(ch->arr.size(), 1u);
    EXPECT_EQ(ch->arr[0].find("message")->str_or("content"), "hello world");
    EXPECT_EQ(ch->arr[0].str_or("finish_reason"), "stop");
}

TEST(RespXlate, UsageMapsAndSumsTotal)
{
    std::string in = R"({"content":[{"type":"text","text":"x"}],"usage":{"input_tokens":40,"output_tokens":2}})";
    Value out = P(anthropic_to_openai_response(in));
    const Value* u = out.find("usage");
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->num_or("prompt_tokens"), "40");
    EXPECT_EQ(u->num_or("completion_tokens"), "2");
    EXPECT_EQ(u->num_or("total_tokens"), "42");
}

class StopReasonMap : public ::testing::TestWithParam<std::pair<const char*, const char*>> {};
TEST_P(StopReasonMap, MapsToOpenAiFinishReason)
{
    const auto& [anthropic, openai] = GetParam();
    std::string in = std::string(R"({"content":[{"type":"text","text":"x"}],"stop_reason":")") + anthropic + "\"}";
    Value out = P(anthropic_to_openai_response(in));
    EXPECT_EQ(out.find("choices")->arr[0].str_or("finish_reason"), openai);
}
INSTANTIATE_TEST_SUITE_P(Cases, StopReasonMap,
                         ::testing::Values(std::make_pair("end_turn", "stop"),
                                           std::make_pair("max_tokens", "length"),
                                           std::make_pair("stop_sequence", "stop"),
                                           std::make_pair("tool_use", "tool_calls")),
                         [](const testing::TestParamInfo<std::pair<const char*, const char*>>& i) {
                             return std::string(i.param.first);
                         });

TEST(RespXlate, MissingUsageIsZero)
{
    std::string in = R"({"content":[{"type":"text","text":"x"}],"stop_reason":"end_turn"})";
    Value out = P(anthropic_to_openai_response(in));
    EXPECT_EQ(out.find("usage")->num_or("total_tokens"), "0");
}

TEST(RespXlate, MalformedReturnsEmpty)
{
    EXPECT_TRUE(anthropic_to_openai_response("}{").empty());
}

// ── full round trip: an OpenAI request, translated out and a matching
//    Anthropic response translated back, stays coherent. ─────────────────────
TEST(RoundTrip, RequestThenResponseStaysCoherent)
{
    std::string oai_req = R"({"model":"gpt-4o","max_tokens":64,"messages":[
        {"role":"system","content":"sys"},{"role":"user","content":"ping"}]})";
    Value areq = P(openai_to_anthropic_request(oai_req));
    EXPECT_EQ(areq.str_or("system"), "sys");
    EXPECT_EQ(areq.find("messages")->arr[0].str_or("content"), "ping");

    // Simulate the provider's reply in Anthropic format, translate back.
    std::string aresp = R"({"id":"msg_2","model":"claude-x","content":[{"type":"text","text":"pong"}],
        "stop_reason":"end_turn","usage":{"input_tokens":5,"output_tokens":1}})";
    Value oai_resp = P(anthropic_to_openai_response(aresp));
    EXPECT_EQ(oai_resp.find("choices")->arr[0].find("message")->str_or("content"), "pong");
    EXPECT_EQ(oai_resp.find("usage")->num_or("total_tokens"), "6");
}

// ════════════════════════════════════════════════════════════════════════════
// Google Gemini (generateContent)
// ════════════════════════════════════════════════════════════════════════════

TEST(GeminiReq, MapsRolesAndParts)
{
    std::string in = R"({"model":"gpt-4o","messages":[
        {"role":"user","content":"hi"},
        {"role":"assistant","content":"hello"},
        {"role":"user","content":"bye"}]})";
    Value out = P(openai_to_gemini_request(in));
    const Value* c = out.find("contents");
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->arr.size(), 3u);
    EXPECT_EQ(c->arr[0].str_or("role"), "user");          // user stays user
    EXPECT_EQ(c->arr[1].str_or("role"), "model");         // assistant -> model
    EXPECT_EQ(c->arr[0].find("parts")->arr[0].str_or("text"), "hi");
    EXPECT_EQ(c->arr[2].find("parts")->arr[0].str_or("text"), "bye");
}

TEST(GeminiReq, SystemBecomesSystemInstruction)
{
    std::string in = R"({"model":"m","messages":[
        {"role":"system","content":"be terse"},
        {"role":"user","content":"hi"}]})";
    Value out = P(openai_to_gemini_request(in));
    ASSERT_NE(out.find("systemInstruction"), nullptr);
    EXPECT_EQ(out.find("systemInstruction")->find("parts")->arr[0].str_or("text"), "be terse");
    EXPECT_EQ(out.find("contents")->arr.size(), 1u);      // system removed from contents
}

TEST(GeminiReq, GenerationConfigCarriesParams)
{
    std::string in = R"({"model":"m","max_tokens":256,"temperature":0.3,"top_p":0.9,
        "messages":[{"role":"user","content":"x"}]})";
    Value out = P(openai_to_gemini_request(in));
    const Value* gc = out.find("generationConfig");
    ASSERT_NE(gc, nullptr);
    EXPECT_EQ(gc->num_or("maxOutputTokens"), "256");      // max_tokens -> maxOutputTokens
    EXPECT_EQ(gc->num_or("temperature"), "0.3");
    EXPECT_EQ(gc->num_or("topP"), "0.9");                 // top_p -> topP
}

TEST(GeminiReq, NoGenerationConfigWhenNoParams)
{
    Value out = P(openai_to_gemini_request(R"({"model":"m","messages":[{"role":"user","content":"x"}]})"));
    EXPECT_EQ(out.find("generationConfig"), nullptr);     // omit when empty
}

TEST(GeminiReq, MalformedReturnsEmpty)
{
    EXPECT_TRUE(openai_to_gemini_request("nope").empty());
    EXPECT_TRUE(openai_to_gemini_request(R"(["x"])").empty());
}

TEST(GeminiResp, JoinsPartsAndMapsUsage)
{
    std::string in = R"({"candidates":[{"content":{"role":"model","parts":[
        {"text":"hello "},{"text":"world"}]},"finishReason":"STOP"}],
        "usageMetadata":{"promptTokenCount":8,"candidatesTokenCount":3,"totalTokenCount":11},
        "modelVersion":"gemini-2.0"})";
    Value out = P(gemini_to_openai_response(in));
    EXPECT_EQ(out.str_or("object"), "chat.completion");
    EXPECT_EQ(out.str_or("model"), "gemini-2.0");
    const Value* ch = out.find("choices");
    ASSERT_EQ(ch->arr.size(), 1u);
    EXPECT_EQ(ch->arr[0].find("message")->str_or("content"), "hello world");
    EXPECT_EQ(ch->arr[0].str_or("finish_reason"), "stop");
    const Value* u = out.find("usage");
    EXPECT_EQ(u->num_or("prompt_tokens"), "8");
    EXPECT_EQ(u->num_or("completion_tokens"), "3");
    EXPECT_EQ(u->num_or("total_tokens"), "11");
}

class GemFinish : public ::testing::TestWithParam<std::pair<const char*, const char*>> {};
TEST_P(GemFinish, MapsFinishReason)
{
    const auto& [gem, oai] = GetParam();
    std::string in = std::string(R"({"candidates":[{"content":{"parts":[{"text":"x"}]},"finishReason":")")
                     + gem + "\"}]}";
    Value out = P(gemini_to_openai_response(in));
    EXPECT_EQ(out.find("choices")->arr[0].str_or("finish_reason"), oai);
}
INSTANTIATE_TEST_SUITE_P(Cases, GemFinish,
                         ::testing::Values(std::make_pair("STOP", "stop"),
                                           std::make_pair("MAX_TOKENS", "length"),
                                           std::make_pair("SAFETY", "content_filter"),
                                           std::make_pair("RECITATION", "content_filter")),
                         [](const testing::TestParamInfo<std::pair<const char*, const char*>>& i) {
                             return std::string(i.param.first);
                         });

TEST(GeminiResp, TotalTokensFallsBackToSum)
{
    std::string in = R"({"candidates":[{"content":{"parts":[{"text":"x"}]},"finishReason":"STOP"}],
        "usageMetadata":{"promptTokenCount":5,"candidatesTokenCount":2}})";
    Value out = P(gemini_to_openai_response(in));
    EXPECT_EQ(out.find("usage")->num_or("total_tokens"), "7"); // no totalTokenCount -> sum
}

TEST(GeminiResp, MalformedReturnsEmpty)
{
    EXPECT_TRUE(gemini_to_openai_response("}{").empty());
}

// ════════════════════════════════════════════════════════════════════════════
// Cohere (Chat API v2)
// ════════════════════════════════════════════════════════════════════════════

TEST(CohereReq, KeepsMessagesAndRemapsTopP)
{
    std::string in = R"({"model":"command-r-plus","top_p":0.8,"max_tokens":128,"messages":[
        {"role":"system","content":"sys"},{"role":"user","content":"hi"}]})";
    Value out = P(openai_to_cohere_request(in));
    EXPECT_EQ(out.str_or("model"), "command-r-plus");
    EXPECT_EQ(out.num_or("max_tokens"), "128");
    EXPECT_EQ(out.num_or("p"), "0.8");                    // top_p -> p
    EXPECT_EQ(out.find("top_p"), nullptr);                // not passed through as top_p
    const Value* m = out.find("messages");
    ASSERT_EQ(m->arr.size(), 2u);                         // system stays a message in Cohere v2
    EXPECT_EQ(m->arr[0].str_or("role"), "system");
    EXPECT_EQ(m->arr[1].str_or("content"), "hi");
}

TEST(CohereReq, FlattensContentArray)
{
    std::string in = R"({"model":"m","messages":[
        {"role":"user","content":[{"type":"text","text":"foo"},{"type":"text","text":"bar"}]}]})";
    Value out = P(openai_to_cohere_request(in));
    EXPECT_EQ(out.find("messages")->arr[0].str_or("content"), "foobar");
}

TEST(CohereReq, MalformedReturnsEmpty)
{
    EXPECT_TRUE(openai_to_cohere_request("x").empty());
}

TEST(CohereResp, JoinsContentBlocksAndUsage)
{
    std::string in = R"({"id":"c1","message":{"role":"assistant","content":[
        {"type":"text","text":"po"},{"type":"text","text":"ng"}]},
        "finish_reason":"COMPLETE","usage":{"tokens":{"input_tokens":9,"output_tokens":2}}})";
    Value out = P(cohere_to_openai_response(in));
    EXPECT_EQ(out.str_or("id"), "c1");
    EXPECT_EQ(out.find("choices")->arr[0].find("message")->str_or("content"), "pong");
    EXPECT_EQ(out.find("choices")->arr[0].str_or("finish_reason"), "stop");
    const Value* u = out.find("usage");
    EXPECT_EQ(u->num_or("prompt_tokens"), "9");
    EXPECT_EQ(u->num_or("completion_tokens"), "2");
    EXPECT_EQ(u->num_or("total_tokens"), "11");
}

class CohFinish : public ::testing::TestWithParam<std::pair<const char*, const char*>> {};
TEST_P(CohFinish, MapsFinishReason)
{
    const auto& [coh, oai] = GetParam();
    std::string in = std::string(R"({"message":{"content":[{"type":"text","text":"x"}]},"finish_reason":")")
                     + coh + "\"}";
    Value out = P(cohere_to_openai_response(in));
    EXPECT_EQ(out.find("choices")->arr[0].str_or("finish_reason"), oai);
}
INSTANTIATE_TEST_SUITE_P(Cases, CohFinish,
                         ::testing::Values(std::make_pair("COMPLETE", "stop"),
                                           std::make_pair("MAX_TOKENS", "length"),
                                           std::make_pair("TOOL_CALL", "tool_calls")),
                         [](const testing::TestParamInfo<std::pair<const char*, const char*>>& i) {
                             return std::string(i.param.first);
                         });

TEST(CohereResp, MalformedReturnsEmpty)
{
    EXPECT_TRUE(cohere_to_openai_response("][").empty());
}

// ── round trips for the new dialects ────────────────────────────────────────
TEST(RoundTrip, GeminiRequestThenResponse)
{
    std::string oai = R"({"model":"gpt-4o","max_tokens":64,"messages":[
        {"role":"system","content":"sys"},{"role":"user","content":"ping"}]})";
    Value greq = P(openai_to_gemini_request(oai));
    EXPECT_EQ(greq.find("systemInstruction")->find("parts")->arr[0].str_or("text"), "sys");
    EXPECT_EQ(greq.find("contents")->arr[0].find("parts")->arr[0].str_or("text"), "ping");

    std::string gresp = R"({"candidates":[{"content":{"parts":[{"text":"pong"}]},"finishReason":"STOP"}],
        "usageMetadata":{"promptTokenCount":5,"candidatesTokenCount":1,"totalTokenCount":6}})";
    Value oai_resp = P(gemini_to_openai_response(gresp));
    EXPECT_EQ(oai_resp.find("choices")->arr[0].find("message")->str_or("content"), "pong");
    EXPECT_EQ(oai_resp.find("usage")->num_or("total_tokens"), "6");
}

TEST(RoundTrip, CohereRequestThenResponse)
{
    std::string oai = R"({"model":"command-r","messages":[{"role":"user","content":"ping"}]})";
    Value creq = P(openai_to_cohere_request(oai));
    EXPECT_EQ(creq.find("messages")->arr[0].str_or("content"), "ping");

    std::string cresp = R"({"message":{"content":[{"type":"text","text":"pong"}]},
        "finish_reason":"COMPLETE","usage":{"tokens":{"input_tokens":5,"output_tokens":1}}})";
    Value oai_resp = P(cohere_to_openai_response(cresp));
    EXPECT_EQ(oai_resp.find("choices")->arr[0].find("message")->str_or("content"), "pong");
    EXPECT_EQ(oai_resp.find("usage")->num_or("total_tokens"), "6");
}
