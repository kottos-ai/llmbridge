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
#include "provider/translate.hpp" // cross-path reconstruction vs the whole-body translator

#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "provider/json.hpp"

using llmbridge::provider::AnthropicToOpenAiSse;
using llmbridge::provider::json::parse;
using llmbridge::provider::json::Value;

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

    // A fixed `created` so byte-exact comparisons are deterministic (the wall
    // clock would otherwise make two runs differ across a 1-second boundary).
    constexpr long long kFixedCreated = 1785280000;

    // Feed the whole input in one shot.
    std::string translate_whole(std::string_view in)
    {
        AnthropicToOpenAiSse t(kFixedCreated);
        std::string out;
        t.feed(in, out);
        t.finish(out);
        return out;
    }

    // Feed the input one byte at a time: worst-case fragmentation.
    std::string translate_byte_by_byte(std::string_view in)
    {
        AnthropicToOpenAiSse t(kFixedCreated);
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

// content_started() is the TTFT signal: it must stay false through message_start,
// content_block_start and the role delta, and flip exactly when the first text
// token is emitted. A gateway stamps its clock the moment this turns true.
TEST(Sse, ContentStartedLatchesOnFirstToken)
{
    AnthropicToOpenAiSse t(kFixedCreated);
    std::string out;
    EXPECT_FALSE(t.content_started());
    t.feed("event: message_start\n"
           "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"model\":\"x\","
           "\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}}\n\n", out);
    EXPECT_FALSE(t.content_started()) << "the head/usage is not a token";
    t.feed("data: {\"type\":\"content_block_start\",\"index\":0,"
           "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n", out);
    EXPECT_FALSE(t.content_started()) << "opening a text block emits no token yet";
    t.feed("data: {\"type\":\"content_block_delta\",\"index\":0,"
           "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi\"}}\n\n", out);
    EXPECT_TRUE(t.content_started()) << "the first text delta is the first token";
}

// A tool-only reply has no text; its first token is the tool call being opened.
TEST(Sse, ContentStartedLatchesOnFirstToolCall)
{
    AnthropicToOpenAiSse t(kFixedCreated);
    std::string out;
    t.feed("data: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"model\":\"x\"}}\n\n", out);
    EXPECT_FALSE(t.content_started());
    t.feed("data: {\"type\":\"content_block_start\",\"index\":0,"
           "\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"lookup\"}}\n\n", out);
    EXPECT_TRUE(t.content_started());
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

// ── Coverage: robustness, framing, mapping, caps ────────────────────────────

namespace
{
    // The invariant append_sanitized guarantees: our output carries no bare C0
    // control byte other than the '\n' we frame with. (We never emit '\r'.)
    bool output_is_strict(const std::string& out)
    {
        for (unsigned char c : out)
            if (c < 0x20 && c != '\n') return false;
        return true;
    }

    std::string crlf(std::string_view s) // rewrite LF -> CRLF
    {
        std::string o;
        for (char c : s) { if (c == '\n') o += '\r'; o += c; }
        return o;
    }
} // namespace

// The property that matters: for ANY input (hostile or malformed), the emitted
// stream is strict (no bare control bytes leak through passthrough) and the
// translator never crashes (ASan/UBSan enforce the latter in CI).
TEST(Sse, OutputIsAlwaysStrict)
{
    std::vector<std::string> evil;
    // control bytes inside a text_delta
    evil.push_back(std::string("data: {\"type\":\"content_block_delta\",\"delta\":"
                               "{\"type\":\"text_delta\",\"text\":\"a") + char(0x01) + "b" + char(0x1f)
                   + "c\"}}\n\n");
    // control bytes inside id / model (stored across chunks)
    evil.push_back(std::string("data: {\"type\":\"message_start\",\"message\":{\"id\":\"i") + char(0x02)
                   + "d\",\"model\":\"m" + char(0x1b) + "l\"}}\n\n"
                     "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"ok\"}}\n\n");
    // raw newlines that try to smuggle a nested event
    evil.emplace_back("data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"x\n"
                      "\ndata: junk\"}}\n\n");
    // outright garbage
    evil.emplace_back("data: not json at all\n\n");
    evil.emplace_back(kAnthropicText); // and the well-formed one

    for (const auto& in : evil)
    {
        std::string whole;
        {
            AnthropicToOpenAiSse t;
            t.feed(in, whole);
            t.finish(whole);
        }
        EXPECT_TRUE(output_is_strict(whole)) << "leaked a bare control byte";

        std::string bybyte; // and under worst-case fragmentation
        {
            AnthropicToOpenAiSse t;
            for (char c : in) t.feed(std::string_view(&c, 1), bybyte);
            t.finish(bybyte);
        }
        EXPECT_TRUE(output_is_strict(bybyte));
    }
}

TEST(Sse, CrlfLineEndingsMatchLf)
{
    EXPECT_EQ(translate_whole(crlf(kAnthropicText)), translate_whole(kAnthropicText));
}

TEST(Sse, PingCommentUnknownProduceNoOutput)
{
    AnthropicToOpenAiSse t;
    std::string out;
    t.feed(": a comment line\n\n"
           "event: ping\ndata: {\"type\":\"ping\"}\n\n"
           "data: {\"type\":\"some_unknown_event\",\"index\":9}\n\n",
           out);
    EXPECT_TRUE(out.empty()); // none of these map to an OpenAI chunk
    // ...and the stream still works afterward:
    t.feed("data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n", out);
    EXPECT_NE(out.find("\"content\":\"hi\""), std::string::npos);
}

TEST(Sse, GarbledFrameIsSkippedStreamContinues)
{
    const char* s =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\",\"model\":\"x\"}}\n\n"
        "data: {this is : not, valid json]\n\n" // garbage between good frames
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"OK\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    const auto p = data_payloads(translate_whole(s));
    // role chunk, "OK", finish chunk, [DONE]; the garbage produced nothing.
    ASSERT_EQ(p.size(), 4u);
    EXPECT_EQ(P(p[1]).find("choices")->arr[0].find("delta")->str_or("content"), "OK");
    EXPECT_EQ(p.back(), "[DONE]");
}

TEST(Sse, MultiLineDataIsJoined)
{
    // A JSON object split across two data: lines (joined by '\n', which our JSON
    // parser treats as whitespace between tokens).
    const char* s =
        "data: {\"type\":\n"
        "data: \"message_stop\"}\n\n";
    EXPECT_EQ(data_payloads(translate_whole(s)).back(), "[DONE]");
}

TEST(Sse, StopSequenceMapsToStop)
{
    const char* s =
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"stop_sequence\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    const auto p = data_payloads(translate_whole(s));
    EXPECT_EQ(P(p[p.size() - 2]).find("choices")->arr[0].str_or("finish_reason"), "stop");
}

TEST(Sse, MultipleTextBlocksAllFlow)
{
    const char* s =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"model\":\"x\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"A\"}}\n\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"B\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    const auto p = data_payloads(translate_whole(s));
    std::string content;
    for (const auto& pay : p)
    {
        if (pay == "[DONE]") continue;
        Value chunk = P(pay); // keep the DOM alive; find() returns views into it
        if (const Value* d = chunk.find("choices")->arr[0].find("delta"))
            content += std::string(d->str_or("content"));
    }
    EXPECT_EQ(content, "AB");
}

TEST(Sse, EmptyTextDeltaIsBenign)
{
    AnthropicToOpenAiSse t;
    std::string out;
    t.feed("data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"\"}}\n\n", out);
    EXPECT_TRUE(output_is_strict(out));
    EXPECT_NE(out.find("\"content\":\"\""), std::string::npos); // empty content, well-formed
}

TEST(Sse, NoSpaceAfterDataColon)
{
    // SSE allows zero or one space after "data:"; the payload must still parse.
    EXPECT_EQ(data_payloads(translate_whole("data:{\"type\":\"message_stop\"}\n\n")).back(), "[DONE]");
}

TEST(Sse, MissingIdModelFallBack)
{
    const char* s =
        "data: {\"type\":\"message_start\",\"message\":{}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n";
    Value first = P(data_payloads(translate_whole(s))[0]);
    EXPECT_EQ(first.str_or("id"), "chatcmpl-llmbridge"); // default id
    EXPECT_EQ(first.str_or("model"), "");                // empty, not garbage
}

TEST(Sse, CreatedIsStableAcrossChunks)
{
    std::string_view created;
    bool first = true;
    for (const auto& pay : data_payloads(translate_whole(kAnthropicText)))
    {
        if (pay == "[DONE]") continue;
        std::string_view c = P(pay).num_or("created");
        if (first) { created = c; first = false; }
        else EXPECT_EQ(c, created); // OpenAI keeps `created` constant across a stream
    }
    EXPECT_FALSE(created.empty());
}

TEST(Sse, CapOnEndlessLineRejectsAndIsSticky)
{
    AnthropicToOpenAiSse t;
    std::string out;
    std::string endless(AnthropicToOpenAiSse::kMaxPending + 64, 'a'); // one line, no '\n'
    EXPECT_FALSE(t.feed(endless, out));      // over the line cap
    std::string out2;
    EXPECT_FALSE(t.feed("more", out2));      // sticky: stays dead
    std::string out3;
    EXPECT_FALSE(t.finish(out3));            // and won't fabricate a clean [DONE]
}

TEST(Sse, CapOnEndlessEventRejects)
{
    AnthropicToOpenAiSse t;
    std::string out;
    // one complete data: line whose payload exceeds the per-event cap
    std::string huge = "data: " + std::string(AnthropicToOpenAiSse::kMaxEvent + 64, 'a') + "\n";
    EXPECT_FALSE(t.feed(huge, out));
}

// ── stream_options.include_usage ───────────────────────────────────────────
namespace
{
    // Anthropic reports input tokens in message_start and CUMULATIVE output
    // tokens in message_delta; this stream ends at 7 output tokens.
    const char* kAnthropicWithUsage =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_u\",\"model\":\"claude-3-5-sonnet\","
        "\"usage\":{\"input_tokens\":11,\"output_tokens\":1}}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":7}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";

    std::string translate_usage(std::string_view in, bool include_usage)
    {
        AnthropicToOpenAiSse t(kFixedCreated, include_usage);
        std::string out;
        t.feed(in, out);
        t.finish(out);
        return out;
    }
} // namespace

TEST(SseUsage, CacheReadTokensSurfaceInAccessorAndUsageChunk)
{
    const char* stream =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_c\",\"model\":\"claude-3-5-sonnet\","
        "\"usage\":{\"input_tokens\":100,\"output_tokens\":1,\"cache_read_input_tokens\":80}}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":5}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    AnthropicToOpenAiSse t(kFixedCreated, /*include_usage=*/true);
    std::string out;
    t.feed(stream, out);
    t.finish(out);
    EXPECT_EQ(t.cached_tokens(), 80);  // the accessor the tape reads

    const auto p = data_payloads(out);
    Value u = P(p[p.size() - 2]);
    const Value* usage = u.find("usage");
    ASSERT_NE(usage, nullptr);
    const Value* det = usage->find("prompt_tokens_details");
    ASSERT_NE(det, nullptr);
    EXPECT_EQ(det->num_or("cached_tokens"), "80");
}

TEST(SseUsage, NoCacheReadEmitsNoDetails)
{
    const auto p = data_payloads(translate_usage(kAnthropicWithUsage, /*include_usage=*/true));
    ASSERT_GE(p.size(), 2u);
    Value u = P(p[p.size() - 2]);
    const Value* usage = u.find("usage");
    ASSERT_NE(usage, nullptr);
    EXPECT_EQ(usage->find("prompt_tokens_details"), nullptr);
}

TEST(SseUsage, EmitsFinalUsageChunkBeforeDone)
{
    const auto p = data_payloads(translate_usage(kAnthropicWithUsage, /*include_usage=*/true));
    ASSERT_GE(p.size(), 2u);
    EXPECT_EQ(p.back(), "[DONE]");           // sentinel is last...
    Value u = P(p[p.size() - 2]);            // ...and the usage chunk precedes it
    const Value* usage = u.find("usage");
    ASSERT_NE(usage, nullptr);
    EXPECT_EQ(usage->num_or("prompt_tokens"), "11");     // from message_start
    EXPECT_EQ(usage->num_or("completion_tokens"), "7");  // cumulative, from message_delta
    EXPECT_EQ(usage->num_or("total_tokens"), "18");
    // Per the OpenAI spec the usage chunk carries an EMPTY choices array.
    const Value* ch = u.find("choices");
    ASSERT_TRUE(ch && ch->is_array());
    EXPECT_TRUE(ch->arr.empty());
}

TEST(SseUsage, NormalChunksCarryNullUsageWhenRequested)
{
    for (const auto& pay : data_payloads(translate_usage(kAnthropicWithUsage, true)))
    {
        if (pay == "[DONE]") continue;
        Value v = P(pay);
        const Value* ch = v.find("choices");
        if (ch && ch->is_array() && !ch->arr.empty()) // a normal (non-usage) chunk
        {
            const Value* u = v.find("usage");
            ASSERT_NE(u, nullptr) << "include_usage puts a usage key on every chunk";
            EXPECT_EQ(u->type, Value::Type::Null) << "...and it is null on normal chunks";
        }
    }
}

TEST(SseUsage, OmittedEntirelyWhenNotRequested)
{
    const std::string out = translate_usage(kAnthropicWithUsage, /*include_usage=*/false);
    EXPECT_EQ(out.find("\"usage\""), std::string::npos) << "no usage key unless asked";
    const auto p = data_payloads(out);
    EXPECT_EQ(p.back(), "[DONE]");
    EXPECT_EQ(p.size(), 4u); // role, content, finish, [DONE]: no extra chunk
}

TEST(SseUsage, EmittedOnceEvenAtEofWithoutMessageStop)
{
    // Truncated stream: finish() must still produce usage-then-[DONE], exactly once.
    std::string in(kAnthropicWithUsage);
    in.erase(in.find("data: {\"type\":\"message_stop\"")); // drop message_stop
    const std::string out = translate_usage(in, true);
    EXPECT_EQ(out.find("[DONE]"), out.rfind("[DONE]"));               // one sentinel
    EXPECT_EQ(out.find("prompt_tokens"), out.rfind("prompt_tokens")); // one usage chunk
    const auto p = data_payloads(out);
    EXPECT_EQ(p.back(), "[DONE]");
    EXPECT_NE(P(p[p.size() - 2]).find("usage"), nullptr);
}

TEST(SseUsage, MissingUpstreamUsageYieldsZeros)
{
    // No usage anywhere upstream: still emit a well-formed chunk (zeros), never an
    // estimate and never a malformed one.
    const char* no_usage =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"model\":\"x\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    const auto p = data_payloads(translate_usage(no_usage, true));
    Value u = P(p[p.size() - 2]);
    const Value* usage = u.find("usage");
    ASSERT_NE(usage, nullptr);
    EXPECT_EQ(usage->num_or("prompt_tokens"), "0");
    EXPECT_EQ(usage->num_or("total_tokens"), "0");
}

// ── Cross-path "reconstruction": stream-then-reassemble == translate-the-whole-body ─
//
// We have only the forward direction (Anthropic->OpenAI), so a true round-trip
// isn't possible yet. The strong analog is that the STREAMING path and the
// NON-STREAMING path must agree: translating an Anthropic SSE stream and
// reassembling its OpenAI chunks must recover exactly what
// anthropic_to_openai_response() produces for the equivalent whole body. This
// pins the two production paths together (the reason the shared helpers were
// extracted) and runs over the same nasty escaping/UTF-8 payloads the request
// reconstruction uses, which proves the streaming raw-span passthrough is byte-faithful.

namespace
{
    using llmbridge::provider::anthropic_to_openai_response;

    // Anthropic non-streaming response with a single text block == `text_escaped`.
    std::string anthropic_body(const std::string& id, const std::string& model,
                               const std::string& text_escaped, const std::string& stop)
    {
        return "{\"id\":\"" + id + "\",\"model\":\"" + model +
               "\",\"content\":[{\"type\":\"text\",\"text\":\"" + text_escaped +
               "\"}],\"stop_reason\":\"" + stop +
               "\",\"usage\":{\"input_tokens\":3,\"output_tokens\":5}}";
    }

    // The equivalent Anthropic SSE stream: same id/model/stop, content delivered
    // as `deltas` (each a valid escaped string, as Anthropic sends them).
    std::string anthropic_stream(const std::string& id, const std::string& model,
                                 const std::vector<std::string>& deltas, const std::string& stop)
    {
        std::string s = "data: {\"type\":\"message_start\",\"message\":{\"id\":\"" + id +
                        "\",\"model\":\"" + model +
                        "\",\"usage\":{\"input_tokens\":3,\"output_tokens\":1}}}\n\n"
                        "data: {\"type\":\"content_block_start\",\"index\":0,"
                        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
        for (const auto& d : deltas)
            s += "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
                 "{\"type\":\"text_delta\",\"text\":\"" + d + "\"}}\n\n";
        s += "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
             "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"" + stop +
             "\"},\"usage\":{\"output_tokens\":5}}\n\n"
             "data: {\"type\":\"message_stop\"}\n\n";
        return s;
    }

    struct Reassembled
    {
        std::string id, model, content, finish;
        std::string prompt_tokens, completion_tokens, total_tokens; // from the usage chunk
    };

    // Fold the emitted OpenAI chunk stream back into a whole completion.
    Reassembled reassemble(const std::string& out)
    {
        Reassembled r;
        bool first = true;
        for (const auto& pay : data_payloads(out))
        {
            if (pay == "[DONE]") continue;
            Value ch = P(pay); // named local: keep the DOM alive for find()
            if (first) { r.id.assign(ch.str_or("id")); r.model.assign(ch.str_or("model")); first = false; }
            // The usage-only chunk has an EMPTY choices array; harvest it.
            if (const Value* u = ch.find("usage"); u && u->is_object())
            {
                r.prompt_tokens.assign(u->num_or("prompt_tokens"));
                r.completion_tokens.assign(u->num_or("completion_tokens"));
                r.total_tokens.assign(u->num_or("total_tokens"));
            }
            const Value* choices = ch.find("choices");
            if (!choices || !choices->is_array() || choices->arr.empty()) continue;
            const Value& c0 = choices->arr[0];
            if (const Value* d = c0.find("delta")) r.content += std::string(d->str_or("content"));
            if (const std::string_view fr = c0.str_or("finish_reason"); !fr.empty()) r.finish.assign(fr);
        }
        return r;
    }

    // Same as translate_whole, but asking for the final usage chunk.
    std::string translate_whole_usage(std::string_view in)
    {
        AnthropicToOpenAiSse t(kFixedCreated, /*include_usage=*/true);
        std::string out;
        t.feed(in, out);
        t.finish(out);
        return out;
    }

    struct SsePayload { const char* name; std::string body; };
    std::vector<SsePayload> sse_payloads()
    {
        return {
            {"plain", "hello world this is ordinary"},
            {"escaped_quotes", R"(she said \"hi there\" then \"bye\")"},
            {"backslashes", R"(path C:\\Users\\admin\\f.txt unc \\\\srv\\share)"},
            {"escaped_newlines", R"(one\ntwo\nthree\nfour)"},
            {"tabs_cr", R"(a\tb\tc\r\nd)"},
            {"unicode_escapes", R"(café naïve 中文 ¡hola! — dash)"},
            {"json_inside_string", R"({\"nested\":{\"k\":[1,2,3]}} as text)"},
            {"all_escapes_mixed", R"(q=\" b=\\ n=\n t=\t r=\r done)"},
            {"emoji_literal_utf8", "rocket \xF0\x9F\x9A\x80 sushi \xF0\x9F\x8D\xA3 done"},
            {"adjacent_escapes", R"(\\\"\\\"\n\n\t\tABC)"},
            {"only_escaped_quote", R"(\")"},
        };
    }
} // namespace

TEST(SseReconstruction, StreamReassemblesToWholeTranslation)
{
    const std::string id = "msg_01", model = "claude-3-5-sonnet-20241022";
    for (const auto& p : sse_payloads())
    {
        SCOPED_TRACE(p.name);
        const std::string whole = anthropic_to_openai_response(anthropic_body(id, model, p.body, "end_turn"));
        ASSERT_FALSE(whole.empty());
        Value w = P(whole);
        const Value* wmsg = w.find("choices")->arr[0].find("message");

        Reassembled r = reassemble(translate_whole(anthropic_stream(id, model, {p.body}, "end_turn")));
        // The bytes both paths pass through must be identical (raw escaped span).
        EXPECT_EQ(r.content, std::string(wmsg->str_or("content")));
        EXPECT_EQ(r.id, std::string(w.str_or("id")));
        EXPECT_EQ(r.model, std::string(w.str_or("model")));
        EXPECT_EQ(r.finish, std::string(w.find("choices")->arr[0].str_or("finish_reason")));
    }
}

// The usage numbers must survive the streaming path identically to the whole-body
// path: same provider counts, same OpenAI field names. (This closes the gap the
// original reconstruction test could only note; streaming now emits usage.)
TEST(SseReconstruction, UsageMatchesWholeTranslation)
{
    const std::string id = "msg_01", model = "claude-3-5-sonnet-20241022";
    Value w = P(anthropic_to_openai_response(anthropic_body(id, model, "hi", "end_turn")));
    const Value* wu = w.find("usage");
    ASSERT_NE(wu, nullptr);

    const Reassembled r =
        reassemble(translate_whole_usage(anthropic_stream(id, model, {"hi"}, "end_turn")));
    EXPECT_EQ(r.prompt_tokens, std::string(wu->num_or("prompt_tokens")));
    EXPECT_EQ(r.completion_tokens, std::string(wu->num_or("completion_tokens")));
    EXPECT_EQ(r.total_tokens, std::string(wu->num_or("total_tokens")));
    EXPECT_EQ(r.total_tokens, "8"); // 3 in + 5 out, both paths
}

TEST(SseReconstruction, FinishReasonAgreesWithWholePath)
{
    const std::string id = "m", model = "x";
    for (const char* stop : {"end_turn", "stop_sequence", "max_tokens", "tool_use"})
    {
        SCOPED_TRACE(stop);
        Value w = P(anthropic_to_openai_response(anthropic_body(id, model, "hi", stop)));
        const std::string whole_finish(w.find("choices")->arr[0].str_or("finish_reason"));
        const std::string streamed_finish = reassemble(
            translate_whole(anthropic_stream(id, model, {"hi"}, stop))).finish;
        EXPECT_EQ(streamed_finish, whole_finish); // shared map => both paths agree
    }
}

TEST(SseReconstruction, ContentSplitAcrossDeltasReassemblesWhole)
{
    const std::string id = "m", model = "x";
    Reassembled one = reassemble(translate_whole(anthropic_stream(id, model, {"Hello, world!"}, "end_turn")));
    Reassembled many = reassemble(translate_whole(
        anthropic_stream(id, model, {"Hel", "lo, ", "wor", "ld!"}, "end_turn")));
    EXPECT_EQ(one.content, "Hello, world!");
    EXPECT_EQ(many.content, "Hello, world!"); // chunk boundaries don't change the message
}

// Guard against a tautological comparison: different content MUST reassemble
// differently (so the equivalence tests above can actually fail).
TEST(SseReconstruction, DetectsContentMismatch)
{
    const std::string a = reassemble(translate_whole(anthropic_stream("m", "x", {"original"}, "end_turn"))).content;
    const std::string b = reassemble(translate_whole(anthropic_stream("m", "x", {"different"}, "end_turn"))).content;
    EXPECT_NE(a, b);
}

// ── Streamed tool calls ─────────────────────────────────────────────────────
// Anthropic streams a call as content_block_start (id + name) followed by
// input_json_delta fragments; OpenAI expects one opening tool_calls delta carrying
// id/name, then arguments fragments under the same index. The indices are NOT the
// same number, which is what most of these tests are about.

namespace
{
    // Build an Anthropic SSE event.
    std::string ev(const std::string& type, const std::string& data)
    {
        return "event: " + type + "\ndata: " + data + "\n\n";
    }
    std::string blk_start_tool(int idx, const std::string& id, const std::string& name)
    {
        return ev("content_block_start",
                  R"({"type":"content_block_start","index":)" + std::to_string(idx) +
                      R"(,"content_block":{"type":"tool_use","id":")" + id + R"(","name":")" +
                      name + R"(","input":{}}})");
    }
    std::string blk_args(int idx, const std::string& escaped_fragment)
    {
        return ev("content_block_delta",
                  R"({"type":"content_block_delta","index":)" + std::to_string(idx) +
                      R"(,"delta":{"type":"input_json_delta","partial_json":")" +
                      escaped_fragment + R"("}})");
    }
    // Concatenate every arguments fragment for a given tool_calls index, exactly as
    // an OpenAI client would when reassembling the call.
    std::string reassemble(const std::string& out, int ord)
    {
        const std::string key = R"("index":)" + std::to_string(ord);
        std::string args;
        size_t p = 0;
        while ((p = out.find(R"("tool_calls":[{)" + key, p)) != std::string::npos)
        {
            const size_t a = out.find(R"("arguments":")", p);
            if (a == std::string::npos) break;
            size_t s = a + 13;
            // Find the CLOSING quote, skipping escaped ones: the arguments value is
            // JSON-inside-JSON, so it is full of \" and a naive find('"') stops on
            // the first one. (That bug made this test report "{\\" as the payload.)
            size_t e = s;
            while (e < out.size() && out[e] != '"')
                e += (out[e] == '\\') ? 2 : 1;
            args += out.substr(s, e - s);
            p = e;
        }
        return args;
    }
} // namespace

TEST(SseTools, OpeningChunkCarriesIdAndNameWithEmptyArguments)
{
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "toolu_1", "get_weather"), out));
    EXPECT_NE(out.find(R"("tool_calls":[{"index":0,"id":"toolu_1","type":"function")"),
              std::string::npos) << out;
    EXPECT_NE(out.find(R"("name":"get_weather","arguments":"")"), std::string::npos) << out;
    // A client keys off the role chunk arriving first, even when the stream opens
    // straight into a tool call with no text.
    EXPECT_LT(out.find(R"("role":"assistant")"), out.find("tool_calls")) << out;
}

TEST(SseTools, ArgumentFragmentsReassembleExactly)
{
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "t1", "f"), out));
    ASSERT_TRUE(t.feed(blk_args(0, R"({\"city\":)"), out));
    ASSERT_TRUE(t.feed(blk_args(0, R"(\"Paris\",\"n\":1.50})"), out));
    // The fragments must concatenate to the original JSON, byte for byte,
    // including the 1.50 a re-serialising implementation would normalize to 1.5.
    EXPECT_EQ(reassemble(out, 0), R"({\"city\":\"Paris\",\"n\":1.50})");
}

TEST(SseTools, BlockIndexIsNotTheToolOrdinal)
{
    // THE bug this mapping exists to prevent: Anthropic indexes every content
    // block, so a leading TEXT block pushes tool blocks to 1,2, while OpenAI's
    // tool_calls index must still start at 0. Emitting tool_calls[1] with no [0]
    // breaks client-side reassembly.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(ev("content_block_start",
                          R"({"type":"content_block_start","index":0,)"
                          R"("content_block":{"type":"text","text":""}})"), out));
    ASSERT_TRUE(t.feed(ev("content_block_delta",
                          R"({"type":"content_block_delta","index":0,)"
                          R"("delta":{"type":"text_delta","text":"hi"}})"), out));
    ASSERT_TRUE(t.feed(blk_start_tool(1, "A", "fa"), out));
    ASSERT_TRUE(t.feed(blk_start_tool(2, "B", "fb"), out));
    EXPECT_NE(out.find(R"({"index":0,"id":"A")"), std::string::npos) << out;
    EXPECT_NE(out.find(R"({"index":1,"id":"B")"), std::string::npos) << out;
    EXPECT_EQ(out.find(R"({"index":2,)"), std::string::npos)
        << "leaked Anthropic's block index into tool_calls:\n" << out;
}

TEST(SseTools, FragmentsRouteToTheirOwnCall)
{
    // Interleaved fragments for two open calls must not cross-contaminate.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "A", "fa"), out));
    ASSERT_TRUE(t.feed(blk_start_tool(1, "B", "fb"), out));
    ASSERT_TRUE(t.feed(blk_args(0, "aa"), out));
    ASSERT_TRUE(t.feed(blk_args(1, "bb"), out));
    ASSERT_TRUE(t.feed(blk_args(0, "cc"), out));
    EXPECT_EQ(reassemble(out, 0), "aacc");
    EXPECT_EQ(reassemble(out, 1), "bb");
}

TEST(SseTools, FinishReasonToolCallsNowHasCallsBehindIt)
{
    // The regression this whole feature had to fix: reporting tool_calls with no
    // tool_calls array. Now the claim must be backed.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "t1", "f") + blk_args(0, "{}") +
                       ev("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"tool_use"}})") +
                       ev("message_stop", R"({"type":"message_stop"})"), out));
    EXPECT_NE(out.find(R"("finish_reason":"tool_calls")"), std::string::npos) << out;
    EXPECT_NE(out.find(R"("tool_calls":[{"index":0,"id":"t1")"), std::string::npos) << out;
    EXPECT_NE(out.find("[DONE]"), std::string::npos) << "a complete stream must end cleanly";
}

TEST(SseTools, FragmentForAnUnknownBlockIsIgnored)
{
    // A fragment whose content_block_start we never saw has no ordinal. Guessing
    // one would attach a customer's arguments to the wrong call.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    EXPECT_TRUE(t.feed(blk_args(7, "orphan"), out));
    EXPECT_EQ(out.find("tool_calls"), std::string::npos) << out;
}

TEST(SseTools, AbsurdBlockIndexDoesNotAllocate)
{
    // A hostile index must not make us size a vector to it.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    EXPECT_TRUE(t.feed(blk_start_tool(100000000, "x", "f"), out));
    EXPECT_EQ(out.find("tool_calls"), std::string::npos) << out;
}

TEST(SseTools, MalformedIndexIsRejectedNotAliasedToBlockZero)
{
    // to_ll()/from_chars leave their output UNTOUCHED on overflow, so a naive parse
    // turns a garbage index into 0, attaching a customer's argument fragments to
    // whichever tool call happens to occupy block 0. Found by audit before merge.
    for (const char* bad : {"99999999999999999999", "-99999999999999999999", "1e5", "0x10"})
    {
        llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
        std::string out;
        ASSERT_TRUE(t.feed(blk_start_tool(0, "REAL", "f"), out));
        out.clear();
        ASSERT_TRUE(t.feed(std::string("event: content_block_delta\ndata: ") +
                           R"({"type":"content_block_delta","index":)" + bad +
                           R"(,"delta":{"type":"input_json_delta","partial_json":"STOLEN"}})" "\n\n",
                           out));
        EXPECT_EQ(out.find("STOLEN"), std::string::npos)
            << "index " << bad << " was aliased onto block 0:\n" << out;
    }
}

TEST(SseTools, MalformedIndexDoesNotOpenACall)
{
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed("event: content_block_start\ndata: "
                       R"({"type":"content_block_start","index":99999999999999999999,)"
                       R"("content_block":{"type":"tool_use","id":"x","name":"f"}})" "\n\n", out));
    EXPECT_EQ(out.find("tool_calls"), std::string::npos) << out;
}

TEST(SseTools, EveryEmittedChunkIsValidJsonUnderHostileEscaping)
{
    // id, name and every argument fragment are forwarded as RAW (still-escaped)
    // spans. A span that terminated in a lone backslash would escape our closing
    // quote and corrupt the chunk. The parser's escape-skipping makes that
    // impossible; this pins it so a future parser change cannot silently break it.
    const char* frags[] = {R"({"a":1})", R"(say "hi")", R"(path\)", R"(café)", R"(a	b
c)"};
    for (const char* f : frags)
    {
        llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
        std::string out;
        ASSERT_TRUE(t.feed(blk_start_tool(0, R"(id\"q)", R"(name\\)"), out));
        ASSERT_TRUE(t.feed(blk_args(0, f), out));
        size_t p = 0;
        int checked = 0;
        while ((p = out.find("data: ", p)) != std::string::npos)
        {
            const size_t e = out.find('\n', p);
            const std::string line = out.substr(p + 6, e - (p + 6));
            p = e;
            if (line == "[DONE]") continue;
            bool ok = false;
            llmbridge::provider::json::parse(line, ok);
            EXPECT_TRUE(ok) << "fragment " << f << " produced invalid JSON:\n" << line;
            ++checked;
        }
        EXPECT_GT(checked, 0);
    }
}

TEST(SseTools, ToolChunksCarryUsageNullWhenIncludeUsageIsSet)
{
    // include_usage puts "usage":null on every normal chunk; tool chunks go through
    // the same tail and must not be an exception, or a client that reads usage off
    // each chunk sees an inconsistent stream.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, /*include_usage=*/true);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "t1", "f") + blk_args(0, "{}"), out));
    size_t n = 0, p = 0;
    while ((p = out.find("tool_calls", p)) != std::string::npos) { ++n; ++p; }
    ASSERT_EQ(n, 2u) << out;
    // Count chunks, then assert EVERY one carries usage:null; the role chunk does
    // too, so pinning a literal 2 would have been wrong for the wrong reason.
    size_t q = 0, chunks = 0, usage = 0;
    while ((q = out.find("data: ", q)) != std::string::npos) { ++chunks; ++q; }
    q = 0;
    while ((q = out.find(R"("usage":null)", q)) != std::string::npos) { ++usage; ++q; }
    EXPECT_EQ(usage, chunks) << "a chunk is missing usage:null\n" << out;
    EXPECT_GE(chunks, 3u) << out; // role + open + args
}

TEST(SseTools, ToolWithNoNameIsDroppedLikeNonStreaming)
{
    // The non-streaming translator drops a tool with no name ("unusable without a
    // name"). Streaming must agree, or the same upstream yields a usable response
    // one way and a call the client cannot dispatch the other.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "t1", "") + blk_args(0, "{}"), out));
    EXPECT_EQ(out.find("tool_calls"), std::string::npos) << out;
    EXPECT_EQ(out.find(R"("name":"")"), std::string::npos) << out;
}

TEST(SseTools, TruncatedToolCallRefusesACleanEnding)
{
    // Arguments cut mid-JSON. Emitting [DONE] would hand the client unparseable
    // arguments inside a stream that looked complete: the same "corrupt framing
    // fabricated a clean ending" failure 0.3.0 fixed for text.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "t1", "f") + blk_args(0, R"({\"city\":)"), out));
    EXPECT_FALSE(t.finish(out)) << "a truncated tool call must not report success";
    EXPECT_EQ(out.find("[DONE]"), std::string::npos) << out;
}

TEST(SseTools, CompletedToolCallStillEndsCleanly)
{
    // The guard above must not fire on a well-formed stream.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "t1", "f") + blk_args(0, "{}") +
                       ev("message_delta",
                          R"({"type":"message_delta","delta":{"stop_reason":"tool_use"}})"), out));
    EXPECT_TRUE(t.finish(out));
    EXPECT_NE(out.find("[DONE]"), std::string::npos) << out;
}

TEST(SseTools, MessageStopWithoutMessageDeltaStillEndsCleanly)
{
    // Regression for a bug the truncation guard itself introduced: dispatch()
    // emits [DONE] on message_stop, but the guard was checked BEFORE _done, so a
    // stream that had already ended correctly was reported as a failure; the
    // gateway then counts an error and closes abruptly on a good response.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "t1", "f") + blk_args(0, "{}") +
                       ev("message_stop", R"({"type":"message_stop"})"), out));
    ASSERT_NE(out.find("[DONE]"), std::string::npos) << out;
    EXPECT_TRUE(t.finish(out)) << "already-complete stream reported as failed";
}

TEST(SseTools, ToolOpenCounterIsBounded)
{
    // A reopened index would otherwise increment the ordinal without limit
    // (signed overflow is UB). Past the cap, further opens are refused.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    for (int i = 0; i < 300; ++i) ASSERT_TRUE(t.feed(blk_start_tool(0, "id", "f"), out));
    // Ordinals never exceed the cap.
    EXPECT_EQ(out.find(R"("index":256,)"), std::string::npos) << "ordinal ran past the cap";
    EXPECT_NE(out.find(R"("index":255,)"), std::string::npos) << "cap should be reachable";
}

TEST(SseTools, FinishReasonDefaultsToToolCallsOnceACallWasEmitted)
{
    // Found by second-pass review. Every OpenAI SDK branches on
    // finish_reason == "tool_calls" to decide whether to dispatch; reporting "stop"
    // makes the client treat the call as a plain answer and SILENTLY ignore it.
    // Reachable whenever the upstream sends message_stop with no message_delta.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "t1", "f") + blk_args(0, "{}") +
                       ev("message_stop", R"({"type":"message_stop"})"), out));
    EXPECT_NE(out.find(R"("finish_reason":"tool_calls")"), std::string::npos) << out;
    EXPECT_EQ(out.find(R"("finish_reason":"stop")"), std::string::npos) << out;
}

TEST(SseTools, TextOnlyStreamStillDefaultsToStop)
{
    // The default above must not leak into streams that emitted no tool call.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(ev("content_block_delta",
                          R"({"type":"content_block_delta","index":0,)"
                          R"("delta":{"type":"text_delta","text":"hi"}})") +
                       ev("message_stop", R"({"type":"message_stop"})"), out));
    EXPECT_NE(out.find(R"("finish_reason":"stop")"), std::string::npos) << out;
    EXPECT_EQ(out.find("tool_calls"), std::string::npos) << out;
}

TEST(SseTools, ForeignDoneCannotVouchForATruncatedCall)
{
    // `data: [DONE]` is an OpenAI-ism; Anthropic ends with message_stop. Honouring
    // it blindly gave a truncated tool call a clean ending AND skipped the finish
    // chunk, leaving finish_reason:null with no way for a client to know.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "t1", "f") + blk_args(0, R"({\"a\":)") +
                       "data: [DONE]\n\n", out));
    EXPECT_EQ(out.find("[DONE]"), std::string::npos) << "truncated call ended cleanly:\n" << out;
    EXPECT_FALSE(t.finish(out));
}

TEST(SseTools, ForeignDoneOnACompleteStreamStillEmitsAFinishChunk)
{
    // ...but a well-formed stream terminated by [DONE] must still get its finish
    // chunk, not jump straight to the sentinel with finish_reason never set.
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(blk_start_tool(0, "t1", "f") + blk_args(0, "{}") +
                       ev("message_delta",
                          R"({"type":"message_delta","delta":{"stop_reason":"tool_use"}})") +
                       "data: [DONE]\n\n", out));
    EXPECT_NE(out.find(R"("finish_reason":"tool_calls")"), std::string::npos) << out;
    EXPECT_NE(out.find("[DONE]"), std::string::npos) << out;
}

TEST(SseTools, PlainTextStreamsAreUnaffected)
{
    llmbridge::provider::AnthropicToOpenAiSse t(1700000000, false);
    std::string out;
    ASSERT_TRUE(t.feed(
        ev("message_start", R"({"type":"message_start","message":{"id":"m1","model":"c"}})") +
        ev("content_block_delta",
           R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"hi"}})") +
        ev("message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})") +
        ev("message_stop", R"({"type":"message_stop"})"), out));
    EXPECT_NE(out.find(R"("content":"hi")"), std::string::npos) << out;
    EXPECT_NE(out.find(R"("finish_reason":"stop")"), std::string::npos) << out;
    EXPECT_EQ(out.find("tool_calls"), std::string::npos) << out;
}
