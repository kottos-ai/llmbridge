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
#include <cstring>
#include <string>
#include <string_view>

#include "core/conn.hpp"
#include "gateway/gateway.hpp"
#include "scan.hpp"

namespace llmbridge::detail
{
    /// Whether this client can be handed a chunked stream and keep its connection.
    [[nodiscard]] inline bool stream_reusable_out(const Connection* c) noexcept
    {
        return c->msg.http_1_1 && c->msg.keep_alive;
    }

    /// Can reuse only if we framed the reply so the body has an end marker, the
    /// stream reached that marker, and the caller wanted the connection kept.
    [[nodiscard]] inline bool stream_client_reusable(const Connection* c) noexcept
    {
        return c->req.f.stream_chunked_out && !c->close_after_resp && c->msg.keep_alive;
    }

    /// The upstream may outlive this stream: it said keep-alive, and the stream was not
    /// aborted, corrupt or timed out.
    [[nodiscard]] inline bool stream_keeps_upstream(const Connection* c) noexcept
    {
        return c->req.f.stream_keep_alive && !c->close_after_resp;
    }

    /// The terminal chunk was consumed: a close-delimited body cannot tell finished
    /// from died.
    [[nodiscard]] inline bool stream_body_ended(const Connection* c) noexcept
    {
        return c->req.f.stream_chunked && c->req.chunkdec.done();
    }

    /// Wrap whatever was appended to `out` at or after `pos` in one chunk frame.
    /// A no-op on a close-delimited stream, so both framings share one write path.
    inline void chunk_wrap(const Connection* c, std::string& out, size_t pos)
    {
        if (!c->req.f.stream_chunked_out || out.size() <= pos) return;
        char hdr[24];
        char* p = hdr + sizeof hdr;
        *--p = '\n';
        *--p = '\r';
        for (size_t n = out.size() - pos; n; n >>= 4) *--p = "0123456789abcdef"[n & 15];
        out.insert(pos, p, static_cast<size_t>(hdr + sizeof hdr - p));
        out.append("\r\n", 2);
    }

    /// The terminating zero-length chunk. Emitted only on a clean end: a truncated
    /// stream must not be given the marker that says it finished.
    inline void chunk_terminate(const Connection* c, std::string& out)
    {
        if (c->req.f.stream_chunked_out) out.append("0\r\n\r\n", 5);
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
        if (!c->req.f.served_tier.empty() || c->req.f.served_tier_tries >= kTierTries) return;
        ++c->req.f.served_tier_tries;
        const size_t w = tail ? kTierTail : kTierHead;
        const std::string_view head =
            bytes.size() <= w ? bytes
                              : (tail ? bytes.substr(bytes.size() - w) : bytes.substr(0, w));
        c->req.f.served_tier.set(scan_string(head, "\"service_tier\"", tail));
    }

    inline void keep_served_model(Connection* c, std::string_view m) noexcept
    {
        c->req.f.served_model.set(m);
    }

    /// The model a reply names. A non-streamed body is read whole, top level only;
    /// a stream is read in the head of each of its first reads, one `data:` line at
    /// a time, since both dialects name the model in their first event.
    inline void note_served_model(Connection* c, std::string_view bytes, bool streamed) noexcept
    {
        if (!c->req.f.served_model.empty() || c->req.f.served_model_tries >= kTierTries) return;
        ++c->req.f.served_model_tries;
        if (!streamed) return keep_served_model(c, provider::reply_model(bytes));
        const std::string_view head = bytes.substr(0, kTierHead);
        constexpr std::string_view kData = "data:";
        for (size_t at = 0; at < head.size();)
        {
            size_t eol = head.find('\n', at);
            if (eol == std::string_view::npos) eol = head.size();
            std::string_view line = head.substr(at, eol - at);
            at = eol + 1;
            if (line.substr(0, kData.size()) != kData) continue;
            const std::string_view m = provider::reply_model(line.substr(kData.size()));
            if (!m.empty()) return keep_served_model(c, m);
        }
    }

    /// Usage a byte-forwarded stream states, read as it arrives: Anthropic states its
    /// input and cache counts in the first event and OpenAI its totals in the last.
    inline void stream_note_usage(Connection* client, std::string_view bytes)
    {
        client->req.stream_usage.feed(bytes);
    }

    /// A finished stream's token counts, from the Anthropic translator or from what a
    /// byte-forwarded stream stated; one function so the sink and the log agree. A
    /// dialect that learns to stream adds its own branch here.
    inline BodyUsage stream_tokens(const Connection* c) noexcept
    {
        if (c->req.f.sse_translating) return c->sse_xlate.usage();
        return c->req.stream_usage.usage();
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
        if (client->req.f.stream_chunked) return client->req.chunkdec.done();
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
        std::string& sse_in = client->req.sse_scratch;
        sse_in.clear();
        if (client->req.f.stream_chunked)
        {
            const bool ok = client->req.chunkdec.feed(in, sse_in);
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
            if (client->req.f.ts_first_token != 0 && client->req.f.ts_last_chunk != 0)
            {
                const int64_t gap = now - client->req.f.ts_last_chunk;
                if (gap > client->req.f.max_chunk_gap_ns) client->req.f.max_chunk_gap_ns = gap;
            }
            client->req.f.ts_last_chunk = now;
            // Beside the gap stamp, and for the same reason: this is the one point
            // every chunk crosses in either dialect.
            note_served_tier(client, sse_in, /*tail=*/false);
            note_served_model(client, sse_in, /*streamed=*/true);
        }

        // No translator = BYTE-FORWARD. An OpenAI-compatible venue already speaks
        // the client's dialect, so the bytes pass through untouched and the
        // provider's own [DONE] terminates the stream. Until 2026-08-21 this mode
        // never reached the pump at all: streaming was detected for the Anthropic
        // path only, so a passthrough stream was framed as a whole body and
        // delivered at the end.
        if (!client->req.f.sse_translating)
        {
            if (!sse_in.empty())
            {
                stream_note_usage(client, sse_in);
                // Reasoning is stamped before the token, because it comes first on
                // the wire and one read can carry both. See the two helpers.
                if (client->req.f.ts_first_thinking == 0 && client->req.f.ts_first_token == 0 &&
                    sse_carries_thinking(sse_in))
                    client->req.f.ts_first_thinking = now_ns();
                if (client->req.f.ts_first_token == 0 && sse_carries_first_token(sse_in))
                    client->req.f.ts_first_token = now_ns();
                const size_t at = out.size();
                out.append(sse_in);
                chunk_wrap(client, out, at);
            }
            if (stream_complete(client, at_eof) && !client->req.f.stream_ended)
            {
                chunk_terminate(client, out); // clean end only; see the helper
                client->req.f.stream_ended = true;
                return StreamStep::Ended;
            }
            // EOF with the framing unfinished is the truncation. Reported, so the
            // backends tear the stream down honestly; returning Ok here left the
            // client holding an open connection that would never speak again.
            if (at_eof && !client->req.f.stream_ended) return StreamStep::Corrupt;
            return client->req.f.stream_ended ? StreamStep::Ended : StreamStep::Ok;
        }

        // Honour the translator's own failure (its DoS caps are sticky): a
        // hostile/broken upstream must tear the stream down, not silently
        // produce nothing while we keep reading it forever.
        const size_t xlate_at = out.size();
        if (!sse_in.empty() && !client->sse_xlate.feed(sse_in, out)) return StreamStep::Failed;
        chunk_wrap(client, out, xlate_at);

        // TTFT: the first content token, stamped here because this is the one place
        // both backends translate, and it runs right after the read that carried
        // the bytes.
        // Before the token stamp, because reasoning comes first on the wire and a
        // single read can carry both.
        if (client->req.f.ts_first_thinking == 0 && sse_carries_thinking(sse_in))
            client->req.f.ts_first_thinking = now_ns();
        if (client->req.f.ts_first_token == 0 && client->sse_xlate.content_started())
            client->req.f.ts_first_token = now_ns();

        if (stream_complete(client, at_eof) && !client->req.f.stream_ended)
        {
            // The translator's trailer ([DONE] and any final event) is body, so it
            // is framed like every other write before the terminator closes it.
            const size_t fin_at = out.size();
            if (!client->sse_xlate.finish(out)) return StreamStep::Failed;
            chunk_wrap(client, out, fin_at);
            chunk_terminate(client, out);
            client->req.f.stream_ended = true;
            return StreamStep::Ended;
        }
        if (at_eof && !client->req.f.stream_ended) return StreamStep::Corrupt; // see above
        return client->req.f.stream_ended ? StreamStep::Ended : StreamStep::Ok;
    }

} // namespace llmbridge::detail
