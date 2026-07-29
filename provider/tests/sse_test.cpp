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

    // Feed the input one byte at a time — worst-case fragmentation.
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
// stream is strict — no bare control bytes leak through passthrough — and the
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
    // role chunk, "OK", finish chunk, [DONE] — the garbage produced nothing.
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

// ── Cross-path "reconstruction": stream-then-reassemble == translate-the-whole-body ─
//
// We have only the forward direction (Anthropic->OpenAI), so a true round-trip
// isn't possible yet. The strong analog is that the STREAMING path and the
// NON-STREAMING path must agree: translating an Anthropic SSE stream and
// reassembling its OpenAI chunks must recover exactly what
// anthropic_to_openai_response() produces for the equivalent whole body. This
// pins the two production paths together (the reason the shared helpers were
// extracted) and — run over the same nasty escaping/UTF-8 payloads the request
// reconstruction uses — proves the streaming raw-span passthrough is byte-faithful.

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
                        "\",\"model\":\"" + model + "\"}}\n\n"
                        "data: {\"type\":\"content_block_start\",\"index\":0,"
                        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
        for (const auto& d : deltas)
            s += "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
                 "{\"type\":\"text_delta\",\"text\":\"" + d + "\"}}\n\n";
        s += "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
             "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"" + stop + "\"}}\n\n"
             "data: {\"type\":\"message_stop\"}\n\n";
        return s;
    }

    struct Reassembled { std::string id, model, content, finish; };

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
            const Value* choices = ch.find("choices");
            if (!choices || !choices->is_array() || choices->arr.empty()) continue;
            const Value& c0 = choices->arr[0];
            if (const Value* d = c0.find("delta")) r.content += std::string(d->str_or("content"));
            if (const std::string_view fr = c0.str_or("finish_reason"); !fr.empty()) r.finish.assign(fr);
        }
        return r;
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
    // NOTE: `usage` is intentionally NOT cross-checked — the text-only streaming
    // slice does not yet emit a usage chunk (OpenAI's include_usage). That's a
    // known gap tracked for the next slice, not a bug here.
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
