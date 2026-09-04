// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// One streaming step, shared by the epoll and io_uring pumps: chunk-decode, translate
// or forward, stamp the first token, notice the end. Inline for the same reason as
// scan.hpp: this runs on every read of every stream.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "gateway/gateway.hpp"
#include "scan.hpp"

namespace llmbridge::detail
{
    /// Whether this client can be handed a chunked stream and keep its connection.
    [[nodiscard]] inline bool stream_reusable_out(const Connection* c) noexcept
    {
        return c->msg.http_1_1 && c->msg.keep_alive;
    }

    /// Wrap whatever was appended to `out` at or after `pos` in one chunk frame.
    /// A no-op on a close-delimited stream, so both framings share one write path.
    inline void chunk_wrap(const Connection* c, std::string& out, size_t pos)
    {
        if (!c->stream_chunked_out || out.size() <= pos) return;
        char hdr[24];
        const int n = std::snprintf(hdr, sizeof hdr, "%zx\r\n", out.size() - pos);
        if (n <= 0) return;
        out.insert(pos, hdr, static_cast<size_t>(n));
        out.append("\r\n", 2);
    }

    /// The terminating zero-length chunk. Emitted only on a clean end: a truncated
    /// stream must not be given the marker that says it finished.
    inline void chunk_terminate(const Connection* c, std::string& out)
    {
        if (c->stream_chunked_out) out.append("0\r\n\r\n", 5);
    }

    /// The first chunk carrying a visible token, on the byte-forward path.
    ///
    /// Empty content is not a token. OpenAI opens a stream with
    /// {"role":"assistant","content":""} and matching that collapsed TTFT to TTFB
    /// on every OpenAI-dialect stream.
    inline bool sse_carries_first_token(std::string_view s) noexcept
    {
        constexpr std::string_view kContent = "\"content\":\"";
        for (size_t pos = find_fast(s, kContent); pos != std::string_view::npos;
             pos = find_fast(s, kContent, pos + 1))
        {
            const size_t v = pos + kContent.size();
            if (v >= s.size()) return false; // value byte not in this read yet
            if (s[v] != '"') return true;    // non-empty content
        }
        return find_fast(s, "\"text_delta\"") != std::string_view::npos ||
               find_fast(s, "\"type\":\"tool_use\"") != std::string_view::npos ||
               find_fast(s, "\"tool_calls\"") != std::string_view::npos;
    }

    /// The first reasoning marker on the wire, shared by both stamp sites so the
    /// two never diverge. `thinking_delta` (Anthropic) and `reasoning_content`
    /// (OpenAI-dialect) are the streamed forms; `redacted_thinking` is the block
    /// Anthropic emits when it safety-filters the chain of thought.
    inline bool sse_carries_thinking(std::string_view s) noexcept
    {
        // `thinking_delta` and `redacted_thinking` are type names: their presence
        // is the signal. `reasoning_content` is a field, and an OpenAI-compatible
        // server that supports reasoning sends it on every delta whether or not
        // the model reasoned, as `"reasoning_content":null`.
        if (find_fast(s, "\"thinking_delta\"") != std::string_view::npos ||
            find_fast(s, "\"redacted_thinking\"") != std::string_view::npos)
            return true;
        constexpr std::string_view kReason = "\"reasoning_content\":";
        for (size_t pos = find_fast(s, kReason); pos != std::string_view::npos;
             pos = find_fast(s, kReason, pos + 1))
        {
            size_t v = pos + kReason.size();
            while (v < s.size() && (s[v] == ' ' || s[v] == '\t')) ++v;
            if (v >= s.size()) return false; // value byte not in this read yet
            if (s[v] != '"') continue;       // null, and nothing else is a string
            if (v + 1 < s.size() && s[v + 1] != '"') return true; // non-empty
        }
        return false;
    }

    /// Bytes of a reply worth searching for the served tier, and it is a different
    /// end of the reply in each dialect.
    constexpr size_t kTierHead = 2048;

    constexpr size_t kTierTail = 2048;

    /// Reads to search before concluding the venue has no such field. Both dialects
    /// put it in the first chunk, so this only has to survive a read that splits
    /// one, and four is generous for that.
    constexpr uint8_t kTierTries = 4;

    /// `tail` searches the end of `bytes`, which is where a non-streamed body puts
    /// it, beside the usage block.
    inline void note_served_tier(Connection* c, std::string_view bytes, bool tail) noexcept
    {
        if (c->served_tier_len || c->served_tier_tries >= kTierTries) return;
        ++c->served_tier_tries;
        const size_t w = tail ? kTierTail : kTierHead;
        const std::string_view head =
            bytes.size() <= w ? bytes
                              : (tail ? bytes.substr(bytes.size() - w) : bytes.substr(0, w));
        const std::string_view t = json_string_at(head, "\"service_tier\"");
        const size_t n = t.size() < sizeof c->served_tier ? t.size()
                                                          : sizeof c->served_tier;
        c->served_tier_len = static_cast<uint8_t>(n);
        if (n) std::memcpy(c->served_tier, t.data(), n);
    }

    /// Keep the tail of a byte-forwarded stream, so the final usage chunk can be
    /// read at the end.
    ///
    /// The anthropic stream translator counts tokens as it parses, so that path
    /// needs none of this. A byte-forwarded stream is never parsed, so without the
    /// tail a streamed passthrough request records no tokens at all, which is the
    /// shape most of a voice workload takes.
    ///
    /// Not "translated streams" as a category: `Connection::sse_xlate` is typed
    /// `AnthropicToOpenAiSse` and is the only SSE translator that exists. Gemini
    /// and Cohere have none and do not stream here at all. Whoever adds one owns
    /// its token accounting; stream_tokens() below is where that decision lands.
    ///
    /// Bounded and only kept when the client asked for usage: with no
    /// `stream_options.include_usage` the provider sends no usage chunk, so the
    /// copy would buy nothing.
    inline void stream_note_usage(Connection* client, std::string_view bytes) noexcept
    {
        client->stream_tail.append(bytes);

        // Scanned as it arrives, not once at the end, because Anthropic reports
        // input and cache tokens in `message_start`, at the very beginning of the
        // stream. A tail window holds the last 2 KiB, so on any stream longer
        // than that the input count had already scrolled out by the time anyone
        // looked. OpenAI puts everything in one chunk before [DONE], which is why
        // the tail was enough until this dialect arrived.
        //
        // The `wants_usage` gate went with it: that flag reads
        // `stream_options.include_usage`, an OpenAI option Anthropic does not
        // have and Claude Code never sends, so gating on it meant no counts at
        // all for the dialect this exists to measure.
        //
        // Gated instead on a cheap search of the arriving bytes, so the hot path
        // pays one substring scan per chunk. `_tokens` catches a usage block
        // split across two reads: the half carrying the numbers triggers a
        // rescan of the tail, which by then holds both halves.
        //
        // The scan runs before the window is trimmed, and that ordering is the
        // whole fix. Trimming first discards whatever arrived earlier in the same
        // read, and a mock or a fast provider delivers an entire stream in one
        // read: message_start had already been cut away when the scan ran, so
        // input and cache came back as "not reported" while output was found.
        if (find_fast(bytes, "usage") != std::string_view::npos ||
            find_fast(bytes, "_tokens") != std::string_view::npos)
        {
            const BodyUsage u = scan_usage(client->stream_tail, 0);
        // Input and cache are first-wins: stated once, in message_start, and a
        // later chunk mentioning them again is not a new fact. Output is
        // last-wins: message_start carries a placeholder 1 and message_delta
        // carries the real total.
            if (client->usage_in < 0 && u.in >= 0) client->usage_in = u.in;
            if (client->usage_cached < 0 && u.cached >= 0) client->usage_cached = u.cached;
            if (client->usage_cache_write < 0 && u.cache_write >= 0)
                client->usage_cache_write = u.cache_write;
            if (client->usage_cw_5m < 0 && u.cache_write_5m >= 0)
                client->usage_cw_5m = u.cache_write_5m;
            if (client->usage_cw_1h < 0 && u.cache_write_1h >= 0)
                client->usage_cw_1h = u.cache_write_1h;
            if (u.out >= 0) client->usage_out = u.out;
        }
        // Trimmed last, so the buffer stays bounded across reads while every read
        // is searched whole.
        if (client->stream_tail.size() > 2 * kUsageWindow)
            client->stream_tail.erase(0, client->stream_tail.size() - kUsageWindow);
    }

    /// A finished stream's token counts, from whichever of the two paths carried
    /// it: the Anthropic translator, or the tail of a byte-forwarded stream.
    ///
    /// One function because the sink and the debug log both want them, and a
    /// number that appears in one but not the other is a bug this file has been
    /// bitten by before. -1 means not reported, never zero.
    ///
    /// Two paths because there are two, not because "translated" is a category. A
    /// third dialect that learns to stream must add its own branch here; falling
    /// through to the tail scan would search a non-OpenAI stream for an OpenAI
    /// usage block and quietly report nothing.
    inline BodyUsage stream_tokens(const Connection* c) noexcept
    {
        if (c->sse_xlate)
            return {c->sse_xlate->input_tokens(), c->sse_xlate->output_tokens(),
                    static_cast<long long>(c->sse_xlate->cached_tokens()),
                    static_cast<long long>(c->sse_xlate->cache_write_tokens()),
                    static_cast<long long>(c->sse_xlate->cache_write_5m_tokens()),
                    static_cast<long long>(c->sse_xlate->cache_write_1h_tokens())};
        // Accumulated by stream_note_usage as the stream ran. Reading the tail
        // here instead would miss anything stated before the last 2 KiB.
        return {c->usage_in, c->usage_out, c->usage_cached, c->usage_cache_write,
                c->usage_cw_5m, c->usage_cw_1h};
    }

    // Did this stream end, or did it just stop? The two are not the same, and one
    // Or between them was the whole defect: `chunkdec.done() || at_eof` let EOF
    // override the framing even when the framing itself proves the stream was cut.
    // The decoder only reports done() on the terminating 0-length chunk, so on a
    // chunked stream that is the only honest end; EOF before it is a truncation, and
    // finishing there emits a fabricated finish_reason "stop" and a [DONE] telling
    // the client it received a complete answer.
    //
    // A close-delimited stream (no chunked framing) genuinely ends at EOF, and must
    // still finish there.
    inline bool stream_complete(const Connection* client, bool at_eof)
    {
        if (client->stream_chunked) return client->chunkdec.done();
        return at_eof;
    }

    // Outcome of one streaming translate step (shared by both backends).
    enum class StreamStep
    {
        Ok,      // bytes translated (maybe none); stream continues
        Ended,   // upstream signalled end; terminal [DONE] emitted
        Corrupt, // malformed chunked framing: drop, never a fake clean [DONE]
        Failed   // translator refused (cap tripped / protocol error): drop
    };

    // The dialect/transport transform shared by the epoll and io_uring pumps:
    // chunk-decode -> SSE-translate -> detect end. Lives in one place so a fix
    // (e.g. honouring the translator's cap) can't land on one backend only; the
    // backends keep their own idiomatic delivery (flush+pause vs kick+cap).
    // `in` is the upstream's raw buffer (consumed); `out` receives client SSE.
    inline StreamStep stream_step(Connection* client, std::string& in, std::string& out, bool at_eof)
    {
        // Reused, never re-allocated
        std::string& sse_in = client->sse_scratch;
        sse_in.clear();
        if (client->stream_chunked)
        {
            const bool ok = client->chunkdec.feed(in, sse_in);
            in.clear();
            if (!ok) return StreamStep::Corrupt; // truncate honestly: no fake [DONE]
        }
        else
        {
            sse_in.swap(in);
            in.clear();
        }

        // The longest silence between chunks, measured here because this is the one
        // point every chunk crosses in either dialect and on both backends. Only
        // after the first visible token.
        if (!sse_in.empty())
        {
            const int64_t now = now_ns();
            if (client->ts_first_token != 0 && client->ts_last_chunk != 0)
            {
                const int64_t gap = now - client->ts_last_chunk;
                if (gap > client->max_chunk_gap_ns) client->max_chunk_gap_ns = gap;
            }
            client->ts_last_chunk = now;
            // Beside the gap stamp, and for the same reason: this is the one point
            // every chunk crosses in either dialect.
            note_served_tier(client, sse_in, /*tail=*/false);
        }

        // No translator = BYTE-FORWARD. An OpenAI-compatible venue already speaks
        // the client's dialect, so the bytes pass through untouched and the
        // provider's own [DONE] terminates the stream. Until 2026-08-21 this mode
        // never reached the pump at all: streaming was detected for the Anthropic
        // path only, so a passthrough stream was framed as a whole body and
        // delivered at the end.
        if (!client->sse_xlate)
        {
            if (!sse_in.empty())
            {
                stream_note_usage(client, sse_in);
                // Reasoning is stamped before the token, because it comes first on
                // the wire and one read can carry both. See the two helpers.
                if (client->ts_first_thinking == 0 && client->ts_first_token == 0 &&
                    sse_carries_thinking(sse_in))
                    client->ts_first_thinking = now_ns();
                if (client->ts_first_token == 0 && sse_carries_first_token(sse_in))
                    client->ts_first_token = now_ns();
                const size_t at = out.size();
                out.append(sse_in);
                chunk_wrap(client, out, at);
            }
            if (stream_complete(client, at_eof) && !client->stream_ended)
            {
                chunk_terminate(client, out); // clean end only; see the helper
                client->stream_ended = true;
                return StreamStep::Ended;
            }
            // EOF with the framing unfinished is the truncation. Reported, so the
            // backends tear the stream down honestly; returning Ok here left the
            // client holding an open connection that would never speak again.
            if (at_eof && !client->stream_ended) return StreamStep::Corrupt;
            return client->stream_ended ? StreamStep::Ended : StreamStep::Ok;
        }

        // Honour the translator's own failure (its DoS caps are sticky): a
        // hostile/broken upstream must tear the stream down, not silently
        // produce nothing while we keep reading it forever.
        const size_t xlate_at = out.size();
        if (!sse_in.empty() && !client->sse_xlate->feed(sse_in, out)) return StreamStep::Failed;
        chunk_wrap(client, out, xlate_at);

        // TTFT: the first content token, stamped here because this is the one place
        // both backends translate, and it runs right after the read that carried
        // the bytes.
        // Before the token stamp, because reasoning comes first on the wire and a
        // single read can carry both.
        if (client->ts_first_thinking == 0 && sse_carries_thinking(sse_in))
            client->ts_first_thinking = now_ns();
        if (client->ts_first_token == 0 && client->sse_xlate->content_started())
            client->ts_first_token = now_ns();

        if (stream_complete(client, at_eof) && !client->stream_ended)
        {
            // The translator's trailer ([DONE] and any final event) is body, so it
            // is framed like every other write before the terminator closes it.
            const size_t fin_at = out.size();
            if (!client->sse_xlate->finish(out)) return StreamStep::Failed;
            chunk_wrap(client, out, fin_at);
            chunk_terminate(client, out);
            client->stream_ended = true;
            return StreamStep::Ended;
        }
        if (at_eof && !client->stream_ended) return StreamStep::Corrupt; // see above
        return client->stream_ended ? StreamStep::Ended : StreamStep::Ok;
    }

} // namespace llmbridge::detail
