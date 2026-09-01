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
using llmbridge::provider::json::Value;
using llmbridge::provider::json::parse;

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

TEST(ReqXlate, PassesStreamTrueThrough)
{
    // stream:true must survive to the Anthropic request, or the upstream never
    // streams (and the gateway's SSE pump never engages).
    Value out = P(openai_to_anthropic_request(
        R"({"model":"m","stream":true,"messages":[{"role":"user","content":"hi"}]})"));
    const Value* s = out.find("stream");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->type, Value::Type::Bool);
    EXPECT_TRUE(s->boolean);
}

TEST(ReqXlate, OmitsStreamWhenAbsentOrFalse)
{
    // Absent -> no stream key (non-streaming request).
    EXPECT_EQ(P(openai_to_anthropic_request(
                  R"({"model":"m","messages":[{"role":"user","content":"hi"}]})"))
                  .find("stream"),
              nullptr);
    // Explicit false -> we only ever add stream when true.
    EXPECT_EQ(P(openai_to_anthropic_request(
                  R"({"model":"m","stream":false,"messages":[{"role":"user","content":"hi"}]})"))
                  .find("stream"),
              nullptr);
}

// ── stream_options.include_usage detection ──────────────────────────────────
//
// The flag is an output of the request translation, because that is where the body
// is parsed. It used to have a function of its own, which scanned the whole body and
// then parsed all of it a second time to answer what one key of an existing DOM
// answers: 796 us on a 287 KB request, measured on the reference server.
TEST(StreamUsageFlag, DetectsIncludeUsage)
{
    auto wants = [](std::string_view body) {
        bool wu = false;
        openai_to_anthropic_request(body, &wu);
        return wu;
    };
    EXPECT_TRUE(wants(
        R"({"model":"m","stream":true,"stream_options":{"include_usage":true},"messages":[]})"));
    EXPECT_FALSE(wants(
        R"({"model":"m","stream":true,"stream_options":{"include_usage":false},"messages":[]})"));
    EXPECT_FALSE(wants(R"({"model":"m","stream":true,"messages":[]})"));
    EXPECT_FALSE(wants(R"({"model":"m","stream_options":{},"messages":[]})"));
    // Not a bool -> not a request for usage (no coercion).
    EXPECT_FALSE(wants(R"({"stream_options":{"include_usage":"true"},"messages":[]})"));
    // Nested is not top-level: a tool schema may name anything it likes.
    EXPECT_FALSE(wants(
        R"({"model":"m","messages":[],"tools":[{"stream_options":{"include_usage":true}}]})"));
    // Garbage must not throw or crash, and leaves the caller's flag alone.
    EXPECT_FALSE(wants("include_usage but not json"));
    EXPECT_FALSE(wants(""));
}

// ── upstream error passthrough ──────────────────────────────────────────────
TEST(UpstreamError, MapsAnthropicErrorEnvelope)
{
    // Anthropic overloaded (529) / rate limit (429): the client must get the real
    // type + message so it can back off, not a generic gateway failure.
    Value out = P(llmbridge::provider::upstream_error_to_openai(
        R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})", "upstream_error"));
    const Value* e = out.find("error");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->str_or("type"), "overloaded_error");
    EXPECT_EQ(e->str_or("message"), "Overloaded");
}

TEST(UpstreamError, MapsOpenAiStyleAndGeminiStatus)
{
    Value a = P(llmbridge::provider::upstream_error_to_openai(
        R"({"error":{"message":"Rate limit reached","type":"rate_limit_error"}})", "upstream_error"));
    EXPECT_EQ(a.find("error")->str_or("type"), "rate_limit_error");
    EXPECT_EQ(a.find("error")->str_or("message"), "Rate limit reached");

    // Gemini-style uses "status" instead of "type".
    Value g = P(llmbridge::provider::upstream_error_to_openai(
        R"({"error":{"code":429,"message":"Quota exceeded","status":"RESOURCE_EXHAUSTED"}})", "upstream_error"));
    EXPECT_EQ(g.find("error")->str_or("type"), "RESOURCE_EXHAUSTED");
    EXPECT_EQ(g.find("error")->str_or("message"), "Quota exceeded");
}

TEST(UpstreamError, UnparseableBodyStillYieldsValidEnvelope)
{
    // Never empty: the caller must always be able to relay the upstream status.
    for (const char* body : {"", "not json at all", "<html>502</html>", "{}"})
    {
        Value out = P(llmbridge::provider::upstream_error_to_openai(body, "upstream_error"));
        const Value* e = out.find("error");
        ASSERT_NE(e, nullptr) << body;
        EXPECT_EQ(e->str_or("type"), "upstream_error") << body;
        EXPECT_FALSE(e->str_or("message").empty()) << body;
    }
}

TEST(UpstreamError, RawControlBytesFromUpstreamNeverReachTheEnvelope)
{
    // A hostile/broken provider puts a raw control byte in its error message.
    // Relaying it verbatim would make our error envelope invalid JSON for a strict
    // client. Since the parser was tightened (RFC 8259 §7) such a body does not
    // parse at all, so we emit the generic envelope instead of guessing at a
    // message inside malformed JSON, refusing to interpret beats sanitising and
    // forwarding. The invariant under test is unchanged and is the one that
    // matters: no raw control byte, and the envelope always parses.
    std::string body = "{\"error\":{\"type\":\"api_err\",\"message\":\"boom";
    body += char(0x0A);
    body += "next";
    body += char(0x01);
    body += "\"}}";
    const std::string out = llmbridge::provider::upstream_error_to_openai(body, "upstream_error");
    for (unsigned char c : out) EXPECT_GE(c, 0x20) << "raw control byte leaked into the envelope";
    Value v = P(out);
    ASSERT_NE(v.find("error"), nullptr) << "envelope must still be valid JSON";
    EXPECT_EQ(v.find("error")->str_or("type"), "upstream_error");
    EXPECT_EQ(v.find("error")->str_or("message"), "upstream provider error");
}

TEST(UpstreamError, WellFormedProviderErrorStillRelaysItsOwnMessage)
{
    // The case that actually happens: real providers emit valid JSON, and the
    // stricter parser must not cost us the provider's own type and message
    // that is the whole point of relaying a 429 instead of laundering it to 502.
    const std::string body =
        R"({"error":{"type":"rate_limit_error","message":"Number of requests has exceeded your rate limit"}})";
    const std::string out = llmbridge::provider::upstream_error_to_openai(body, "upstream_error");
    Value v = P(out);
    ASSERT_NE(v.find("error"), nullptr);
    EXPECT_EQ(v.find("error")->str_or("type"), "rate_limit_error");
    EXPECT_EQ(v.find("error")->str_or("message"),
              "Number of requests has exceeded your rate limit");
}

TEST(UpstreamError, EscapedControlCharsRelayAsEscaped)
{
    // Legal input: the control character is already escaped, so the body parses and
    // the span is forwarded still-escaped. Valid JSON in, valid JSON out.
    const std::string body =
        "{\"error\":{\"type\":\"t\",\"message\":\"line1\\nline2\"}}";
    const std::string out = llmbridge::provider::upstream_error_to_openai(body, "upstream_error");
    for (unsigned char c : out) EXPECT_GE(c, 0x20);
    Value v = P(out);
    ASSERT_NE(v.find("error"), nullptr);
    EXPECT_EQ(v.find("error")->str_or("type"), "t");
}

TEST(UpstreamError, PreservesEscapingInMessage)
{
    Value out = P(llmbridge::provider::upstream_error_to_openai(
        R"({"error":{"type":"invalid_request_error","message":"bad \"quote\" and \\ slash"}})", "x"));
    EXPECT_EQ(out.find("error")->str_or("message"), R"(bad \"quote\" and \\ slash)");
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
    // No cache read reported, so the details object is absent, byte-for-byte the
    // usage this translator has always produced.
    EXPECT_EQ(u->find("prompt_tokens_details"), nullptr);
}

TEST(RespXlate, CacheReadTokensBecomePromptTokensDetails)
{
    std::string in = R"({"content":[{"type":"text","text":"x"}],
        "usage":{"input_tokens":100,"output_tokens":2,"cache_read_input_tokens":80,
        "cache_creation_input_tokens":20}})";
    Value out = P(anthropic_to_openai_response(in));
    const Value* u = out.find("usage");
    ASSERT_NE(u, nullptr);
    // prompt_tokens is the whole prompt, OpenAI's convention: fresh 100 + read 80 +
    // write 20. Anthropic reports those three separately; a translated response must
    // sum them so it agrees with a byte-forwarded one.
    EXPECT_EQ(u->num_or("prompt_tokens"), "200");
    const Value* det = u->find("prompt_tokens_details");
    ASSERT_NE(det, nullptr);
    EXPECT_EQ(det->num_or("cached_tokens"), "80");
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

// ── Tool calling ────────────────────────────────────────────────────────────
// The three genuine conversions (declaration shape, arguments string <-> input
// object, tool result role) plus the failure modes. Every assertion re-parses the
// output instead of string-matching, so a shape change cannot pass by accident.

TEST(ToolReq, DeclarationBecomesAnthropicShape)
{
    const Value v = P(openai_to_anthropic_request(R"({"model":"m","max_tokens":8,
      "messages":[{"role":"user","content":"hi"}],
      "tools":[{"type":"function","function":{"name":"get_weather",
        "description":"Get weather","parameters":{"type":"object",
        "properties":{"city":{"type":"string"}},"required":["city"]}}}]})"));
    const Value* tools = v.find("tools");
    ASSERT_NE(tools, nullptr);
    ASSERT_TRUE(tools->is_array());
    ASSERT_EQ(tools->arr.size(), 1u);
    EXPECT_EQ(tools->arr[0].str_or("name"), "get_weather");
    EXPECT_EQ(tools->arr[0].str_or("description"), "Get weather");
    // OpenAI's `parameters` becomes Anthropic's `input_schema`.
    const Value* schema = tools->arr[0].find("input_schema");
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->str_or("type"), "object");
    EXPECT_EQ(v.find("tools")->arr[0].find("function"), nullptr) << "OpenAI wrapper leaked";
}

TEST(ToolReq, SchemaIsForwardedByteForByte)
{
    // A customer's JSON Schema is arbitrary. Rebuilding it from the DOM could change
    // number formatting, escapes or key order, so it must pass through verbatim.
    const std::string schema =
        R"({"type":"object","properties":{"n":{"type":"number","default":1.50},)"
        R"("s":{"type":"string","pattern":"^a\\/b$"},"e":{"enum":[1,"two",null]}},)"
        R"("required":["n"],"additionalProperties":false})";
    const std::string out = openai_to_anthropic_request(
        R"({"model":"m","max_tokens":8,"messages":[{"role":"user","content":"hi"}],)"
        R"("tools":[{"type":"function","function":{"name":"f","parameters":)" + schema + "}}]}");
    EXPECT_NE(out.find(schema), std::string::npos)
        << "schema was rewritten rather than forwarded:\n" << out;
}

TEST(ToolReq, ToolChoiceMapping)
{
    const auto choice = [](const char* tc) {
        std::string body = R"({"model":"m","max_tokens":8,"messages":[{"role":"user","content":"hi"}],)"
                           R"("tools":[{"type":"function","function":{"name":"f","parameters":{}}}])";
        if (tc && *tc) { body += ",\"tool_choice\":"; body += tc; }
        body += "}";
        return openai_to_anthropic_request(body);
    };
    EXPECT_NE(choice(R"("auto")").find(R"("tool_choice":{"type":"auto"})"), std::string::npos);
    EXPECT_NE(choice(R"("required")").find(R"("tool_choice":{"type":"any"})"), std::string::npos);
    EXPECT_NE(choice(R"({"type":"function","function":{"name":"f"}})")
                  .find(R"("tool_choice":{"type":"tool","name":"f"})"), std::string::npos);
    // "none" means do not call tools; Anthropic expresses that by having none.
    const std::string none = choice(R"("none")");
    EXPECT_EQ(none.find("\"tools\""), std::string::npos) << none;
    // Absent tool_choice: tools present, no choice emitted (provider default).
    const std::string absent = choice("");
    EXPECT_NE(absent.find("\"tools\""), std::string::npos);
    EXPECT_EQ(absent.find("tool_choice"), std::string::npos);
}

TEST(ToolReq, AssistantCallBecomesToolUseWithObjectInput)
{
    const Value v = P(openai_to_anthropic_request(R"({"model":"m","max_tokens":8,"messages":[
      {"role":"user","content":"weather?"},
      {"role":"assistant","content":null,"tool_calls":[
        {"id":"call_1","type":"function","function":{"name":"get_weather",
         "arguments":"{\"city\":\"Paris\",\"units\":\"c\"}"}}]}]})"));
    const Value* msgs = v.find("messages");
    ASSERT_NE(msgs, nullptr);
    ASSERT_EQ(msgs->arr.size(), 2u);
    const Value& asst = msgs->arr[1];
    EXPECT_EQ(asst.str_or("role"), "assistant");
    const Value* content = asst.find("content");
    ASSERT_TRUE(content && content->is_array());
    ASSERT_EQ(content->arr.size(), 1u);
    EXPECT_EQ(content->arr[0].str_or("type"), "tool_use");
    EXPECT_EQ(content->arr[0].str_or("id"), "call_1");
    EXPECT_EQ(content->arr[0].str_or("name"), "get_weather");
    // The crux: OpenAI's arguments string became a real object.
    const Value* input = content->arr[0].find("input");
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->is_object()) << "arguments string was not decoded to an object";
    EXPECT_EQ(input->str_or("city"), "Paris");
    EXPECT_EQ(input->str_or("units"), "c");
}

TEST(ToolReq, ParallelCallsAllSurvive)
{
    const Value v = P(openai_to_anthropic_request(R"({"model":"m","max_tokens":8,"messages":[
      {"role":"assistant","content":null,"tool_calls":[
        {"id":"a","type":"function","function":{"name":"f","arguments":"{\"x\":1}"}},
        {"id":"b","type":"function","function":{"name":"g","arguments":"{\"y\":2}"}},
        {"id":"c","type":"function","function":{"name":"h","arguments":"{}"}}]}]})"));
    const Value* content = v.find("messages")->arr[0].find("content");
    ASSERT_TRUE(content && content->is_array());
    ASSERT_EQ(content->arr.size(), 3u);
    EXPECT_EQ(content->arr[0].str_or("id"), "a");
    EXPECT_EQ(content->arr[2].str_or("id"), "c");
}

TEST(ToolReq, TextAndCallInOneAssistantTurn)
{
    const Value v = P(openai_to_anthropic_request(R"({"model":"m","max_tokens":8,"messages":[
      {"role":"assistant","content":"Let me look that up.","tool_calls":[
        {"id":"a","type":"function","function":{"name":"f","arguments":"{}"}}]}]})"));
    const Value* content = v.find("messages")->arr[0].find("content");
    ASSERT_TRUE(content && content->is_array());
    ASSERT_EQ(content->arr.size(), 2u);
    EXPECT_EQ(content->arr[0].str_or("type"), "text");
    EXPECT_EQ(content->arr[0].str_or("text"), "Let me look that up.");
    EXPECT_EQ(content->arr[1].str_or("type"), "tool_use");
}

TEST(ToolReq, ToolResultBecomesUserTurn)
{
    const Value v = P(openai_to_anthropic_request(R"({"model":"m","max_tokens":8,"messages":[
      {"role":"tool","tool_call_id":"call_1","content":"18C"}]})"));
    const Value& m = v.find("messages")->arr[0];
    EXPECT_EQ(m.str_or("role"), "user") << "tool results must ride a USER turn";
    const Value* content = m.find("content");
    ASSERT_TRUE(content && content->is_array());
    EXPECT_EQ(content->arr[0].str_or("type"), "tool_result");
    EXPECT_EQ(content->arr[0].str_or("tool_use_id"), "call_1");
    EXPECT_EQ(content->arr[0].str_or("content"), "18C");
}

TEST(ToolReq, ConsecutiveToolResultsMergeIntoOneTurn)
{
    // A parallel tool call yields several OpenAI tool messages that are semantically
    // One turn of results. (Anthropic tolerates consecutive user turns, measured,
    // so this is about emitting the canonical shape, not about avoiding an error.)
    const Value v = P(openai_to_anthropic_request(R"({"model":"m","max_tokens":8,"messages":[
      {"role":"tool","tool_call_id":"a","content":"1"},
      {"role":"tool","tool_call_id":"b","content":"2"},
      {"role":"tool","tool_call_id":"c","content":"3"},
      {"role":"user","content":"thanks"}]})"));
    const Value* msgs = v.find("messages");
    ASSERT_EQ(msgs->arr.size(), 2u) << "results did not merge into a single turn";
    EXPECT_EQ(msgs->arr[0].find("content")->arr.size(), 3u);
    EXPECT_EQ(msgs->arr[1].str_or("content"), "thanks"); // the trailing turn is intact
}

TEST(ToolReq, ToolResultTurnIsClosedBeforeAFollowingAssistantTurn)
{
    // Regression guard: the open "[" of a merged tool_result turn must be closed
    // when a non-tool message follows, or the JSON is malformed.
    const Value v = P(openai_to_anthropic_request(R"({"model":"m","max_tokens":8,"messages":[
      {"role":"tool","tool_call_id":"a","content":"1"},
      {"role":"assistant","content":"done"}]})"));
    ASSERT_EQ(v.find("messages")->arr.size(), 2u);
    EXPECT_EQ(v.find("messages")->arr[1].str_or("role"), "assistant");
}

TEST(ToolReq, MalformedToolPiecesAreDroppedNotGuessed)
{
    // A tool with no name, and a call with no function, are unusable. Emitting a
    // half-formed tool would make the provider fail in a way the client cannot read.
    const std::string out = openai_to_anthropic_request(R"({"model":"m","max_tokens":8,
      "messages":[{"role":"assistant","content":null,"tool_calls":[
        {"id":"x","type":"function"},
        {"id":"y","type":"function","function":{"name":"ok","arguments":"{}"}}]}],
      "tools":[{"type":"function","function":{"description":"no name here"}},
               {"type":"function","function":{"name":"good","parameters":{}}}]})");
    const Value v = P(out);
    ASSERT_EQ(v.find("tools")->arr.size(), 1u);
    EXPECT_EQ(v.find("tools")->arr[0].str_or("name"), "good");
    const Value* content = v.find("messages")->arr[0].find("content");
    ASSERT_EQ(content->arr.size(), 1u);
    EXPECT_EQ(content->arr[0].str_or("name"), "ok");
}

TEST(ToolReq, EmptyAndAbsentArgumentsBecomeEmptyObject)
{
    const Value v = P(openai_to_anthropic_request(R"({"model":"m","max_tokens":8,"messages":[
      {"role":"assistant","content":null,"tool_calls":[
        {"id":"a","type":"function","function":{"name":"f","arguments":""}},
        {"id":"b","type":"function","function":{"name":"g"}}]}]})"));
    const Value* content = v.find("messages")->arr[0].find("content");
    ASSERT_EQ(content->arr.size(), 2u);
    for (const auto& blk : content->arr)
    {
        const Value* in = blk.find("input");
        ASSERT_NE(in, nullptr);
        EXPECT_TRUE(in->is_object()) << "must be {}, never a bare string or null";
        EXPECT_TRUE(in->obj.empty());
    }
}

TEST(ToolReq, NoToolsKeyWhenNoneDeclared)
{
    const std::string out = openai_to_anthropic_request(
        R"({"model":"m","max_tokens":8,"messages":[{"role":"user","content":"hi"}]})");
    EXPECT_EQ(out.find("\"tools\""), std::string::npos) << out;
    EXPECT_EQ(out.find("tool_choice"), std::string::npos) << out;
}

TEST(ToolReq, EmptyToolsArrayEmitsNothing)
{
    const std::string out = openai_to_anthropic_request(
        R"({"model":"m","max_tokens":8,"messages":[{"role":"user","content":"hi"}],"tools":[]})");
    EXPECT_EQ(out.find("\"tools\""), std::string::npos) << out;
}

// ── response: Anthropic -> OpenAI ───────────────────────────────────────────

TEST(ToolResp, ToolUseBecomesToolCallsWithStringArguments)
{
    const Value v = P(anthropic_to_openai_response(R"({"id":"msg_1","model":"claude",
      "stop_reason":"tool_use","content":[
        {"type":"tool_use","id":"toolu_1","name":"get_weather","input":{"city":"Paris"}}],
      "usage":{"input_tokens":10,"output_tokens":20}})"));
    const Value& msg = *v.find("choices")->arr[0].find("message");
    const Value* calls = msg.find("tool_calls");
    ASSERT_NE(calls, nullptr);
    ASSERT_EQ(calls->arr.size(), 1u);
    EXPECT_EQ(calls->arr[0].str_or("id"), "toolu_1");
    EXPECT_EQ(calls->arr[0].str_or("type"), "function");
    const Value* fn = calls->arr[0].find("function");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->str_or("name"), "get_weather");
    // arguments is a string containing JSON, so decoding it must yield the input.
    const Value* args = fn->find("arguments");
    ASSERT_NE(args, nullptr);
    ASSERT_TRUE(args->is_string()) << "arguments must be a string, not an object";
    const std::string decoded = llmbridge::provider::json::unescape_string(args->sv);
    EXPECT_EQ(decoded, R"({"city":"Paris"})");
    EXPECT_EQ(v.find("choices")->arr[0].str_or("finish_reason"), "tool_calls");
}

TEST(ToolResp, ContentIsNullNotEmptyStringOnPureToolCall)
{
    // OpenAI SDKs branch on content === null. An empty string reads as "the model
    // answered nothing" instead of "the model called a tool".
    const std::string out = anthropic_to_openai_response(R"({"id":"m","model":"c",
      "stop_reason":"tool_use","content":[{"type":"tool_use","id":"t","name":"f","input":{}}],
      "usage":{"input_tokens":1,"output_tokens":1}})");
    EXPECT_NE(out.find(R"("content":null)"), std::string::npos) << out;
    EXPECT_EQ(out.find(R"("content":"")"), std::string::npos) << out;
}

TEST(ToolResp, TextAndToolCallCoexist)
{
    const Value v = P(anthropic_to_openai_response(R"({"id":"m","model":"c",
      "stop_reason":"tool_use","content":[
        {"type":"text","text":"Checking."},
        {"type":"tool_use","id":"t1","name":"f","input":{"a":1}}],
      "usage":{"input_tokens":1,"output_tokens":2}})"));
    const Value& msg = *v.find("choices")->arr[0].find("message");
    EXPECT_EQ(msg.str_or("content"), "Checking.");
    ASSERT_NE(msg.find("tool_calls"), nullptr);
    EXPECT_EQ(msg.find("tool_calls")->arr.size(), 1u);
}

TEST(ToolResp, NoToolCallsKeyOnAPlainAnswer)
{
    const std::string out = anthropic_to_openai_response(R"({"id":"m","model":"c",
      "stop_reason":"end_turn","content":[{"type":"text","text":"hello"}],
      "usage":{"input_tokens":1,"output_tokens":1}})");
    EXPECT_EQ(out.find("tool_calls"), std::string::npos) << out;
    EXPECT_NE(out.find(R"("content":"hello")"), std::string::npos) << out;
}

TEST(ToolResp, ParallelToolUseBlocks)
{
    const Value v = P(anthropic_to_openai_response(R"({"id":"m","model":"c",
      "stop_reason":"tool_use","content":[
        {"type":"tool_use","id":"t1","name":"f","input":{"a":1}},
        {"type":"tool_use","id":"t2","name":"g","input":{"b":2}}],
      "usage":{"input_tokens":1,"output_tokens":2}})"));
    const Value* calls = v.find("choices")->arr[0].find("message")->find("tool_calls");
    ASSERT_EQ(calls->arr.size(), 2u);
    EXPECT_EQ(calls->arr[1].str_or("id"), "t2");
    EXPECT_EQ(calls->arr[1].find("function")->str_or("name"), "g");
}

TEST(ToolRoundTrip, ArgumentsSurviveBothDirections)
{
    // The round trip that matters in an agent loop: Anthropic emits input (object),
    // we hand the client arguments (string), the client sends it back, and it must
    // arrive at Anthropic as the same object. Includes escaping hazards.
    const std::string anth = R"({"id":"m","model":"c","stop_reason":"tool_use","content":[
      {"type":"tool_use","id":"t1","name":"f","input":{"q":"say \"hi\"\nnow","path":"a/b\\c","n":-1.5e3,"u":"café"}}],
      "usage":{"input_tokens":1,"output_tokens":1}})";
    const Value resp = P(anthropic_to_openai_response(anth));
    const Value* args =
        resp.find("choices")->arr[0].find("message")->find("tool_calls")->arr[0].find("function")->find("arguments");
    ASSERT_NE(args, nullptr);

    // Feed those exact arguments back through the request path.
    std::string back = R"({"model":"m","max_tokens":8,"messages":[{"role":"assistant","content":null,)"
                       R"("tool_calls":[{"id":"t1","type":"function","function":{"name":"f","arguments":")";
    back.append(args->sv); // still-escaped span, exactly as a client would echo it
    back += R"("}}]}]})";
    const Value req = P(openai_to_anthropic_request(back));
    const Value* input = req.find("messages")->arr[0].find("content")->arr[0].find("input");
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->is_object());
    EXPECT_EQ(llmbridge::provider::json::unescape_string(input->str_or("q")), "say \"hi\"\nnow");
    EXPECT_EQ(llmbridge::provider::json::unescape_string(input->str_or("path")), "a/b\\c");
    EXPECT_EQ(input->num_or("n"), "-1.5e3");
    EXPECT_EQ(llmbridge::provider::json::unescape_string(input->str_or("u")), "café");
}

// ── Bedrock: the same Messages body, minus the model, plus the version ─────────

TEST(BedrockRequest, DropsModelAndCarriesAnthropicVersion)
{
    // Bedrock takes the model id in the path, so a `model` in the body is not merely
    // redundant: it is a field the endpoint does not accept.
    std::string model;
    const std::string out = llmbridge::provider::openai_to_bedrock_request(
        R"({"model":"anthropic.claude-3-5-sonnet-20240620-v1:0",
            "max_tokens":512,"messages":[{"role":"user","content":"hi"}]})", model);

    EXPECT_EQ(model, "anthropic.claude-3-5-sonnet-20240620-v1:0");
    EXPECT_EQ(out.find("\"model\""), std::string::npos) << out;
    EXPECT_NE(out.find(R"("anthropic_version":"bedrock-2023-05-31")"), std::string::npos)
        << out;
    EXPECT_NE(out.find(R"("max_tokens":512)"), std::string::npos) << out;
    EXPECT_NE(out.find(R"("messages":)"), std::string::npos) << out;
}

TEST(BedrockRequest, DiffersFromAnthropicInExactlyTwoFields)
{
    // The guard on sharing one message walk between the two dialects. If a change to
    // tool results, vision or system prompts lands on one and not the other, this is
    // what catches it, because everything except those two fields must match byte for
    // byte.
    constexpr std::string_view kIn = R"({
        "model":"claude-3-5-sonnet-latest","max_tokens":64,"temperature":0.5,
        "messages":[{"role":"system","content":"be brief"},
                    {"role":"user","content":"hi"},
                    {"role":"assistant","content":"hello"}]})";
    std::string model;
    std::string a = llmbridge::provider::openai_to_anthropic_request(kIn);
    std::string b = llmbridge::provider::openai_to_bedrock_request(kIn, model);
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());

    const std::string a_head = R"({"model":"claude-3-5-sonnet-latest")";
    const std::string b_head = R"({"anthropic_version":"bedrock-2023-05-31")";
    ASSERT_EQ(a.compare(0, a_head.size(), a_head), 0) << a;
    ASSERT_EQ(b.compare(0, b_head.size(), b_head), 0) << b;
    EXPECT_EQ(a.substr(a_head.size()), b.substr(b_head.size()))
        << "the two bodies diverge somewhere other than the model and the version";
}

TEST(BedrockRequest, StreamingAndToolsSurviveTheSharedPath)
{
    std::string model;
    const std::string out = llmbridge::provider::openai_to_bedrock_request(
        R"({"model":"m","stream":true,"messages":[{"role":"user","content":"x"}],
            "tools":[{"type":"function","function":{"name":"f","parameters":{}}}]})",
        model);
    EXPECT_NE(out.find(R"("stream":true)"), std::string::npos) << out;
    EXPECT_NE(out.find(R"("tools":)"), std::string::npos) << out;
}

TEST(BedrockRequest, RefusesWhatAnthropicRefuses)
{
    std::string model = "leftover";
    EXPECT_TRUE(llmbridge::provider::openai_to_bedrock_request("not json", model).empty());
    EXPECT_TRUE(llmbridge::provider::openai_to_bedrock_request("[]", model).empty());
}

// ── Model rewriting: a splice, not a re-serialisation ─────────────────────────

TEST(RewriteModel, ReplacesTheValueAndLeavesEveryOtherByteAlone)
{
    const std::string in =
        R"({"model":"gpt-4o","messages":[{"role":"user","content":"hi"}],"seed":7})";
    const std::string out = llmbridge::provider::rewrite_model(in, "llama-3.3-70b");
    EXPECT_EQ(out,
              R"({"model":"llama-3.3-70b","messages":[{"role":"user","content":"hi"}],"seed":7})");
}

TEST(RewriteModel, FieldsWeDoNotModelSurvive)
{
    // The whole reason this is a splice. A re-serialisation would drop anything the
    // parser does not know, and providers add parameters faster than we adopt them.
    const std::string in =
        R"({"model":"a","some_future_field":{"nested":[1,2,3]},"logit_bias":{"1":-5},)"
        R"("messages":[]})";
    const std::string out = llmbridge::provider::rewrite_model(in, "b");
    EXPECT_NE(out.find(R"("some_future_field":{"nested":[1,2,3]})"), std::string::npos) << out;
    EXPECT_NE(out.find(R"("logit_bias":{"1":-5})"), std::string::npos) << out;
    EXPECT_EQ(out.size(), in.size()) << out;  // "a" and "b" are the same length
}

TEST(RewriteModel, AModelMentionedInsideAPromptIsNotTouched)
{
    // The failure that would corrupt a customer's prompt. A naive search for
    // "model":" would hit the message content first.
    const std::string in =
        R"({"messages":[{"role":"user","content":"the \"model\":\"gpt-4o\" field"}],)"
        R"("model":"gpt-4o"})";
    const std::string out = llmbridge::provider::rewrite_model(in, "claude-haiku-4-5");
    EXPECT_NE(out.find(R"(the \"model\":\"gpt-4o\" field)"), std::string::npos) << out;
    EXPECT_NE(out.find(R"("model":"claude-haiku-4-5")"), std::string::npos) << out;
    // Exactly one replacement.
    EXPECT_EQ(out.find("claude-haiku-4-5"), out.rfind("claude-haiku-4-5")) << out;
}

TEST(RewriteModel, RefusesRatherThanEditingPartially)
{
    using llmbridge::provider::rewrite_model;
    const std::string ok = R"({"model":"a","messages":[]})";
    EXPECT_TRUE(rewrite_model("not json", "b").empty());
    EXPECT_TRUE(rewrite_model("[]", "b").empty());
    EXPECT_TRUE(rewrite_model(R"({"messages":[]})", "b").empty()) << "no model key";
    EXPECT_TRUE(rewrite_model(R"({"model":7,"messages":[]})", "b").empty()) << "not a string";
    EXPECT_TRUE(rewrite_model(ok, "").empty()) << "empty replacement";
    // Anything needing escaping is refused: a model id never contains these, and
    // guessing at the escaping of a string that lands in a request body is an
    // injection waiting to happen.
    EXPECT_TRUE(rewrite_model(ok, "a\"b").empty());
    EXPECT_TRUE(rewrite_model(ok, "a\\b").empty());
    EXPECT_TRUE(rewrite_model(ok, "a\nb").empty());
}

TEST(RewriteModel, TheResultIsStillParseableAndCarriesTheNewModel)
{
    // Round trip through the parser, so the edit cannot leave the body syntactically
    // valid-looking but structurally wrong.
    const std::string in =
        R"({"model":"anthropic.claude-haiku-4-5","max_tokens":48,"messages":[]})";
    const std::string out = llmbridge::provider::rewrite_model(
        in, "us.anthropic.claude-haiku-4-5-20251001-v1:0");
    bool ok = false;
    const auto v = llmbridge::provider::json::parse(out, ok);
    ASSERT_TRUE(ok) << out;
    EXPECT_EQ(v.str_or("model"), "us.anthropic.claude-haiku-4-5-20251001-v1:0");
    EXPECT_EQ(v.num_or("max_tokens"), "48");
}

// ── `arguments` is client-controlled, so it is parsed before it is spliced ────
//
// The value arrives as a JSON *string* and leaves as a JSON *object*, which means
// its bytes are written into a body we construct. Appending them raw let the client
// write the body instead of us.
namespace
{
    std::string with_arguments(const std::string& escaped_args)
    {
        return R"({"model":"m","max_tokens":8,"messages":[
          {"role":"assistant","content":null,"tool_calls":[
            {"id":"call_1","type":"function","function":{"name":"f","arguments":")" +
               escaped_args + R"("}}]}]})";
    }
} // namespace

TEST(ToolReqInjection, ArgumentsThatCloseOurObjectAreRefused)
{
    // Closes the tool_use object, the content array and the message, then appends
    // members of its own. Before the fix this produced a valid Anthropic body whose
    // top-level keys were model, max_tokens, messages, model, messages: the last
    // model and messages are the caller's, appended after ours.
    const std::string escaped =
        R"({}}]},{\"role\":\"user\",\"content\":\"x\"}],\"model\":\"attacker-model\",\"messages\":[{)";
    EXPECT_TRUE(openai_to_anthropic_request(with_arguments(escaped)).empty())
        << "a caller wrote members into a body llmbridge constructed";
}

TEST(ToolReqInjection, ArgumentsThatAreNotJsonAreRefusedInsteadOfForwarded)
{
    // `"input":not json` is a malformed body that we would then send upstream. Fail
    // closed: refuse the request, do not hand a provider something we broke.
    EXPECT_TRUE(openai_to_anthropic_request(with_arguments("not json")).empty());
    EXPECT_TRUE(openai_to_anthropic_request(with_arguments("[1,2,3]")).empty())
        << "an array is not an input object";
    EXPECT_TRUE(openai_to_anthropic_request(with_arguments("42")).empty());
}

TEST(ToolReqInjection, OrdinaryArgumentsStillTranslate)
{
    // The control. Refusing everything would pass the two tests above and break the
    // agent loop, which is the feature this whole path exists for.
    const Value v = P(openai_to_anthropic_request(
        with_arguments(R"({\"city\":\"Paris\"})")));
    const Value* input = v.find("messages")->arr[0].find("content")->arr[0].find("input");
    ASSERT_TRUE(input && input->is_object());
    EXPECT_EQ(input->str_or("city"), "Paris");
    // An empty arguments string is the documented "no parameters" case and becomes {}.
    const Value e = P(openai_to_anthropic_request(with_arguments("")));
    const Value* empty_input = e.find("messages")->arr[0].find("content")->arr[0].find("input");
    ASSERT_TRUE(empty_input && empty_input->is_object());
    EXPECT_TRUE(empty_input->obj.empty());
}

// ── model_of: the client's model, from the top level only ────────────────────
TEST(ModelOf, ReadsATopLevelModel)
{
    EXPECT_EQ(llmbridge::provider::model_of(R"({"model":"claude-opus-5","max_tokens":8})"),
              "claude-opus-5");
    EXPECT_EQ(llmbridge::provider::model_of("{ \"model\" : \"gpt-4o\" }"), "gpt-4o");
}

TEST(ModelOf, ReadsItAfterOtherKeys)
{
    // Claude Code puts model first; nothing requires that, and a scanner that only
    // works for one client is a scanner that silently mislabels the others.
    EXPECT_EQ(llmbridge::provider::model_of(
                  R"({"max_tokens":8,"stream":true,"messages":[{"role":"user",)"
                  R"("content":"hi"}],"model":"claude-haiku-4-5"})"),
              "claude-haiku-4-5");
}

TEST(ModelOf, ANestedModelKeyIsNotTheModel)
{
    // The reason this is a parser and not a find(). A quote inside a JSON string is
    // escaped, so prompt text cannot forge a key; a nested key is unescaped and can.
    // Anything the client nests, metadata, a tool schema, a provider block, carries
    // real `"model"` keys, and the first textual match is whichever comes first in
    // the body, not whichever is the request's own.
    EXPECT_EQ(llmbridge::provider::model_of(
                  R"({"metadata":{"model":"decoy"},"model":"claude-opus-5"})"),
              "claude-opus-5");
    EXPECT_EQ(llmbridge::provider::model_of(
                  R"({"tools":[{"input_schema":{"properties":{"model":{"type":"string"}}}}],)"
                  R"("model":"claude-haiku-4-5"})"),
              "claude-haiku-4-5");
    EXPECT_TRUE(llmbridge::provider::model_of(R"({"metadata":{"model":"decoy"}})").empty())
        << "a nested key is not the request's model";
}

TEST(ModelOf, AModelInsideAPromptIsText)
{
    EXPECT_EQ(llmbridge::provider::model_of(
                  R"({"messages":[{"role":"user","content":"use {\"model\":\"evil\"} here"}],)"
                  R"("model":"claude-opus-5"})"),
              "claude-opus-5");
    EXPECT_TRUE(llmbridge::provider::model_of(
                    R"({"messages":[{"role":"user","content":"\"model\": \"evil\""}]})")
                    .empty());
}

TEST(ModelOf, RefusesWhatItCannotCompare)
{
    EXPECT_TRUE(llmbridge::provider::model_of("").empty());
    EXPECT_TRUE(llmbridge::provider::model_of("[]").empty());
    EXPECT_TRUE(llmbridge::provider::model_of(R"({"model":7})").empty());
    EXPECT_TRUE(llmbridge::provider::model_of(R"({"model":null})").empty());
    EXPECT_TRUE(llmbridge::provider::model_of(R"({"model":"a\\b"})").empty())
        << "an escaped name is not one we can compare against a configured string";
    EXPECT_TRUE(llmbridge::provider::model_of(R"({"messages":[{"a":1},)").empty())
        << "a truncated body must not yield a model";
}

TEST(ModelOf, SkipsEveryValueShape)
{
    EXPECT_EQ(llmbridge::provider::model_of(
                  R"({"a":{"b":[1,2,{"c":"}"}]},"b":[[]],"c":null,"d":true,"e":-1.5e3,)"
                  R"("f":"a\"b","model":"m"})"),
              "m");
}

TEST(WantsStream, OnlyATopLevelTrueCounts)
{
    using llmbridge::provider::wants_stream;
    EXPECT_TRUE(wants_stream(R"({"model":"m","messages":[],"stream":true})"));
    EXPECT_FALSE(wants_stream(R"({"model":"m","stream":false})"));
    EXPECT_FALSE(wants_stream(R"({"model":"m","messages":[]})"));
    // The cheap reject must not change the answer for a nested key, which is the
    // whole reason this is a parser: a tool schema with a `stream` property is not
    // the request asking to stream.
    EXPECT_FALSE(wants_stream(R"({"tools":[{"input_schema":{"properties":{"stream":true}}}]})"));
    EXPECT_TRUE(wants_stream(R"({"tools":[{"input_schema":{"properties":{"stream":true}}}],"stream":true})"));
}

// ── cache_control ───────────────────────────────────────────────────────────
//
// Anthropic caches what precedes a block marked `cache_control`, and nothing at all
// without one: an identical prompt sent a hundred times is a hundred full-price
// prompts. The translator used to flatten every content part into one string, which
// carried the text and dropped the breakpoint, so an OpenAI-dialect client could not
// reach the discount at all while an Anthropic client got it by byte-forward. Cached
// reads are a tenth of the input price, so on an agent loop that is the difference
// between the two paths.

TEST(CacheControl, ContentWithABreakpointBecomesBlocks)
{
    Value out = P(openai_to_anthropic_request(R"({"model":"m","messages":[
        {"role":"user","content":[
            {"type":"text","text":"the long prefix","cache_control":{"type":"ephemeral"}},
            {"type":"text","text":"the tail"}]}]})"));
    const Value* msgs = out.find("messages");
    ASSERT_NE(msgs, nullptr);
    ASSERT_EQ(msgs->arr.size(), 1u);
    const Value* content = msgs->arr[0].find("content");
    ASSERT_NE(content, nullptr);
    ASSERT_TRUE(content->is_array()) << "a breakpoint cannot survive the string form";
    ASSERT_EQ(content->arr.size(), 2u) << "one block per part, so the mark keeps its place";
    EXPECT_EQ(content->arr[0].str_or("text"), "the long prefix");
    const Value* cc = content->arr[0].find("cache_control");
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->str_or("type"), "ephemeral");
    // The unmarked part must not acquire one: which block carries it is the meaning.
    EXPECT_EQ(content->arr[1].find("cache_control"), nullptr);
    EXPECT_EQ(content->arr[1].str_or("text"), "the tail");
}

TEST(CacheControl, ContentWithoutOneKeepsTheStringForm)
{
    // The shape every request that does not ask for caching still gets. Asserted so a
    // future change cannot quietly turn every message into blocks, which would be a
    // different request to send to a provider for no reason.
    Value out = P(openai_to_anthropic_request(R"({"model":"m","messages":[
        {"role":"user","content":[{"type":"text","text":"a"},{"type":"text","text":"b"}]}]})"));
    const Value* content = out.find("messages")->arr[0].find("content");
    ASSERT_NE(content, nullptr);
    EXPECT_TRUE(content->is_string()) << "parts with no breakpoint still flatten";
    EXPECT_EQ(content->sv, "ab");
}

TEST(CacheControl, SystemWithABreakpointBecomesABlockArray)
{
    Value out = P(openai_to_anthropic_request(R"({"model":"m","messages":[
        {"role":"system","content":[
            {"type":"text","text":"you are terse","cache_control":{"type":"ephemeral"}}]},
        {"role":"user","content":"hi"}]})"));
    const Value* sys = out.find("system");
    ASSERT_NE(sys, nullptr);
    ASSERT_TRUE(sys->is_array()) << "Anthropic only carries a breakpoint in the array form";
    ASSERT_EQ(sys->arr.size(), 1u);
    EXPECT_EQ(sys->arr[0].str_or("text"), "you are terse");
    EXPECT_EQ(sys->arr[0].find("cache_control")->str_or("type"), "ephemeral");
}

TEST(CacheControl, SystemWithoutOneStaysAString)
{
    Value out = P(openai_to_anthropic_request(
        R"({"model":"m","messages":[{"role":"system","content":"be terse"},
                                    {"role":"user","content":"hi"}]})"));
    EXPECT_EQ(out.find("system")->sv, "be terse");
    EXPECT_TRUE(out.find("system")->is_string());
}

TEST(CacheControl, ATollDefinitionCanCarryOne)
{
    // Long, identical every turn, and ahead of the conversation: the other prefix
    // worth caching. Accepted on the tool object or on the function inside it,
    // because clients write it both ways.
    for (const char* body : {
        R"({"model":"m","messages":[],"tools":[{"type":"function","cache_control":{"type":"ephemeral"},
             "function":{"name":"f","parameters":{"type":"object"}}}]})",
        R"({"model":"m","messages":[],"tools":[{"type":"function",
             "function":{"name":"f","cache_control":{"type":"ephemeral"},"parameters":{"type":"object"}}}]})"})
    {
        Value out = P(openai_to_anthropic_request(body));
        const Value* tools = out.find("tools");
        ASSERT_NE(tools, nullptr) << body;
        ASSERT_EQ(tools->arr.size(), 1u);
        const Value* cc = tools->arr[0].find("cache_control");
        ASSERT_NE(cc, nullptr) << "the breakpoint was dropped: " << body;
        EXPECT_EQ(cc->str_or("type"), "ephemeral");
    }
}

TEST(CacheControl, AMalformedBreakpointIsRefused)
{
    // Fail closed. Forwarding it would make the provider reject the request, and the
    // caller would read a 400 from Anthropic about a field they did not know we
    // touched. The schema of the object is the provider's business; whether it is an
    // object at all is ours.
    EXPECT_TRUE(openai_to_anthropic_request(R"({"model":"m","messages":[
        {"role":"user","content":[{"type":"text","text":"x","cache_control":"ephemeral"}]}]})").empty());
    EXPECT_TRUE(openai_to_anthropic_request(R"({"model":"m","messages":[
        {"role":"system","content":[{"type":"text","text":"x","cache_control":7}]},
        {"role":"user","content":"hi"}]})").empty());
}

TEST(CacheControl, ANonTextPartIsStillRefusedInTheBlockForm)
{
    // The block path must refuse exactly what the flattening path refuses: an image
    // dropped from a cached request is the same silent edit, cached or not.
    EXPECT_TRUE(openai_to_anthropic_request(R"({"model":"m","messages":[
        {"role":"user","content":[
            {"type":"text","text":"x","cache_control":{"type":"ephemeral"}},
            {"type":"image_url","image_url":{"url":"http://x/y.png"}}]}]})").empty());
}

// ── upsert_string: the route setting a field the caller usually omits ──────────
// rewrite_model can only replace, because a request with no model is not one this
// gateway serves. A service tier is the opposite: almost nobody sends one, and the
// route has to be able to put it there. Same splice, one more case, and it sits on a
// body a credential travels with, so the refusals matter as much as the successes.
namespace
{
    std::string upsert(std::string_view body, std::string_view k, std::string_view v,
                       std::string_view* had = nullptr)
    {
        return llmbridge::provider::upsert_string(body, k, v, had);
    }
} // namespace

TEST(UpsertString, InsertsWhenTheKeyIsAbsent)
{
    std::string_view had = "x";
    const std::string out = upsert(R"({"model":"m","max_tokens":8})",
                                   "service_tier", "flex", &had);
    EXPECT_EQ(out, R"({"service_tier":"flex","model":"m","max_tokens":8})");
    EXPECT_TRUE(had.empty()) << "absent must be reported as absent, not as a value";
}

TEST(UpsertString, ReplacesWhenTheKeyIsPresentAndReportsWhatItWas)
{
    std::string_view had;
    const std::string out = upsert(R"({"model":"m","service_tier":"priority"})",
                                   "service_tier", "flex", &had);
    EXPECT_EQ(out, R"({"model":"m","service_tier":"flex"})");
    EXPECT_EQ(had, "priority") << "the caller's own value is what a disagreement is";
}

// The reason this is a splice and not a re-serialisation: a field the parser does not
// model must survive untouched.
TEST(UpsertString, LeavesEveryOtherFieldExactlyAsItWas)
{
    const std::string in =
        R"({"model":"m","future_field":{"a":[1,2,{"b":null}]},"stream":true})";
    const std::string out = upsert(in, "service_tier", "flex");
    EXPECT_NE(out.find(R"("future_field":{"a":[1,2,{"b":null}]})"), std::string::npos);
    EXPECT_NE(out.find(R"("stream":true)"), std::string::npos);
}

// The whole point of the top-level rule: a prompt that talks about the key is text.
TEST(UpsertString, DoesNotTouchTheKeyInsideAMessage)
{
    const std::string in =
        R"({"model":"m","messages":[{"role":"user","content":"what is \"service_tier\":\"priority\"?"}]})";
    std::string_view had = "x";
    const std::string out = upsert(in, "service_tier", "flex", &had);
    EXPECT_TRUE(had.empty()) << "the string in the prompt is not the caller's tier";
    EXPECT_NE(out.find(R"(\"service_tier\":\"priority\")"), std::string::npos)
        << "the prompt must reach the venue unchanged";
    EXPECT_EQ(out.find(R"({"service_tier":"flex",)"), 0u);
}

TEST(UpsertString, AnEmptyObjectTakesNoComma)
{
    EXPECT_EQ(upsert("{}", "service_tier", "flex"), R"({"service_tier":"flex"})");
}

TEST(UpsertString, WhitespaceBeforeTheFirstMemberIsPreserved)
{
    const std::string out = upsert("{  \"model\":\"m\" }", "service_tier", "flex");
    EXPECT_EQ(out, "{\"service_tier\":\"flex\",  \"model\":\"m\" }");
    bool ok = false;
    llmbridge::provider::json::parse(out, ok);
    EXPECT_TRUE(ok) << "and the result still parses";
}

// Present but not a string. Replacing the span with a quoted value would produce a
// body neither side meant, so this refuses instead of guessing.
TEST(UpsertString, RefusesWhenTheKeyIsPresentAndNotAString)
{
    EXPECT_TRUE(upsert(R"({"model":"m","service_tier":3})", "service_tier", "flex").empty());
    EXPECT_TRUE(upsert(R"({"model":"m","service_tier":null})", "service_tier", "flex").empty());
    EXPECT_TRUE(upsert(R"({"model":"m","service_tier":{"a":1}})", "service_tier", "flex").empty());
}

// A value needing an escape is a caller speaking a schema we do not model. Guessing at
// the escaping of a string that lands in a request body is how an injection starts.
TEST(UpsertString, RefusesAValueOrKeyNeedingEscapes)
{
    EXPECT_TRUE(upsert(R"({"model":"m"})", "service_tier", "fl\"ex").empty());
    EXPECT_TRUE(upsert(R"({"model":"m"})", "service_tier", "fl\\ex").empty());
    EXPECT_TRUE(upsert(R"({"model":"m"})", "service_tier", std::string("a\x01b")).empty());
    EXPECT_TRUE(upsert(R"({"model":"m"})", "ser\"vice", "flex").empty());
}

TEST(UpsertString, RefusesWhatIsNotAnObject)
{
    EXPECT_TRUE(upsert("[1,2,3]", "service_tier", "flex").empty());
    EXPECT_TRUE(upsert("\"a string\"", "service_tier", "flex").empty());
    EXPECT_TRUE(upsert("not json", "service_tier", "flex").empty());
    EXPECT_TRUE(upsert("", "service_tier", "flex").empty());
}

TEST(UpsertString, RefusesAnEmptyKeyOrValue)
{
    EXPECT_TRUE(upsert(R"({"model":"m"})", "", "flex").empty());
    EXPECT_TRUE(upsert(R"({"model":"m"})", "service_tier", "").empty());
}

// Every accepted result has to be a body a venue can parse, whatever shape went in.
TEST(UpsertString, EveryAcceptedResultStillParses)
{
    const char* bodies[] = {
        R"({})", R"({"model":"m"})", R"({"a":1,"b":[1,2],"c":{"d":"e"}})",
        R"({ "model" : "m" })", R"({"service_tier":"priority","model":"m"})",
        R"({"model":"m","service_tier":"flex"})",
    };
    for (const char* b : bodies)
    {
        const std::string out = upsert(b, "service_tier", "flex");
        ASSERT_FALSE(out.empty()) << b;
        bool ok = false;
        const auto v = llmbridge::provider::json::parse(out, ok);
        EXPECT_TRUE(ok) << b << " -> " << out;
        ASSERT_TRUE(v.is_object());
        const auto* got = v.find("service_tier");
        ASSERT_NE(got, nullptr) << out;
        EXPECT_EQ(got->sv, "flex") << out;
    }
}
