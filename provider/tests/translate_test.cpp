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

#include "provider/dialect.hpp"
#include "provider/json.hpp"
#include "provider/openai.hpp"

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

TEST(ReqXlate, RefusesBodyWithNoMessagesArray)
{
    // A Responses-API body reaches this translator because /v1/responses is not a
    // recognised target and falls back to the OpenAI dialect. It carries `input`, not
    // `messages`, and every venue translator used to emit an empty conversation and
    // forward it: the upstream saw messages:[], the caller was billed, and the prompt
    // was gone. Measured against the live mock on 2026-09-02, which received
    // {"model":"claude-3","max_tokens":1024,"messages":[]} and answered 200.
    //
    // An explicitly empty array stays valid: it is a well-formed chat request, and
    // several tests below rely on it. What is refused is a body that is not a chat
    // request at all.
    const std::string responses_body = R"({"model":"claude-3","input":"SECRET_PROMPT"})";
    EXPECT_TRUE(openai_to_anthropic_request(responses_body).empty());
    EXPECT_TRUE(openai_to_gemini_request(responses_body).empty());
    EXPECT_TRUE(openai_to_cohere_request(responses_body).empty());
    std::string model_out;
    EXPECT_TRUE(llmbridge::provider::openai_to_bedrock_request(responses_body, model_out).empty());

    // `messages` present but the wrong shape is the same refusal.
    const std::string wrong_shape = R"({"model":"m","messages":{"role":"user"}})";
    EXPECT_TRUE(openai_to_anthropic_request(wrong_shape).empty());
    EXPECT_TRUE(openai_to_gemini_request(wrong_shape).empty());
    EXPECT_TRUE(openai_to_cohere_request(wrong_shape).empty());
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
        "usageMetadata":{"promptTokenCount":8,"candidatesTokenCount":3,"totalTokenCount":13,
        "cachedContentTokenCount":5,"thoughtsTokenCount":2},
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
    // Gemini states thinking beside candidatesTokenCount and bills it as output.
    EXPECT_EQ(u->num_or("completion_tokens"), "5");
    EXPECT_EQ(u->num_or("total_tokens"), "13");
    EXPECT_EQ(u->find("prompt_tokens_details")->num_or("cached_tokens"), "5");
    EXPECT_EQ(u->find("completion_tokens_details")->num_or("reasoning_tokens"), "2");
}

TEST(GeminiResp, NoDetailsBlockWhenGeminiStatesNone)
{
    std::string in = R"({"candidates":[{"content":{"parts":[{"text":"x"}]},"finishReason":"STOP"}],
        "usageMetadata":{"promptTokenCount":8,"candidatesTokenCount":3,"totalTokenCount":11}})";
    Value out = P(gemini_to_openai_response(in));
    const Value* u = out.find("usage");
    EXPECT_EQ(u->find("prompt_tokens_details"), nullptr);
    EXPECT_EQ(u->find("completion_tokens_details"), nullptr);
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
    // "none" keeps the tools, so a history holding tool_use blocks stays valid (P11).
    const std::string none = choice(R"("none")");
    EXPECT_NE(none.find("\"tools\""), std::string::npos) << none;
    EXPECT_NE(none.find(R"("tool_choice":{"type":"none"})"), std::string::npos) << none;
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

TEST(ToolReqInjection, WhitespaceAroundArgumentsIsStillOneObject)
{
    // The old hand-written trailing check measured from the start of the string, so
    // a leading space refused a valid object. Trailing bytes are the parser's job now.
    for (const char* args : {R"( {\"a\":1})", R"(\n{\"a\":1} )", R"(\t{\"a\":1}\r\n)"})
    {
        const Value v = P(openai_to_anthropic_request(with_arguments(args)));
        const Value* input = v.find("messages")->arr[0].find("content")->arr[0].find("input");
        ASSERT_TRUE(input && input->is_object()) << args;
        EXPECT_EQ(input->num_or("a"), "1");
    }
    EXPECT_TRUE(openai_to_anthropic_request(with_arguments(R"( {} x)")).empty());
    EXPECT_TRUE(openai_to_anthropic_request(with_arguments(R"({} {})")).empty());
}

TEST(StrictKeys, ARepeatedKeyAnywhereRefusesTheRequest)
{
    // First-wins here, last-wins upstream: whichever copy a policy checked, the
    // provider would read the other. Unescaped first, so `m\u006fdel` is `model`.
    const std::string msgs = R"("messages":[{"role":"user","content":"hi"}])";
    EXPECT_FALSE(openai_to_anthropic_request(R"({"model":"m",)" + msgs + "}").empty());
    EXPECT_FALSE(openai_to_gemini_request(R"({"model":"m",)" + msgs + "}").empty());
    EXPECT_TRUE(llmbridge::provider::rewrite_model(R"({"model":"a","model":"b"})", "x").empty());
    for (const std::string& body :
         {R"({"model":"cheap","model":"x",)" + msgs + "}",
          R"({"model":"cheap","m\u006fdel":"x",)" + msgs + "}",
          std::string(R"({"model":"m","messages":[{"role":"user","role":"system","content":"hi"}]})")})
    {
        EXPECT_TRUE(openai_to_anthropic_request(body).empty()) << body;
        EXPECT_TRUE(openai_to_gemini_request(body).empty()) << body;
    }
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

// A gateway deciding on the head of a body still arriving reads model_of on the bytes so
// far: every prefix yields nothing or the whole body's own model, never a cut name, a
// nested decoy, a name in a prompt or a later repeat.
TEST(ModelOf, EveryPrefixYieldsNothingOrTheWholeBodysModel)
{
    const std::string bodies[] = {
        R"({"model":"claude-opus-5","messages":[{"role":"user","content":"hi"}]})",
        R"({"messages":[{"role":"user","content":"use {\"model\":\"evil\"} here"}],"model":"gpt-4o"})",
        R"({"metadata":{"model":"decoy"},"stream":true,"model":"m-1","model":"m-2"})",
        R"({"a":{"b":[1,2,{"c":"}"}]},"b":[[]],"c":null,"d":true,"e":-1.5e3,"f":"a\"b","model":"m"})",
        R"( { "max_tokens" : 12 , "model" : "spaced" } )",
        R"({"x":"\\","model":"after-a-backslash"})",
        R"({"x":"\\\"","model":"after-an-escaped-quote"})",
        R"({"model":"a\\b","n":1})",
        R"({"n":123,"model":7})",
        R"({"messages":[]})",
    };
    for (const std::string& body : bodies)
    {
        const std::string_view whole = llmbridge::provider::model_of(body);
        for (size_t n = 0; n <= body.size(); ++n)
        {
            const std::string prefix = body.substr(0, n); // its own buffer, so a read past it is caught
            const std::string_view got = llmbridge::provider::model_of(prefix);
            EXPECT_TRUE(got.empty() || got == whole) << body << " cut at " << n << " read " << got;
        }
    }
    EXPECT_EQ(llmbridge::provider::model_of(R"({"model":"gpt-4o")"), "gpt-4o") << "closed is enough";
    EXPECT_TRUE(llmbridge::provider::model_of(R"({"model":"gpt-4)").empty());
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

// A pretty-printed body put a newline between `true` and the closing brace, and that
// newline used to be read as part of the value.
TEST(WantsStream, WhitespaceAfterTheLiteralIsNotPartOfIt)
{
    using llmbridge::provider::wants_stream;
    EXPECT_TRUE(wants_stream("{\n  \"stream\": true\n}"));
    EXPECT_TRUE(wants_stream("{\"stream\":true \t\r\n, \"model\":\"m\"}"));
    EXPECT_FALSE(wants_stream("{\"stream\": truex }"));
}

// Our readers take the first raw match; a provider unescapes keys and Python's json
// keeps the last duplicate. Any body where the two can differ is refused.
TEST(TopLevelKeys, ARepeatedOrEscapedKeyIsAmbiguousOnlyWhereWeRead)
{
    using llmbridge::provider::KeyCheck;
    using llmbridge::provider::top_level_key_check;
    EXPECT_EQ(top_level_key_check(R"({"model":"m","stream":true,"messages":[]})"), KeyCheck::Ok);
    EXPECT_EQ(top_level_key_check(R"({"stream":false,"stream":true})"), KeyCheck::DuplicateKey);
    EXPECT_EQ(top_level_key_check(R"({"stream":true,"model":"m","stream":false})"),
              KeyCheck::DuplicateKey);
    EXPECT_EQ(top_level_key_check(R"({"model":"a","x":1,"model":"b"})"), KeyCheck::DuplicateKey);
    EXPECT_EQ(top_level_key_check(R"({"str\u0065am":true})"), KeyCheck::EscapedKey);
    EXPECT_EQ(top_level_key_check(R"({"a\"b":1})"), KeyCheck::EscapedKey);
    // stream_options is read one level down, so it is held to the same rule.
    EXPECT_EQ(top_level_key_check(
                  R"({"stream_options":{"include_usage":true,"include_usage":false}})"),
              KeyCheck::DuplicateKey);
    EXPECT_EQ(top_level_key_check(R"({"stream_options":{"include\u005fusage":true}})"),
              KeyCheck::EscapedKey);
    // Anywhere else, keys are the client's schema and none of our business.
    EXPECT_EQ(top_level_key_check(
                  R"({"tools":[{"parameters":{"str\u0065am":1,"a":1,"a":2}}],)"
                  R"("metadata":{"k":1,"k":2},"messages":[{"role":"user","content":"\"stream\""}]})"),
              KeyCheck::Ok);
    EXPECT_EQ(top_level_key_check(R"({"model":"str\u0065am"})"), KeyCheck::Ok) << "a value";
    EXPECT_EQ(top_level_key_check("{}"), KeyCheck::Ok);
    EXPECT_EQ(top_level_key_check(""), KeyCheck::Ok);
    EXPECT_EQ(top_level_key_check("[1,2]"), KeyCheck::Ok);
    std::string many = "{";
    for (int i = 0; i < 257; ++i) many += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":0";
    EXPECT_EQ(top_level_key_check(many + "}"), KeyCheck::TooManyKeys);
}

// The walker finds a string's end with memchr and the parity of the backslashes
// before each quote, and skips nested values over whole strings. These are the
// inputs where that can differ from reading byte by byte.
TEST(TopLevelWalk, StringsEndAtTheFirstQuoteNotEscaped)
{
    using llmbridge::provider::wants_stream;
    // An escaped quote right before the closing one.
    EXPECT_TRUE(wants_stream(R"({"a":"x\"","stream":true})"));
    // Runs of backslashes: even closes the string, odd escapes the quote.
    EXPECT_TRUE(wants_stream(R"({"a":"x\\\\","stream":true})"));
    EXPECT_TRUE(wants_stream(R"({"a":"x\\\"y","stream":true})"));
    EXPECT_FALSE(wants_stream(R"({"a":"x\\\","stream":true})")) << "the string swallows stream";
    EXPECT_TRUE(wants_stream(R"({"a":"\\","stream":true})"));
    // Structure inside strings, at the top level and nested, is text.
    EXPECT_TRUE(wants_stream(R"({"a":"}],{","stream":true})"));
    EXPECT_TRUE(wants_stream(R"({"tools":[{"d":"}]},{\"x","e":["]"]}],"stream":true})"));
}

TEST(TopLevelWalk, UnterminatedAndUnbalancedInputReadsAsAbsent)
{
    using llmbridge::provider::KeyCheck;
    using llmbridge::provider::top_level_key_check;
    using llmbridge::provider::wants_stream;
    EXPECT_FALSE(wants_stream(R"({"a":"abc)" R"(,"stream":true)"));
    EXPECT_FALSE(wants_stream(R"({"a":"abc\"})" R"(,"stream":true})"));
    EXPECT_FALSE(wants_stream(R"({"tools":[{"a":"}]}],"stream":true)"));
    EXPECT_FALSE(wants_stream(R"({"tools":[{"a":1}],"stream":true)" "\\"));
    EXPECT_FALSE(wants_stream(R"({"str)"));
    EXPECT_EQ(top_level_key_check(R"({"a":1,"a)"), KeyCheck::Ok) << "unwalkable: no opinion";
    // Depth: a thousand levels is still one value, and one bracket short is malformed.
    const std::string deep = std::string(1000, '[') + std::string(1000, ']');
    EXPECT_TRUE(wants_stream("{\"x\":" + deep + ",\"stream\":true}"));
    EXPECT_FALSE(wants_stream("{\"x\":" + deep.substr(1) + ",\"stream\":true}"));
}

// memchr reads in wide chunks, so a key or a closing quote at every offset across
// several chunk widths must land where a byte-by-byte read would.
TEST(TopLevelWalk, AQuoteAtEveryOffsetIsFound)
{
    using llmbridge::provider::KeyCheck;
    using llmbridge::provider::stream_usage_of;
    using llmbridge::provider::top_level_key_check;
    using llmbridge::provider::wants_stream;
    for (size_t pad = 0; pad < 140; ++pad)
    {
        const std::string fill(pad, 'a');
        const std::string esc = pad % 2 ? "\\\"" : "\\\\";
        const std::string body = "{\"p\":\"" + fill + esc + "\",\"stream\":true,"
                                 "\"stream_options\":{\"include_usage\":true}}";
        EXPECT_TRUE(wants_stream(body)) << pad;
        EXPECT_TRUE(stream_usage_of(body)) << pad;
        EXPECT_EQ(top_level_key_check(body), KeyCheck::Ok) << pad;
        EXPECT_EQ(top_level_key_check(body.substr(0, body.size() - 1) + ",\"" + fill + "\":1,\"" +
                                      fill + "\":2}"),
                  KeyCheck::DuplicateKey)
            << pad;
    }
}

// The gateway reads all four facts in one walk. It must answer exactly as the four
// single-key readers do, first occurrence included, on bodies they disagree about.
TEST(TopLevelFacts, OnePassMatchesTheSingleKeyReaders)
{
    namespace p = llmbridge::provider;
    const char* bodies[] = {
        R"({"model":"m","stream":true,"stream_options":{"include_usage":true}})",
        R"({"messages":[{"role":"user","content":"hi"}],"model":"gpt","stream":true})",
        R"({"stream":false,"stream":true,"model":"a","model":"b"})",
        R"({"stream":true,"stream":false,"stream_options":{"include_usage":true},"stream_options":{}})",
        R"({"stream_options":{"include_usage":true,"include_usage":false},"stream":true})",
        R"({"model":,"model":"b"})",
        R"({"model":"a\"b","stream":true})",
        R"({"model":"m","stream":true,"x":"unterminated)",
        R"({"x":"unterminated,"model":"m"})",
        R"({"str\u0065am":true,"model":"m","stream":true})",
        R"({"tools":[{"parameters":{"stream":true,"model":"n"}}],"model":"m"})",
        R"([{"model":"m"}])",
        "",
        "{}",
    };
    for (const char* b : bodies)
    {
        const p::TopLevelFacts f = p::top_level_facts(b);
        EXPECT_EQ(f.model, p::model_of(b)) << b;
        EXPECT_EQ(f.model.data(), p::model_of(b).data()) << b;
        EXPECT_EQ(f.stream, p::wants_stream(b)) << b;
        EXPECT_EQ(f.include_usage, p::stream_usage_of(b)) << b;
        EXPECT_EQ(f.keys, p::top_level_key_check(b)) << b;
    }
    const p::TopLevelFacts dup = p::top_level_facts(R"({"stream":false,"stream":true})");
    EXPECT_FALSE(dup.stream) << "the first occurrence, as wants_stream reads it";
    EXPECT_EQ(dup.keys, p::KeyCheck::DuplicateKey);
}

TEST(TopLevelFacts, TheRequestedCacheLifetimeIsReadInTheSameWalk)
{
    namespace p = llmbridge::provider;
    const p::TopLevelFacts f = p::top_level_facts(
        R"({"messages":[],"prompt_cache_options":{"mode":"explicit","ttl":"1h"},"model":"m"})");
    EXPECT_EQ(f.cache_ttl, "1h");
    EXPECT_EQ(f.model, "m");
    EXPECT_EQ(p::top_level_facts(R"({"model":"m"})").cache_ttl, "");
    EXPECT_EQ(p::top_level_facts(R"({"tools":[{"prompt_cache_options":{"ttl":"1h"}}]})").cache_ttl, "")
        << "a nested member is not the request's";
    EXPECT_EQ(p::top_level_facts(R"({"prompt_cache_options":{"ttl":"5m","ttl":"1h"}})").keys,
              p::KeyCheck::DuplicateKey)
        << "we would price the first lifetime while the venue may sell the last";
    EXPECT_EQ(p::top_level_facts(R"({"prompt_cache_options":{"tt\u006c":"1h"}})").keys,
              p::KeyCheck::EscapedKey);
}

TEST(StreamUsage, OnlyATopLevelStreamOptionsIncludeUsageTrueCounts)
{
    using llmbridge::provider::stream_usage_of;
    EXPECT_TRUE(stream_usage_of(R"({"stream":true,"stream_options":{"include_usage":true}})"));
    EXPECT_TRUE(stream_usage_of("{ \"stream_options\" : { \"x\":[1], \"include_usage\" : true\n }\n}"));
    EXPECT_FALSE(stream_usage_of(R"({"stream":true})"));
    EXPECT_FALSE(stream_usage_of(R"({"stream_options":{"include_usage":false}})"));
    EXPECT_FALSE(stream_usage_of(R"({"stream_options":{}})"));
    // Not a boolean, so not true: the provider would refuse it or ignore it.
    EXPECT_FALSE(stream_usage_of(R"({"stream_options":{"include_usage":"true"}})"));
    EXPECT_FALSE(stream_usage_of(R"({"stream_options":{"include_usage":1}})"));
    EXPECT_FALSE(stream_usage_of(R"({"stream_options":true})"));
    EXPECT_FALSE(stream_usage_of(R"({"stream_options":"{\"include_usage\":true}"})"));
    // Top level only: the same key in a tool schema or at the root is not the option.
    EXPECT_FALSE(stream_usage_of(R"({"include_usage":true})"));
    EXPECT_FALSE(stream_usage_of(
        R"({"tools":[{"parameters":{"stream_options":{"include_usage":true}}}]})"));
    EXPECT_FALSE(stream_usage_of(R"({"metadata":{"stream_options":{"include_usage":true}}})"));
    EXPECT_FALSE(stream_usage_of(""));
    EXPECT_FALSE(stream_usage_of("[]"));
    EXPECT_FALSE(stream_usage_of(R"({"stream_options":{"include_usage":tr)"));
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

// ── apply_overrides: every body override in one parse ──────────────────────────
// Anthropic's fast mode and US-only inference are top-level fields like the tier, so
// the route sets them the same way. All four ride one parse and one copy of the body.
namespace
{
    std::string overrides(std::string_view body, llmbridge::provider::Overrides o,
                          std::string_view* had = nullptr)
    {
        std::string out;
        if (!llmbridge::provider::apply_overrides(body, o, had, out)) return {};
        return out;
    }
} // namespace

TEST(ApplyOverrides, InsertsSpeedAndInferenceGeoWhenAbsent)
{
    EXPECT_EQ(overrides(R"({"model":"m","max_tokens":8})", {.speed = "fast", .inference_geo = "us"}),
              R"({"speed":"fast","inference_geo":"us","model":"m","max_tokens":8})");
    EXPECT_EQ(overrides("{}", {.speed = "fast"}), R"({"speed":"fast"})");
    EXPECT_EQ(overrides("{ }", {.speed = "fast", .inference_geo = "us"}),
              R"({"speed":"fast","inference_geo":"us" })");
}

TEST(ApplyOverrides, ReplacesSpeedAndInferenceGeoWhenPresent)
{
    EXPECT_EQ(overrides(R"({"model":"m","speed":"standard","inference_geo":"global"})",
                        {.speed = "fast", .inference_geo = "us"}),
              R"({"model":"m","speed":"fast","inference_geo":"us"})");
}

TEST(ApplyOverrides, AllFourInOnePassWhetherPresentOrAbsent)
{
    std::string_view had;
    const std::string out =
        overrides(R"({"inference_geo":"global","model":"a","messages":[],"service_tier":"auto"})",
                  {"b", "standard_only", "fast", "us"}, &had);
    EXPECT_EQ(out, R"({"speed":"fast","inference_geo":"us","model":"b","messages":[],"service_tier":"standard_only"})");
    EXPECT_EQ(had, "auto");
    bool ok = false;
    const auto v = llmbridge::provider::json::parse(out, ok);
    ASSERT_TRUE(ok) << out;
    EXPECT_EQ(v.find("speed")->sv, "fast");
    EXPECT_EQ(v.find("inference_geo")->sv, "us");
}

TEST(ApplyOverrides, AKeyQuotedInsideAStringIsNotTheKey)
{
    const std::string out =
        overrides(R"({"model":"m","messages":[{"content":"\"speed\":\"standard\""}]})", {.speed = "fast"});
    EXPECT_EQ(out.find(R"({"speed":"fast",)"), 0u) << out;
    EXPECT_NE(out.find(R"(\"speed\":\"standard\")"), std::string::npos) << out;
}

TEST(ApplyOverrides, RefusesBadBytesInEveryValue)
{
    const std::string_view body = R"({"model":"m"})";
    for (const std::string& bad : {std::string("fa\"st"), std::string("fa\\st"), std::string("fa\x01st"),
                                  std::string("fa\rst"), std::string("fa\nst"), std::string("fa\x7fst")})
    {
        EXPECT_TRUE(overrides(body, {.speed = bad}).empty()) << bad;
        EXPECT_TRUE(overrides(body, {.inference_geo = bad}).empty()) << bad;
    }
}

TEST(ApplyOverrides, RefusesANonStringValueAlreadyThere)
{
    EXPECT_TRUE(overrides(R"({"model":"m","speed":null})", {.speed = "fast"}).empty());
    EXPECT_TRUE(overrides(R"({"model":"m","inference_geo":1})", {.inference_geo = "us"}).empty());
}

// The parser keeps keys as written, so `sp\u0065ed` is not found as `speed`; inserting
// beside it would hand the provider two.
TEST(ApplyOverrides, RefusesToInsertBesideAnEscapedKey)
{
    EXPECT_TRUE(overrides(R"({"model":"m","sp\u0065ed":"standard"})", {.speed = "fast"}).empty());
}

TEST(ApplyOverrides, TheTwoArgumentFormIsUnchanged)
{
    const std::string_view body = R"({"model":"m","service_tier":"flex"})";
    EXPECT_EQ(llmbridge::provider::apply_overrides(body, "n", "priority"),
              overrides(body, {.model = "n", .service_tier = "priority"}));
    EXPECT_EQ(overrides(body, {}), body);
}

// ── openai::scan_usage and StreamUsage: key-anchored reads of venue bytes ──────
namespace
{
    namespace oai = llmbridge::provider::openai;
} // namespace

TEST(JsonScanUsage, ReadsEachVenuesShape)
{
    const oai::Usage o = oai::scan_usage(
        R"({"choices":[],"usage":{"prompt_tokens":10,"completion_tokens":20,)"
        R"("prompt_tokens_details":{"cached_tokens":4,"audio_tokens":1},)"
        R"("completion_tokens_details":{"reasoning_tokens":7,"audio_tokens":2}}})");
    EXPECT_EQ(o.in, 10);
    EXPECT_EQ(o.out, 20);
    EXPECT_EQ(o.cached, 4);
    EXPECT_EQ(o.audio_in, 1);
    EXPECT_EQ(o.audio_out, 2);
    EXPECT_EQ(o.reasoning, 7);
    const oai::Usage a = oai::scan_usage(
        R"({"content":[],"usage":{"input_tokens":5,"cache_read_input_tokens":3,)"
        R"("cache_creation_input_tokens":2,"output_tokens":9}})");
    EXPECT_EQ(a.in, 10) << "the whole prompt: fresh, read and write";
    EXPECT_EQ(a.cached, 3);
    EXPECT_EQ(a.cache_write, 2);
    EXPECT_EQ(a.out, 9);
    EXPECT_EQ(oai::scan_usage(R"({"usage":null})").in, -1);
    EXPECT_EQ(oai::scan_usage("").out, -1);
}

TEST(JsonScanUsage, GeminiThinkingIsOutput)
{
    const oai::Usage g = oai::scan_usage(
        R"({"usageMetadata":{"promptTokenCount":8,"candidatesTokenCount":3,)"
        R"("thoughtsTokenCount":2,"totalTokenCount":13}})");
    EXPECT_EQ(g.in, 8);
    EXPECT_EQ(g.out, 5) << "stated beside candidatesTokenCount, billed as output";
    EXPECT_EQ(g.reasoning, 2);
}

TEST(JsonScanUsage, ResponsesAndAnthropicDetails)
{
    const oai::Usage a = oai::scan_usage(
        R"({"content":[],"usage":{"input_tokens":5,"cache_read_input_tokens":3,)"
        R"("cache_creation_input_tokens":0,"output_tokens":267,)"
        R"("output_tokens_details":{"thinking_tokens":185}}})");
    EXPECT_EQ(a.out, 267) << "thinking stays inside output_tokens";
    EXPECT_EQ(a.reasoning, 185);
    const oai::Usage r = oai::scan_usage(
        R"({"output":[],"usage":{"input_tokens":66125,)"
        R"("input_tokens_details":{"cache_write_tokens":66123,"cached_tokens":0},)"
        R"("output_tokens":107,"output_tokens_details":{"reasoning_tokens":52},)"
        R"("total_tokens":66232}})");
    EXPECT_EQ(r.in, 66125);
    EXPECT_EQ(r.cache_write, 66123) << "Bedrock Mantle's Responses write, billed above input";
    EXPECT_EQ(r.out, 107);
    EXPECT_EQ(r.reasoning, 52);
    EXPECT_EQ(oai::scan_usage(R"({"usage":{"input_tokens":5,"output_tokens":9}})").reasoning, -1)
        << "no breakdown is not a breakdown of zero";
}

TEST(JsonScanUsage, TogethersFlatCachedCount)
{
    const oai::Usage u = oai::scan_usage(
        R"({"usage":{"prompt_tokens":1200,"completion_tokens":8,"total_tokens":1208,)"
        R"("cached_tokens":1024}})");
    EXPECT_EQ(u.in, 1200);
    EXPECT_EQ(u.cached, 1024) << "beside prompt_tokens, not in its details";
}

// Anthropic bills a declined attempt as it bills a refusal ("How refusals are billed"): one
// that produced output always; one before any output only in the bio, frontier_llm and
// reasoning_extraction categories, which only the reply's fallback block names.
namespace
{
    std::string fallback_reply(const std::string& category)
    {
        return R"({"content":[{"type":"fallback","from":{"model":"claude-fable-5"},"to":{"model":"claude-opus-4-8"},)"
               R"("trigger":{"type":"refusal","category":)" + category + R"(}},{"type":"text","text":"Hi"}],)"
               R"("usage":{"input_tokens":412,"output_tokens":264,)"
               R"("cache_read_input_tokens":0,"cache_creation_input_tokens":0,"iterations":[)"
               R"({"type":"message","model":"claude-fable-5","input_tokens":535,"output_tokens":0,)"
               R"("cache_read_input_tokens":20,"cache_creation_input_tokens":7},)"
               R"({"type":"fallback_message","model":"claude-opus-4-8","input_tokens":412,)"
               R"("output_tokens":264,"cache_read_input_tokens":0,"cache_creation_input_tokens":0}]}})";
    }
    oai::Usage billed(const std::string& body)
    {
        oai::Usage u = oai::scan_usage(body, 0);
        oai::RefusalTriggers t;
        oai::scan_refusal_triggers(body, t);
        oai::bill_declined(u, t);
        return u;
    }
} // namespace

TEST(JsonScanUsage, ADeclineBeforeAnyOutputIsBilledOnlyInABilledCategory)
{
    const oai::Usage held = oai::scan_usage(fallback_reply(R"("bio")"), 0);
    EXPECT_EQ(held.in, 412) << "the top level is the attempt that answered";
    EXPECT_EQ(held.out, 264);
    EXPECT_EQ(held.prior_attempts, 1);
    EXPECT_EQ(held.prior_in, 0) << "held for its category, not yet billed";
    EXPECT_EQ(held.prior_held_n, 1);
    EXPECT_EQ(held.prior_model.view(), "claude-fable-5");
    EXPECT_EQ(held.compaction_in, -1);

    for (const char* cat : {R"("bio")", R"("frontier_llm")", R"("reasoning_extraction")"})
    {
        const oai::Usage u = billed(fallback_reply(cat));
        EXPECT_EQ(u.prior_in, 562) << cat << ": its cache read and write are part of its prompt";
        EXPECT_EQ(u.prior_out, 0);
        EXPECT_EQ(u.prior_cached, 20);
        EXPECT_EQ(u.prior_cache_write, 7);
        EXPECT_EQ(u.prior_billed, 1);
        EXPECT_EQ(u.prior_unverified, 0);
        EXPECT_EQ(u.prior_held_n, 0);
    }
    for (const char* cat : {R"("cyber")", R"("general_harms")", "null"})
    {
        const oai::Usage u = billed(fallback_reply(cat));
        EXPECT_EQ(u.prior_in, 0) << cat << " is not billed before any output";
        EXPECT_EQ(u.prior_billed, 0);
        EXPECT_EQ(u.prior_unverified, 0) << "its category was read";
        EXPECT_EQ(u.prior_attempts, 1);
    }
    // No fallback block to read: left out, said so.
    oai::Usage blind = oai::scan_usage(fallback_reply(R"("bio")"), 0);
    oai::bill_declined(blind, {});
    EXPECT_EQ(blind.prior_in, 0);
    EXPECT_EQ(blind.prior_unverified, 1);
    oai::bill_declined(blind, {});
    EXPECT_EQ(blind.prior_unverified, 1) << "judged once";
}

// A decline after some output is billed whatever its category; in a chain, each attempt is
// judged by its own block, in order.
TEST(JsonScanUsage, EachDeclinedAttemptIsJudgedByItsOwnBlock)
{
    const std::string body =
        R"({"content":[{"type":"text","text":"Part"},)"
        R"({"type":"fallback","from":{"model":"a"},"to":{"model":"b"},"trigger":{"type":"refusal","category":"cyber"}},)"
        R"({"type":"fallback","from":{"model":"b"},"to":{"model":"c"},"trigger":{"type":"refusal","category":"cyber"}},)"
        R"({"type":"fallback","from":{"model":"c"},"to":{"model":"d"},"trigger":{"type":"refusal","category":"bio"}},)"
        R"({"type":"text","text":"Hi"}],"usage":{"input_tokens":10,"output_tokens":5,"iterations":[)"
        R"({"type":"message","model":"a","input_tokens":100,"output_tokens":30},)"
        R"({"type":"message","model":"b","input_tokens":200,"output_tokens":0},)"
        R"({"type":"message","model":"c","input_tokens":400,"output_tokens":0},)"
        R"({"type":"fallback_message","model":"d","input_tokens":10,"output_tokens":5}]}})";
    const oai::Usage u = billed(body);
    EXPECT_EQ(u.prior_attempts, 3);
    EXPECT_EQ(u.prior_billed, 2) << "a produced output; c declined in bio; b in cyber before any";
    EXPECT_EQ(u.prior_in, 100 + 400);
    EXPECT_EQ(u.prior_out, 30);
    EXPECT_EQ(u.prior_model.view(), "a");
    // Text that mentions a trigger is not one: its quotes are escaped.
    oai::RefusalTriggers t;
    oai::scan_refusal_triggers(R"({"text":"\"trigger\":{\"type\":\"refusal\",\"category\":\"bio\"}"})", t);
    EXPECT_EQ(t.n, 0);
}

// A stream's fallback block can be split across reads anywhere: it is read once.
TEST(JsonScanStream, ADeclinedAttemptsCategoryIsReadAcrossReads)
{
    const std::string stream =
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
        "{\"type\":\"fallback\",\"from\":{\"model\":\"claude-fable-5\"},\"to\":{\"model\":\"claude-opus-4-8\"},"
        "\"trigger\":{\"type\":\"refusal\",\"category\":\"reasoning_extraction\"}}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"usage\":{\"output_tokens\":264,\"iterations\":["
        "{\"type\":\"message\",\"model\":\"claude-fable-5\",\"input_tokens\":535,\"output_tokens\":0},"
        "{\"type\":\"fallback_message\",\"model\":\"claude-opus-4-8\",\"input_tokens\":412,\"output_tokens\":264}]}}\n\n";
    for (size_t step = 1; step <= stream.size(); step += (step < 40 ? 1 : 37))
    {
        oai::StreamUsage s;
        s.reset();
        for (size_t at = 0; at < stream.size(); at += step) s.feed(std::string_view(stream).substr(at, step));
        const oai::Usage u = s.usage();
        EXPECT_EQ(u.prior_billed, 1) << "reads of " << step;
        EXPECT_EQ(u.prior_in, 535) << "reads of " << step;
        EXPECT_EQ(u.prior_unverified, 0) << "reads of " << step;
    }
}

TEST(JsonScanUsage, AnthropicsCompactionPassIsBilledApart)
{
    const oai::Usage u = oai::scan_usage(
        R"({"usage":{"input_tokens":23000,"output_tokens":1000,"iterations":[)"
        R"({"type":"compaction","input_tokens":180000,"output_tokens":3500},)"
        R"({"type":"message","input_tokens":23000,"output_tokens":1000}]}})");
    EXPECT_EQ(u.in, 23000);
    EXPECT_EQ(u.compaction_in, 180000);
    EXPECT_EQ(u.compaction_out, 3500);
    EXPECT_EQ(u.prior_attempts, -1) << "without a fallback a message entry is the served one";
    const oai::Usage none = oai::scan_usage(
        R"({"usage":{"input_tokens":9,"output_tokens":2,"iterations":[)"
        R"({"type":"message","input_tokens":9,"output_tokens":2}]}})");
    EXPECT_EQ(none.compaction_in, -1) << "the beta lists the one attempt on every reply";
    EXPECT_EQ(none.prior_in, -1);
}

TEST(JsonScanUsage, PerCallToolsAndWhatServed)
{
    const oai::Usage a = oai::scan_usage(
        R"({"usage":{"input_tokens":10682,"output_tokens":510,"service_tier":"standard",)"
        R"("server_tool_use":{"web_search_requests":3,"web_fetch_requests":1,)"
        R"("code_execution_requests":2},"speed":"fast","inference_geo":"us"}})");
    EXPECT_EQ(a.web_search, 3);
    EXPECT_EQ(a.web_fetch, 1);
    EXPECT_EQ(a.code_exec, 2);
    EXPECT_EQ(a.speed.view(), "fast");
    EXPECT_EQ(a.inference_geo.view(), "us");
    EXPECT_EQ(a.in, 10682);
    const oai::Usage o = oai::scan_usage(
        R"({"usage":{"prompt_tokens":800,"completion_tokens":5,)"
        R"("prompt_tokens_details":{"image_tokens":765,"text_tokens":35}}})");
    EXPECT_EQ(o.image_in, 765);
    EXPECT_EQ(oai::scan_usage(R"({"usage":{"input_tokens":1,"speed":"\u0066ast"}})").speed.view(), "")
        << "an escaped word is not read";
    EXPECT_EQ(oai::scan_usage(R"({"usage":{"input_tokens":1,"inference_geo":")" +
                              std::string(17, 'x') + R"("}})").inference_geo.view(), "")
        << "a word that does not fit whole is left out, not cut";
}

TEST(JsonScanUsage, DeepInfrasEstimatedCostInPicodollars)
{
    const auto cost = [](std::string_view v) {
        return oai::scan_usage(R"({"usage":{"prompt_tokens":1,"estimated_cost":)" +
                               std::string(v) + "}}").venue_cost_pdollars;
    };
    EXPECT_EQ(cost("0.0000268"), 26'800'000);
    EXPECT_EQ(cost("2.68e-05"), 26'800'000);
    EXPECT_EQ(cost("2.68E-5"), 26'800'000);
    EXPECT_EQ(cost("1.5"), 1'500'000'000'000);
    EXPECT_EQ(cost("0"), 0);
    EXPECT_EQ(cost("4.5e-13"), 0);
    EXPECT_EQ(cost("5.5e-12"), 6) << "rounded half up";
    EXPECT_EQ(cost("-0.1"), -1);
    EXPECT_EQ(cost("\"0.1\""), -1);
    EXPECT_EQ(cost("1e400"), -1);
    EXPECT_EQ(cost("1.2.3"), -1);
}

TEST(JsonScanUsage, ResponsesSearchCallsAreCountedFromTheOutput)
{
    oai::Usage u;
    oai::count_search_calls(
        R"({"output":[{"type":"web_search_call","id":"ws_1","status":"completed"},)"
        R"({"type": "web_search_call","id":"ws_2"},{"type":"file_search_call","id":"fs_1"},)"
        R"({"type":"message","content":[{"type":"output_text",)"
        R"("text":"the \"web_search_call\" item"}]}],"tools":[{"type":"web_search"}]})",
        false, u);
    EXPECT_EQ(u.web_search, 2) << "prompt text cannot forge an item";
    EXPECT_EQ(u.file_search, 1);
    oai::Usage none;
    oai::count_search_calls(R"({"output":[{"type":"message"}]})", false, none);
    EXPECT_EQ(none.web_search, -1) << "none counted is not a count of zero";
}

TEST(JsonScanStream, ResponsesSearchEventsCountOnceAtEverySplit)
{
    const std::string s =
        "event: response.output_item.added\ndata: {\"type\":\"response.output_item.added\","
        "\"item\":{\"type\":\"web_search_call\",\"id\":\"ws_1\"}}\n\n"
        "event: response.web_search_call.completed\n"
        "data: {\"type\":\"response.web_search_call.completed\",\"item_id\":\"ws_1\"}\n\n"
        "event: response.file_search_call.completed\n"
        "data: {\"type\":\"response.file_search_call.completed\",\"item_id\":\"fs_1\"}\n\n"
        "event: response.completed\ndata: {\"type\":\"response.completed\",\"response\":{"
        "\"output\":[{\"type\":\"web_search_call\"},{\"type\":\"file_search_call\"}],"
        "\"usage\":{\"input_tokens\":30,\"output_tokens\":4}}}\n\n";
    for (size_t cut = 0; cut <= s.size(); ++cut)
        for (size_t cut2 = cut; cut2 <= s.size(); cut2 += 7)
        {
            oai::StreamUsage u;
            u.reset(true);
            u.feed(std::string_view(s).substr(0, cut));
            u.feed(std::string_view(s).substr(cut, cut2 - cut));
            u.feed(std::string_view(s).substr(cut2));
            ASSERT_EQ(u.usage().web_search, 1) << cut << "," << cut2;
            ASSERT_EQ(u.usage().file_search, 1) << cut << "," << cut2;
            ASSERT_EQ(u.usage().in, 30) << cut << "," << cut2;
        }
    oai::StreamUsage bytes;
    bytes.reset(true);
    for (const char c : s) bytes.feed(std::string_view(&c, 1));
    EXPECT_EQ(bytes.usage().web_search, 1) << "a byte a read";
    oai::StreamUsage off;
    off.feed(s);
    EXPECT_EQ(off.usage().web_search, -1) << "counted only where asked";
}

TEST(JsonScanStream, AnthropicsLastDeltaStatesToolsAndAttempts)
{
    oai::StreamUsage s;
    s.feed("data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":20,"
           "\"output_tokens\":1,\"speed\":\"fast\",\"inference_geo\":\"us\"}}}\n\n");
    s.feed("data: {\"type\":\"message_delta\",\"usage\":{\"input_tokens\":900,"
           "\"output_tokens\":40,\"server_tool_use\":{\"web_search_requests\":1}}}\n\n");
    s.feed("data: {\"type\":\"message_delta\",\"usage\":{\"input_tokens\":1500,"
           "\"output_tokens\":80,\"server_tool_use\":{\"web_search_requests\":2},"
           "\"iterations\":[{\"type\":\"message\",\"model\":\"claude-fable-5\","
           "\"input_tokens\":700,\"output_tokens\":30},{\"type\":\"fallback_message\","
           "\"model\":\"claude-opus-4-8\",\"input_tokens\":1500,\"output_tokens\":80}]}}\n\n");
    EXPECT_EQ(s.usage().web_search, 2) << "cumulative: the last statement";
    EXPECT_EQ(s.usage().speed.view(), "fast") << "message_start's, which the deltas leave out";
    EXPECT_EQ(s.usage().inference_geo.view(), "us");
    EXPECT_EQ(s.usage().prior_in, 700);
    EXPECT_EQ(s.usage().prior_out, 30);
    EXPECT_EQ(s.usage().prior_model.view(), "claude-fable-5");
    EXPECT_EQ(s.usage().in, 1500);
}

TEST(JsonScanUsage, ANullDetailsBlockIsNotTheNextOne)
{
    const oai::Usage u = oai::scan_usage(
        R"({"usage":{"prompt_tokens":10,"completion_tokens":20,"prompt_tokens_details":null,)"
        R"("completion_tokens_details":{"reasoning_tokens":7,"audio_tokens":5}}})");
    EXPECT_EQ(u.audio_in, -1) << "the prompt side stated nothing";
    EXPECT_EQ(u.audio_out, 5);
    EXPECT_EQ(u.reasoning, 7);
}

TEST(JsonScanUsage, ACountPastEighteenDigitsIsNotStated)
{
    const oai::Usage u = oai::scan_usage(
        R"({"usage":{"prompt_tokens":99999999999999999999999,"completion_tokens":2}})");
    EXPECT_EQ(u.in, -1);
    EXPECT_EQ(u.out, 2);
    EXPECT_EQ(oai::scan_usage(R"({"usage":{"prompt_tokens":999999999999999999}})").in,
              999999999999999999LL);
}

TEST(JsonScanUsage, ModelOutputCannotForgeACount)
{
    // A quote in a string is escaped, so prompt or completion text is never a key.
    EXPECT_EQ(oai::scan_usage(R"({"choices":[{"message":{"content":"x \"prompt_tokens"}}],)"
                              R"("usage":{"prompt_tokens":10,"completion_tokens":2}})")
                  .in,
              10);
    // A tool's input is raw JSON the model wrote, and it comes before the usage.
    const std::string body =
        R"({"content":[{"type":"tool_use","id":"t","name":"f","input":{"usage":)"
        R"({"input_tokens":1,"cache_creation_input_tokens":900000},"service_tier":"priority"}}],)"
        R"("usage":{"input_tokens":5000,"output_tokens":3,"service_tier":"standard"}})";
    const oai::Usage u = oai::scan_usage(body);
    EXPECT_EQ(u.in, 5000);
    EXPECT_EQ(u.cache_write, 0);
    EXPECT_EQ(oai::scan_string(body, "\"service_tier\"", /*last=*/true), "standard");
    EXPECT_EQ(oai::scan_string(R"({"a":"\"service_tier\":\"x\"","service_tier":"flex"})",
                               "\"service_tier\"", /*last=*/false),
              "flex");
}

TEST(JsonScanUsage, TheErrorTypeIsTheErrorObjectsOwn)
{
    EXPECT_EQ(oai::scan_error_type(
                  R"({"error":{"details":{"code":"nested"},"code":"rate_limit_exceeded"}})"),
              "rate_limit_exceeded");
    EXPECT_EQ(oai::scan_error_type(
                  R"({"type":"error","error":{"type":"overloaded_error","message":"x"}})"),
              "overloaded_error");
    // Google's code is a number, never a name; its name is `status`.
    EXPECT_EQ(oai::scan_error_type(R"({"error":{"code":429,"status":"RESOURCE_EXHAUSTED"}})"),
              "RESOURCE_EXHAUSTED");
    EXPECT_EQ(oai::scan_error_type(R"({"message":"\"error\":{\"code\":\"x\"}"})"), "");
}

TEST(JsonScanStream, EverySplitOfAStreamGivesTheSameCounts)
{
    const std::string anthropic =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m\","
        "\"usage\":{\"input_tokens\":12,\"cache_read_input_tokens\":30,"
        "\"cache_creation_input_tokens\":5,\"cache_creation\":{\"ephemeral_5m_input_tokens\":5},"
        "\"output_tokens\":1}}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\","
        "\"text\":\"\\\"usage\\\":{\\\"output_tokens\\\":99}\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
        "\"usage\":{\"output_tokens\":42,\"output_tokens_details\":{\"thinking_tokens\":30}}}\n\n";
    const std::string openai =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}],\"usage\":null}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3,"
        "\"completion_tokens_details\":{\"reasoning_tokens\":1}}}\n\ndata: [DONE]\n\n";
    for (const std::string* s : {&anthropic, &openai})
    {
        oai::StreamUsage whole;
        whole.feed(*s);
        for (size_t cut = 0; cut <= s->size(); ++cut)
            for (size_t cut2 = cut; cut2 <= s->size(); cut2 += 37)
            {
                oai::StreamUsage split;
                split.feed(std::string_view(*s).substr(0, cut));
                split.feed(std::string_view(*s).substr(cut, cut2 - cut));
                split.feed(std::string_view(*s).substr(cut2));
                const oai::Usage& a = whole.usage();
                const oai::Usage& b = split.usage();
                ASSERT_EQ(a.in, b.in) << cut << "," << cut2;
                ASSERT_EQ(a.out, b.out) << cut << "," << cut2;
                ASSERT_EQ(a.cached, b.cached) << cut << "," << cut2;
                ASSERT_EQ(a.cache_write_5m, b.cache_write_5m) << cut << "," << cut2;
                ASSERT_EQ(a.reasoning, b.reasoning) << cut << "," << cut2;
            }
    }
    oai::StreamUsage a;
    a.feed(anthropic);
    EXPECT_EQ(a.usage().in, 47) << "a delta stating output alone leaves the prompt alone";
    EXPECT_EQ(a.usage().out, 42) << "last statement wins";
    EXPECT_EQ(a.usage().cached, 30);
    EXPECT_EQ(a.usage().cache_write_5m, 5);
    EXPECT_EQ(a.usage().reasoning, 30) << "thinking arrives on the last message_delta";
    oai::StreamUsage o;
    o.feed(openai);
    EXPECT_EQ(o.usage().in, 7);
    EXPECT_EQ(o.usage().reasoning, 1);
    o.reset();
    EXPECT_EQ(o.usage().in, -1);
}

TEST(JsonScanStream, AnthropicsCumulativePromptOnTheLastDeltaWins)
{
    // Server tools grow the prompt after message_start; message_delta restates it.
    oai::StreamUsage s;
    s.feed("data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":2679,"
           "\"cache_read_input_tokens\":100,\"cache_creation_input_tokens\":20,"
           "\"cache_creation\":{\"ephemeral_5m_input_tokens\":20},\"output_tokens\":3}}}\n\n");
    s.feed("data: {\"type\":\"message_delta\",\"usage\":{\"input_tokens\":10682,"
           "\"cache_read_input_tokens\":100,\"cache_creation_input_tokens\":20,"
           "\"output_tokens\":510}}\n\n");
    EXPECT_EQ(s.usage().in, 10802);
    EXPECT_EQ(s.usage().cached, 100);
    EXPECT_EQ(s.usage().cache_write_5m, 20) << "the split is message_start's alone";
    EXPECT_EQ(s.usage().out, 510);
    // A restatement without the cache fields reads them as zero; it must not shrink.
    s.feed("data: {\"type\":\"message_delta\",\"usage\":{\"input_tokens\":10682,"
           "\"output_tokens\":511}}\n\n");
    EXPECT_EQ(s.usage().in, 10802);
    EXPECT_EQ(s.usage().cached, 100);
}

TEST(JsonScanStream, CarriesOnlyWhatAnUnfinishedObjectNeeds)
{
    oai::StreamUsage s;
    const std::string chunk =
        "data: {\"choices\":[{\"delta\":{\"content\":\"" + std::string(4000, 'x') +
        "\"}}],\"usage\":null}\n\n";
    for (int i = 0; i < 1000; ++i) s.feed(chunk);
    s.feed("data: {\"choices\":[],\"usage\":{\"prompt_tokens\":");
    s.feed("5,\"completion_tokens\":6}}\n\n");
    EXPECT_EQ(s.usage().in, 5);
    EXPECT_EQ(s.usage().out, 6);
}

// ── openai::Envelope and write_error: the one writer of OpenAI objects ─────────
TEST(TranslateEnvelope, WritesEachShapeOnce)
{
    std::string out;
    oai::Usage u;
    u.in = 3;
    u.out = 4;
    u.cached = 2;
    oai::Envelope(out, oai::Shape::Completion, "i", "7", "m").choice().end_choice("stop").close(&u);
    EXPECT_EQ(out, R"({"id":"i","object":"chat.completion","created":7,"model":"m","choices":[)"
                   R"({"index":0,"message":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":3,)"
                   R"("completion_tokens":4,"total_tokens":7,"prompt_tokens_details":)"
                   R"({"cached_tokens":2}}})");
    out.clear();
    oai::Envelope(out, oai::Shape::Chunk, "i", "7", "m").choice().end_choice("").close(nullptr, true);
    EXPECT_EQ(out, R"({"id":"i","object":"chat.completion.chunk","created":7,"model":"m",)"
                   R"("choices":[{"index":0,"delta":{},"finish_reason":null}],"usage":null})");
    out.clear();
    oai::write_error(out, "a\nb", "t");
    EXPECT_EQ(out, R"({"error":{"message":"a\u000ab","type":"t","code":null}})");
}

TEST(TranslateEnvelope, TranslatorsReturnTheCountsTheyWrote)
{
    std::string out;
    oai::Usage u;
    ASSERT_TRUE(llmbridge::provider::anthropic_to_openai_response(
        R"({"content":[{"type":"text","text":"x"}],"usage":{"input_tokens":5,)"
        R"("cache_read_input_tokens":3,"cache_creation_input_tokens":2,)"
        R"("cache_creation":{"ephemeral_1h_input_tokens":2},"output_tokens":9}})",
        out, u));
    EXPECT_EQ(u.in, 10);
    EXPECT_EQ(u.out, 9);
    EXPECT_EQ(u.cached, 3);
    EXPECT_EQ(u.cache_write, 2);
    EXPECT_EQ(u.cache_write_1h, 2);
    EXPECT_EQ(oai::scan_usage(out).in, u.in) << "the body and the counts agree";
    ASSERT_TRUE(llmbridge::provider::anthropic_to_openai_response(
        R"({"content":[{"type":"text","text":"x"}],"usage":{"input_tokens":5,"output_tokens":9,)"
        R"("output_tokens_details":{"thinking_tokens":6},)"
        R"("server_tool_use":{"web_search_requests":1},"inference_geo":"us"}})",
        out, u));
    EXPECT_EQ(u.reasoning, 6) << "Anthropic's thinking, as OpenAI's reasoning";
    EXPECT_EQ(oai::scan_usage(out).reasoning, 6) << "and in the body the client reads";
    EXPECT_EQ(u.web_search, 1);
    EXPECT_EQ(u.inference_geo.view(), "us");
    ASSERT_TRUE(llmbridge::provider::gemini_to_openai_response(
        R"({"usageMetadata":{"promptTokenCount":8,"candidatesTokenCount":3,)"
        R"("thoughtsTokenCount":2,"toolUsePromptTokenCount":1}})",
        out, u));
    EXPECT_EQ(u.out, 5);
    EXPECT_EQ(u.reasoning, 2);
    EXPECT_EQ(u.tool_prompt, 1);
    EXPECT_EQ(u.cached, -1) << "none written";
    EXPECT_FALSE(llmbridge::provider::cohere_to_openai_response("x", out, u));
    EXPECT_TRUE(out.empty());
}

// ── The message walk and the Anthropic parameter table ──────────────────────

namespace
{
    std::string anth(std::string_view body) { return openai_to_anthropic_request(body); }

    // The three tool constructs, each alone in an otherwise plain request.
    constexpr const char* kToolBodies[] = {
        R"({"model":"g","messages":[{"role":"user","content":"w?"}],
            "tools":[{"type":"function","function":{"name":"f"}}]})",
        R"({"model":"g","messages":[{"role":"user","content":"w?"},
            {"role":"assistant","content":null,"tool_calls":[
              {"id":"c","type":"function","function":{"name":"f","arguments":"{}"}}]}]})",
        R"({"model":"g","messages":[{"role":"tool","tool_call_id":"c","content":"sunny"}]})",
    };
} // namespace

// P9: Gemini has no tool hooks, so the request is refused instead of losing them.
TEST(MessageWalk, GeminiRefusesToolsInsteadOfDroppingThem)
{
    for (const char* body : kToolBodies) EXPECT_TRUE(openai_to_gemini_request(body).empty()) << body;
    // An empty or null `tools` declares nothing, and an empty tool_calls is a plain turn.
    EXPECT_FALSE(openai_to_gemini_request(
                     R"({"messages":[{"role":"user","content":"hi"}],"tools":[]})").empty());
    EXPECT_FALSE(openai_to_gemini_request(
                     R"({"messages":[{"role":"user","content":"hi"}],"tools":null})").empty());
    EXPECT_FALSE(openai_to_gemini_request(
                     R"({"messages":[{"role":"assistant","content":"x","tool_calls":[]}]})").empty());
}

// P8: the same for Cohere, which dropped the tools and sent tool messages with no id.
TEST(MessageWalk, CohereRefusesToolsInsteadOfDroppingThem)
{
    for (const char* body : kToolBodies) EXPECT_TRUE(openai_to_cohere_request(body).empty()) << body;
    EXPECT_FALSE(openai_to_cohere_request(
                     R"({"model":"c","messages":[{"role":"user","content":"hi"}],"tools":[]})").empty());
}

// P9: Gemini refuses an empty text part, so a message with no text is skipped.
TEST(MessageWalk, GeminiSkipsEmptyText)
{
    EXPECT_EQ(openai_to_gemini_request(R"({"messages":[
        {"role":"system","content":""},
        {"role":"user","content":"hi"},
        {"role":"assistant","content":""},
        {"role":"assistant","content":null},
        {"role":"user","content":[{"type":"text","text":""}]},
        {"role":"user","content":"again"}]})"),
              R"({"contents":[{"role":"user","parts":[{"text":"hi"}]},)"
              R"({"role":"user","parts":[{"text":"again"}]}]})");
    const Value v = P(openai_to_gemini_request(R"({"messages":[{"role":"system","content":"a"},
        {"role":"system","content":""},{"role":"system","content":"b"}]})"));
    EXPECT_EQ(v.find("systemInstruction")->find("parts")->arr[0].str_or("text"), "a\\nb");
}

// Anthropic refuses an empty text block, so the block form skips one.
TEST(MessageWalk, AnthropicBlockFormSkipsEmptyTextParts)
{
    const Value v = P(anth(R"({"model":"m","messages":[{"role":"user","content":[
        {"type":"text","text":""},{"type":"text","text":"a","cache_control":{"type":"ephemeral"}},
        {"type":"text","text":""}]}]})"));
    const Value* c = v.find("messages")->arr[0].find("content");
    ASSERT_TRUE(c && c->is_array());
    ASSERT_EQ(c->arr.size(), 1u);
    EXPECT_EQ(c->arr[0].str_or("text"), "a");
    // A breakpoint on an empty part alone keeps the string form.
    const Value w = P(anth(R"({"model":"m","messages":[{"role":"user","content":[
        {"type":"text","text":"a"},{"type":"text","text":"","cache_control":{"type":"ephemeral"}}]}]})"));
    EXPECT_EQ(w.find("messages")->arr[0].str_or("content"), "a");
}

// A missing or unknown role has no hook in any dialect.
TEST(MessageWalk, UnknownRoleIsRefusedEverywhere)
{
    for (const char* body : {R"({"model":"m","messages":[{"role":"function","name":"f","content":"x"}]})",
                             R"({"model":"m","messages":[{"content":"x"}]})",
                             R"({"model":"m","messages":["x"]})"})
    {
        EXPECT_TRUE(anth(body).empty()) << body;
        EXPECT_TRUE(openai_to_gemini_request(body).empty()) << body;
        EXPECT_TRUE(openai_to_cohere_request(body).empty()) << body;
    }
}

// P10: `developer` is the system prompt under a newer name, in every dialect.
TEST(MessageWalk, DeveloperRoleIsSystem)
{
    const std::string body = R"({"model":"m","messages":[{"role":"developer","content":"be brief"},
        {"role":"system","content":"and kind"},{"role":"user","content":"hi"}]})";
    const Value a = P(anth(body));
    EXPECT_EQ(a.str_or("system"), "be brief\\nand kind");
    EXPECT_EQ(a.find("messages")->arr.size(), 1u);
    const Value g = P(openai_to_gemini_request(body));
    EXPECT_EQ(g.find("systemInstruction")->find("parts")->arr[0].str_or("text"), "be brief\\nand kind");
    EXPECT_EQ(g.find("contents")->arr.size(), 1u);
    const Value c = P(openai_to_cohere_request(body));
    EXPECT_EQ(c.find("messages")->arr[0].str_or("role"), "system");
    EXPECT_EQ(c.find("messages")->arr[0].str_or("content"), "be brief");
}

// P10: max_completion_tokens replaced max_tokens, and wins when both are sent.
TEST(AnthropicParams, MaxCompletionTokensTakesPrecedence)
{
    EXPECT_EQ(P(anth(R"({"model":"m","max_tokens":10,"max_completion_tokens":4000,"messages":[]})"))
                  .num_or("max_tokens"), "4000");
    EXPECT_EQ(P(anth(R"({"model":"m","max_completion_tokens":4000,"messages":[]})")).num_or("max_tokens"),
              "4000");
    EXPECT_EQ(P(anth(R"({"model":"m","max_tokens":10,"max_completion_tokens":null,"messages":[]})"))
                  .num_or("max_tokens"), "10");
    const std::string both = R"({"model":"m","max_tokens":10,"max_completion_tokens":4000,"messages":[]})";
    EXPECT_EQ(P(openai_to_gemini_request(both)).find("generationConfig")->num_or("maxOutputTokens"), "4000");
    EXPECT_EQ(P(openai_to_cohere_request(both)).num_or("max_tokens"), "4000");
}

// P10: `stop` becomes `stop_sequences`, which is always an array.
TEST(AnthropicParams, StopBecomesStopSequences)
{
    const auto stops = [](std::string_view stop) {
        return anth(std::string(R"({"model":"m","messages":[],"stop":)") + std::string(stop) + "}");
    };
    EXPECT_NE(stops(R"("END")").find(R"("stop_sequences":["END"])"), std::string::npos);
    EXPECT_NE(stops(R"([ "a", "b\"c" ])").find(R"("stop_sequences":["a","b\"c"])"), std::string::npos);
    for (const char* none : {"null", "[]"})
    {
        EXPECT_FALSE(stops(none).empty()) << none;
        EXPECT_EQ(stops(none).find("stop"), std::string::npos) << none;
    }
    EXPECT_TRUE(stops("7").empty());
    EXPECT_TRUE(stops(R"(["a",1])").empty());
}

// P10: Anthropic's temperature range is 0 to 1, where OpenAI's reaches 2.
TEST(AnthropicParams, TemperatureAboveOneIsRefused)
{
    const auto temp = [](std::string_view t) {
        return anth(std::string(R"({"model":"m","messages":[],"temperature":)") + std::string(t) + "}");
    };
    for (const char* t : {"0", "-0", "0.0", "0.7", "1", "1.0", "1.000", "1e0", "10e-1", "100E-2",
                          "0.1e+1", "0e5", "5e-1", "0.000001e6", "0.0000000001"})
        EXPECT_NE(temp(t).find(std::string("\"temperature\":") + t), std::string::npos) << t;
    for (const char* t : {"1.0001", "1.5", "2", "10", "1e1", "1e+0001", "0.15e1", "0.0000011e6",
                          "1.00000000000000000001", "-0.5", "-1", "9e-0"})
        EXPECT_TRUE(temp(t).empty()) << t;
}

// P10: parallel_tool_calls:false is disable_parallel_tool_use, inside tool_choice.
TEST(AnthropicParams, ParallelToolCallsFalseDisablesParallelUse)
{
    const auto with = [](std::string_view extra) {
        return anth(std::string(R"({"model":"m","messages":[],)"
                                R"("tools":[{"type":"function","function":{"name":"f"}}])") +
                    std::string(extra) + "}");
    };
    EXPECT_NE(with(R"(,"parallel_tool_calls":false)")
                  .find(R"("tool_choice":{"type":"auto","disable_parallel_tool_use":true})"), std::string::npos);
    EXPECT_NE(with(R"(,"parallel_tool_calls":false,"tool_choice":"required")")
                  .find(R"("tool_choice":{"type":"any","disable_parallel_tool_use":true})"), std::string::npos);
    EXPECT_NE(with(R"(,"parallel_tool_calls":false,"tool_choice":{"type":"function","function":{"name":"f"}})")
                  .find(R"("tool_choice":{"type":"tool","name":"f","disable_parallel_tool_use":true})"),
              std::string::npos);
    // Anthropic's "none" takes no such field, and true is the default.
    EXPECT_NE(with(R"(,"parallel_tool_calls":false,"tool_choice":"none")")
                  .find(R"("tool_choice":{"type":"none"})"), std::string::npos);
    EXPECT_EQ(with(R"(,"parallel_tool_calls":true)").find("tool_choice"), std::string::npos);
    EXPECT_EQ(anth(R"({"model":"m","messages":[],"parallel_tool_calls":false})").find("tool_choice"), std::string::npos);
}

// P11: tool_choice "none" keeps the tools that a tool_use history needs.
TEST(AnthropicParams, ToolChoiceNoneKeepsToolsForAToolHistory)
{
    const Value v = P(anth(R"({"model":"m","tool_choice":"none",
      "tools":[{"type":"function","function":{"name":"f"}}],"messages":[
      {"role":"assistant","content":null,"tool_calls":[
        {"id":"c","type":"function","function":{"name":"f","arguments":"{}"}}]},
      {"role":"tool","tool_call_id":"c","content":"r"}]})"));
    ASSERT_NE(v.find("tools"), nullptr);
    EXPECT_EQ(v.find("tools")->arr[0].str_or("name"), "f");
    ASSERT_NE(v.find("tool_choice"), nullptr);
    EXPECT_EQ(v.find("tool_choice")->str_or("type"), "none");
}

// P10: no default model. The old one was a retired alias, and on Bedrock it named a
// path that does not exist instead of letting the gateway refuse.
TEST(AnthropicParams, NoModelIsRefusedNotDefaulted)
{
    for (const char* body : {R"({"messages":[]})", R"({"model":7,"messages":[]})",
                             R"({"model":"","messages":[]})", R"({"model":null,"messages":[]})"})
    {
        EXPECT_TRUE(anth(body).empty()) << body;
        std::string model, out;
        EXPECT_FALSE(llmbridge::provider::openai_to_bedrock_request(body, model, out)) << body;
        EXPECT_TRUE(out.empty()) << body;
        EXPECT_TRUE(openai_to_cohere_request(body).empty()) << body;
    }
    // Gemini names its model in the path, never in the body.
    EXPECT_FALSE(openai_to_gemini_request(R"({"messages":[]})").empty());
}

// PP1: arguments are decoded into the output and parsed there, escapes and all.
TEST(ToolReq, ArgumentsDecodeInPlaceWithEveryEscape)
{
    const Value v = P(anth(R"({"model":"m","messages":[{"role":"assistant","content":null,
      "tool_calls":[{"id":"a","type":"function","function":{"name":"f",
      "arguments":"{\"q\":\"\\u00e9 \\\\ \\/ \\n\",\"r\":\"plain run\"}"}}]}]})"));
    const Value* in = v.find("messages")->arr[0].find("content")->arr[0].find("input");
    ASSERT_NE(in, nullptr);
    EXPECT_EQ(in->sv, R"({"q":"\u00e9 \\ \/ \n","r":"plain run"})");
}

// ── The venue table ───────────────────────────────────────────────────────────

// Every finish reason each provider documents, and what it reads as. A value missing
// here fell through to "stop": an Anthropic refusal or a Gemini SPII block looked like
// a finished answer, and a Cohere ERROR or a Gemini MALFORMED_FUNCTION_CALL like a
// success. A failure (null here) is no completion at all: the gateway answers 502.
TEST(FinishTables, EveryDocumentedValue)
{
    struct Case { const char* reason; const char* finish; };
    const Case anthropic[] = {{"end_turn", "stop"}, {"stop_sequence", "stop"}, {"pause_turn", "stop"},
                              {"max_tokens", "length"}, {"model_context_window_exceeded", "length"},
                              {"tool_use", "tool_calls"}, {"refusal", "content_filter"}};
    for (const Case& c : anthropic)
    {
        const std::string in = std::string(R"({"content":[{"type":"text","text":"x"}],"stop_reason":")") +
                               c.reason + "\"}";
        Value out = P(anthropic_to_openai_response(in));
        EXPECT_EQ(out.find("choices")->arr[0].str_or("finish_reason"), c.finish) << c.reason;
    }
    const Case gemini[] = {{"STOP", "stop"}, {"FINISH_REASON_UNSPECIFIED", "stop"}, {"OTHER", "stop"},
                           {"MAX_TOKENS", "length"}, {"SAFETY", "content_filter"},
                           {"RECITATION", "content_filter"}, {"LANGUAGE", "content_filter"},
                           {"BLOCKLIST", "content_filter"}, {"PROHIBITED_CONTENT", "content_filter"},
                           {"SPII", "content_filter"}, {"IMAGE_SAFETY", "content_filter"},
                           {"MALFORMED_FUNCTION_CALL", nullptr}, {"UNEXPECTED_TOOL_CALL", nullptr},
                           {"TOO_MANY_TOOL_CALLS", nullptr}};
    for (const Case& c : gemini)
    {
        const std::string in = std::string(R"({"candidates":[{"content":{"parts":[{"text":"x"}]},"finishReason":")") +
                               c.reason + "\"}]}";
        if (!c.finish) { EXPECT_TRUE(gemini_to_openai_response(in).empty()) << c.reason; continue; }
        Value out = P(gemini_to_openai_response(in));
        EXPECT_EQ(out.find("choices")->arr[0].str_or("finish_reason"), c.finish) << c.reason;
    }
    const Case cohere[] = {{"COMPLETE", "stop"}, {"STOP_SEQUENCE", "stop"}, {"MAX_TOKENS", "length"},
                           {"TOOL_CALL", "tool_calls"}, {"ERROR", nullptr}, {"TIMEOUT", nullptr}};
    for (const Case& c : cohere)
    {
        const std::string in = std::string(R"({"message":{"content":[{"type":"text","text":"x"}]},"finish_reason":")") +
                               c.reason + "\"}";
        if (!c.finish) { EXPECT_TRUE(cohere_to_openai_response(in).empty()) << c.reason; continue; }
        Value out = P(cohere_to_openai_response(in));
        EXPECT_EQ(out.find("choices")->arr[0].str_or("finish_reason"), c.finish) << c.reason;
    }
}

// A prompt Gemini blocks has no candidate, only promptFeedback.blockReason: it read as a
// successful empty answer.
TEST(FinishTables, ABlockedGeminiPromptIsAContentFilter)
{
    Value out = P(gemini_to_openai_response(R"({"promptFeedback":{"blockReason":"SAFETY"}})"));
    EXPECT_EQ(out.find("choices")->arr[0].str_or("finish_reason"), "content_filter");
}

// Gemini names the model in the path. It was always `models/gemini`, which names no model.
TEST(VenueTargets, GeminiAndBedrockPutTheModelInThePath)
{
    using llmbridge::provider::spec;
    using llmbridge::provider::Venue;
    std::string path;
    ASSERT_TRUE(spec(Venue::Gemini).target("gemini-2.5-flash", path));
    EXPECT_EQ(path, "/v1beta/models/gemini-2.5-flash:generateContent");
    path.clear();
    ASSERT_TRUE(spec(Venue::Gemini).target("models/gemini-2.5-pro", path));
    EXPECT_EQ(path, "/v1beta/models/gemini-2.5-pro:generateContent") << "the API's own prefix is not doubled";
    path.clear();
    EXPECT_FALSE(spec(Venue::Gemini).target("", path)) << "no model names no path";
    path.clear();
    ASSERT_TRUE(spec(Venue::Bedrock).target(R"(arn:aws:bedrock:us-east-1::inference-profile\/us.x)", path));
    EXPECT_EQ(path, "/model/arn%3Aaws%3Abedrock%3Aus-east-1%3A%3Ainference-profile%2Fus.x/invoke")
        << "decoded from its JSON form, then one segment";
    path.clear();
    ASSERT_TRUE(spec(Venue::Anthropic).target("anything", path));
    EXPECT_EQ(path, "/v1/messages");
}

// The request translators read a body parsed once; the override reaches them through
// RequestOut, and the result is what a splice and a second parse produced.
TEST(VenueTargets, AnOverrideThroughRequestOutMatchesTheSplice)
{
    using llmbridge::provider::RequestOut;
    using llmbridge::provider::spec;
    using llmbridge::provider::Venue;
    const std::string body = R"({"model":"gpt-4o","max_tokens":9,"messages":[{"role":"user","content":"hi"}]})";
    for (const Venue v : {Venue::Anthropic, Venue::Gemini, Venue::Cohere, Venue::Bedrock, Venue::Azure})
    {
        bool ok = false;
        const Value in = parse(body, ok);
        ASSERT_TRUE(ok);
        std::string out;
        RequestOut o{out, body, "claude-x"};
        ASSERT_TRUE(spec(v).request(in, o)) << spec(v).name;
        const std::string spliced = llmbridge::provider::rewrite_model(body, "claude-x");
        Value sp = P(spliced);
        std::string expect;
        RequestOut e{expect, spliced};
        ASSERT_TRUE(spec(v).request(sp, e));
        EXPECT_EQ(out, expect) << spec(v).name;
    }
}

// ── Gemini: what a reply states beside its usage ─────────────────────────────

TEST(GeminiFacts, PartsSayTokenThinkingAndTools)
{
    namespace p = llmbridge::provider;
    p::GeminiFacts f;
    ASSERT_TRUE(p::gemini_facts(R"({"candidates": [{"content": {"parts": [{"text": "", "thoughtSignature": "x"}]}}]})", f));
    EXPECT_FALSE(f.token) << "an empty text part with a signature";
    EXPECT_FALSE(f.thinking);
    f = {};
    ASSERT_TRUE(p::gemini_facts(R"({"candidates": [{"content": {"parts": [{"text": "hmm", "thought": true}]}}]})", f));
    EXPECT_TRUE(f.thinking);
    EXPECT_FALSE(f.token) << "a thought is not shown";
    f = {};
    ASSERT_TRUE(p::gemini_facts(R"({"candidates": [{"content": {"parts": [{"functionCall": {"name": "f"}}]}}]})", f));
    EXPECT_TRUE(f.token);
    f = {};
    ASSERT_TRUE(p::gemini_facts(
        R"({"candidates": [{"content": {"parts": [{"executableCode": {"code": "1"}}]}}, {"groundingMetadata": )"
        R"({"webSearchQueries": ["a"]}}, {"groundingMetadata": {"webSearchQueries": ["b", "c"]}}],)"
        R"("usageMetadata": {"serviceTier": "flex"}, "modelVersion": "gemini-3.8-flash", "responseId": "r"})",
        f));
    EXPECT_TRUE(f.tool_input);
    EXPECT_EQ(f.search_queries, 3) << "every candidate's";
    EXPECT_EQ(f.tier, "flex");
    EXPECT_EQ(f.model_version, "gemini-3.8-flash");
    EXPECT_EQ(f.response_id, "r");
    f = {};
    ASSERT_TRUE(p::gemini_facts(R"({"error": {"code": 503, "message": "m", "status": "UNAVAILABLE"}})", f));
    EXPECT_TRUE(f.error);
    EXPECT_EQ(f.error_status, "UNAVAILABLE");
    EXPECT_FALSE(p::gemini_facts("[{}]", f));
    EXPECT_FALSE(p::gemini_facts("{\"candidates\": [", f));
}

TEST(GeminiUsage, AudioAndAThoughtsCountTheTotalHolds)
{
    namespace oai = llmbridge::provider::openai;
    const oai::Usage u = oai::scan_usage(
        R"({"usageMetadata": {"promptTokenCount": 20, "candidatesTokenCount": 5, "totalTokenCount": 33,)"
        R"( "promptTokensDetails": [{"modality": "TEXT", "tokenCount": 8}, {"modality": "AUDIO", "tokenCount": 12}],)"
        R"( "candidatesTokensDetails": [{"modality": "AUDIO", "tokenCount": 5}]}})");
    EXPECT_EQ(u.in, 20);
    EXPECT_EQ(u.reasoning, 8) << "33 - 20 - 5, thoughts left out";
    EXPECT_EQ(u.out, 13);
    EXPECT_EQ(u.audio_in, 12);
    EXPECT_EQ(u.audio_out, 5);
    const oai::Usage t = oai::scan_usage(
        R"({"usageMetadata": {"promptTokenCount": 20, "candidatesTokenCount": 5, "toolUsePromptTokenCount": 8,)"
        R"( "totalTokenCount": 33}})");
    EXPECT_EQ(t.reasoning, -1) << "the remainder is the tool prompt";
    EXPECT_EQ(t.out, 5);
}

TEST(ScanErrorType, GooglesStatusPastALongMessage)
{
    namespace oai = llmbridge::provider::openai;
    const std::string g = R"({"error": {"code": 429, "message": ")" + std::string(2000, 'x') +
                          R"(", "status": "RESOURCE_EXHAUSTED"}})";
    EXPECT_EQ(oai::scan_error_type(g), "RESOURCE_EXHAUSTED");
    EXPECT_EQ(oai::scan_error_type(R"({"type":"error","error":{"type":"overloaded_error","message":"x"}})"),
              "overloaded_error");
    EXPECT_EQ(oai::scan_error_type(R"({"error":{"code":"rate_limit_exceeded","status":"S"}})"), "rate_limit_exceeded");
}

TEST(GeminiPathModel, TheSegmentAfterTheLastModels)
{
    namespace p = llmbridge::provider;
    EXPECT_EQ(p::gemini_path_model("/v1beta/models/gemini-4-argon:generateContent").model, "gemini-4-argon");
    EXPECT_EQ(p::gemini_path_model("/v1/projects/p/locations/global/publishers/google/models/m-1:streamGenerateContent"
                                   "?alt=sse").model,
              "m-1");
    EXPECT_EQ(p::gemini_path_model("/v1beta/tunedModels/x:generateContent").at, std::string_view::npos);
    EXPECT_EQ(p::gemini_path_model("/v1beta/models/a:b?x=/models/c:d").model, "a") << "never the query";
    EXPECT_TRUE(p::path_model_ok("gemini-3.1-pro-preview"));
    EXPECT_FALSE(p::path_model_ok("a/b"));
    EXPECT_FALSE(p::path_model_ok("a%2Fb"));
    EXPECT_FALSE(p::path_model_ok(""));
    EXPECT_FALSE(p::path_model_ok(std::string(256, 'a')));
}
