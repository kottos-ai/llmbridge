// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// A stream's life on either backend: entering it, the upstream's EOF, the finalize,
// the encoding warning, the latency record and the one way to truncate.

#include "engine.hpp"

#include "core/io.hpp"
#include "core/limits.hpp"
#include "net/http_write.hpp"
#include "request.hpp"
#include "response.hpp"
#include "stream.hpp"

namespace llmbridge::detail
{

    // Token counts cannot be read from a compressed stream. request_without drops
    // Accept-Encoding, so the provider compressed unasked; say so.
    void Engine::stream_warn_if_encoded(const ClientConn* client,
                                         const net::http::ResponseHead& h) noexcept
    {
        if (!h.encoded) return;
        LB_WARN(ReqId{client->req.f.req_seq},
                " upstream compressed the response, so token counts are not readable "
                "for this request; Accept-Encoding was not forwarded");
    }

    // request-path and the handshake do not care what shape the response takes, so a
    // stream records them like any other request. resp_path and added-total it cannot
    // have: both end at the instant a response was built. See LATENCY.md section 4.
    void Engine::stream_record_latency(const ClientConn* client) noexcept
    {
        if (now_ns() - _t_start < _warmup_ns) return;
        // t4 stands in for the absent t5; only the request-side fields are read.
        const TimingSplit sp = split_of(client->req.f, client->req.f.ts_up_recvd);
        if (sp.req_path_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(sp.req_path_ns));
        if (sp.connect_ns >= 0) _stats.connect.record(static_cast<uint64_t>(sp.connect_ns));
        // Zero means no content chunk ever arrived, not an instant answer. The wait starts
        // at the body's last byte, t0 when held: a streamed body's upload is the client's.
        const int64_t from = client->req.f.ts_body_end ? client->req.f.ts_body_end : client->req.f.ts_req_recvd;
        if (client->req.f.ts_first_token > 0 && from > 0 && client->req.f.ts_first_token >= from)
            _stats.first_token.record(static_cast<uint64_t>(client->req.f.ts_first_token - from));
    }

    // The upstream answered text/event-stream: stage the client's SSE head, then
    // translate or forward the body as it arrives.
    void Engine::begin_stream(UpstreamConn* u, const net::http::ResponseHead& h) noexcept
    {
        ClientConn* client = u->client;
        client->req.f.streaming = true;
        client->req.f.stream_body = h.body; // it alone says where the body ends
        client->req.f.stream_left = h.content_length;
        client->req.f.stream_keep_alive = h.keep_alive; // decides poolability at stream end
        stream_warn_if_encoded(client, h);
        // Only a request that needs translating gets a translator.
        client->req.f.sse_translating =
            client->req.f.translate_body &&
            provider::spec(client->req.f.effective_dialect).wire == provider::Wire::Anthropic;
        if (client->req.f.sse_translating) client->sse_xlate.reset(-1, client->req.f.wants_usage);
        else client->req.stream_usage.reset(client->req.f.responses_api);
        // Chosen once, before either head is built, and remembered.
        client->req.f.stream_chunked_out = stream_reusable_out(client);
        std::string& out = client->out.stage();
        const std::string_view base = client->req.f.stream_chunked_out ? kSseHeadChunked : kSseHead;
        out.append(base.substr(0, base.size() - 2)); // its blank line comes last
        if (_timing_headers)
        {
            // t5 = t4, stamped by the caller: a stream's response is not built at one
            // instant, so the compute leg is the request side alone.
            const TimingSplit sp = split_of(client->req.f, client->req.f.ts_up_recvd);
            append_timing_headers(out, client->req.f, sp, "x-llmbridge-upstream-ttfb-us");
        }
        // The venue's request id and rate limits, as on a whole reply.
        net::http::append_relayed(out, std::string_view(u->rbuf.data(), h.header_len - 4), false);
        out.append("\r\n");
        client->req.f.phase = Phase::Streaming;

        u->rbuf.erase(0, h.header_len); // consume the head; the rest is body
        stream_pump(u);                  // translate any initial body, flush the head
    }

    // New body bytes in `u->rbuf`: translate or forward them into the client's buffer.
    void Engine::stream_pump(UpstreamConn* u) noexcept
    {
        ClientConn* client = u->client;
        if (!client) return _ops->close(*u);
        const StreamStep st = stream_step(client, u->rbuf, client->out.stage(), /*at_eof=*/false);
        // Truncate honestly: flush what was translated, with no fabricated [DONE].
        if (st == StreamStep::Corrupt || st == StreamStep::Failed) stream_truncate(client);
        else if (_caps.stream_cap && client->owed() > _caps.stream_cap)
        {
            // The ceiling, paused or not: bytes already read still land after a pause.
            LB_WARN("CAP stream buffer exceeded, dropping the stream ", *client,
                    " buffered=", client->owed(), " limit=", static_cast<uint64_t>(_caps.stream_cap),
                    " (the client is slower than the provider)");
            return abort_pair(client);
        }
        stream_flush(client);
    }

    // Write what the stream owes the client. Behind a slow client a backend that can
    // pauses the upstream's reads (GATEWAY-INTERNALS.md 6c); a stream that ended and
    // drained finalizes.
    void Engine::stream_flush(ClientConn* client) noexcept
    {
        // A hedge leg streaming while its client waits holds its bytes: its first token wins
        // the client, never its head, which can precede an error. One that ends without a
        // token has lost, unless no other leg is left (GATEWAY-INTERNALS.md 13).
        if (client->shadow && client->hedge_of && client->hedge_of->hedge_open)
        {
            const ReqState& f = client->req.f;
            const bool token = f.ts_first_token || f.ts_first_thinking;
            if (!token && !f.stream_ended) return;
            if (token || leg_alone(client)) client = adopt_leg(client);
        }
        // After the hedge chose, so a leg that lost never asks for a note and the winner asks
        // with its own record; once, for both stream kinds on both backends (6d).
        if (client->req.f.stream_ended && !client->req.f.stream_terminated && !client->close_after_resp &&
            !client->shadow)
            stream_terminate(client);
        const uint64_t sent = client->sent_bytes;
        const Sent s = _ops->send(*client);
        // A client draining a paused stream is progress, though the upstream is silent.
        if (client->sent_bytes != sent) client->req.f.ts_progress = now_ns();
        if (s == Sent::Failed) return abort_pair(client);
        UpstreamConn* u = client->up;
        const bool pausable = _caps.stream_backpressure && u && !u->doomed;
        if (s == Sent::Queued)
        {
            if (pausable && !u->read_paused && client->owed() > _caps.stream_pause_above)
            {
                _ops->set_read(*u, false);
                ++_stats.stream_pauses;
                LB_WARN("CAP backpressure, pausing upstream reads ", *u,
                        " (the client is slower than the provider)");
            }
            return;
        }
        client->out.clear();
        if (client->req.f.stream_ended) return finalize_stream(client);
        if (pausable && u->read_paused)
        {
            _ops->set_read(*u, true);
            LB_WARN("CAP backpressure cleared, resuming upstream reads ", *u);
        }
    }

    // A clean end a client reads: the sink's note, then the terminating chunk.
    void Engine::stream_terminate(ClientConn* client) noexcept
    {
        client->req.f.stream_terminated = true;
        std::string& out = client->out.stage();
        if (_sink) sink_note(client, out);
        chunk_terminate(client, out);
    }

    // The upstream closed: translate what remains, emit the terminal [DONE] if the
    // framing allows, and flush toward the finalize.
    void Engine::stream_on_upstream_eof(UpstreamConn* u) noexcept
    {
        ClientConn* client = u->client;
        if (!client)
        {
            _ops->close(*u);
            return;
        }
        const StreamStep st = stream_step(client, u->rbuf, client->out.stage(), /*at_eof=*/true);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed) stream_truncate(client);
        // Ended either way, so the upstream is done. Kept while its reads were paused,
        // the dead fd raised EPOLLHUP on every wait.
        _ops->close(*u);
        stream_flush(client);
    }

    void Engine::finalize_stream(ClientConn* client) noexcept
    {
        // Reuse is the whole point: a streaming request otherwise costs a fresh
        // upstream connect, the dominant term in time-to-first-token.
        if (UpstreamConn* u = unpair(client))
            release_upstream(u, stream_keeps_upstream(client), stream_body_ended(client));
        // The stream's outcome, size and tokens. `close_after_resp` here means
        // truncated: no [DONE], which the client cannot tell from a clean finish.
        LB_DEBUG(ReqId{client->req.f.req_seq}, " stream ended ",
                 client->close_after_resp ? "TRUNCATED" : "clean",
                 " tokens_in=", stream_tokens(client).in,
                 " tokens_out=", stream_tokens(client).out,
                 " on ", *client);
        // Only a clean end is a served request; an abort was counted in errors. A hedge
        // leg is no client's request.
        if (!client->close_after_resp && !client->shadow)
        {
            ++_stats.requests;
            stream_record_latency(client);
        }
        if (_sink && !client->req.f.sunk) sink_emit(client, 200, /*streamed=*/true);

        // Keep the connection when the framing gave the body an end marker.
        if (client->shadow || !stream_client_reusable(client))
        {
            _ops->close(*client);
            return;
        }
        client->req.f.streaming = false; // over; begin() resets the rest at the next framing
        client->req.f.phase = Phase::Idle;
        client->out.clear();
        client->msg = net::http::Message{};
        on_client_bytes(client); // a pipelined next request may already be in rbuf
    }

    void Engine::stream_truncate(ClientConn* client) noexcept
    {
        // One call for the three mutations and the log line: GATEWAY-INTERNALS.md 6b.
        // CorruptStreamFromAProviderThatHoldsTheConnectionStillClosesTheClient guards
        // the stream_ended latch.
        LB_WARN(ReqId{client->req.f.req_seq}, " stream TRUNCATED (no [DONE] emitted)",
                " tokens_in=", stream_tokens(client).in,
                " tokens_out=", stream_tokens(client).out,
                " on ", *client);
        client->req.f.stream_ended = true;      // no more output will be produced
        client->close_after_resp = true;  // close once the client drains what we have
        count_error(client);              // and it counts as a failure, not a finish
    }

} // namespace llmbridge::detail
