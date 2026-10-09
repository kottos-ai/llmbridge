// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// What both event loops read and neither owns: the buffer and tick constants, the
// request sequencer, the credential scrub, and the three stream-lifecycle tests
// the two finalize paths share.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "gateway/gateway.hpp"
#include "net/secure.hpp"

namespace llmbridge::detail
{
    constexpr size_t kInitialBuf = 4096;
    constexpr int kEpMaxEvents = 1024;
    constexpr int kPollTickMs = 200; // so request_stop() is observed promptly
    // No request is larger, so more buffered client bytes than this is never legitimate.
    constexpr size_t kMaxClientBuffered = net::http::kMaxHeaderLen + net::http::kMaxBodyLen;
    // Listener pause after EMFILE/ENFILE: retrying at once is a busy loop.
    constexpr int64_t kAcceptBackoffNs = 100'000'000;
    // A pooled connection the provider had already closed fails within a round trip of
    // reuse. Later, the provider may have run the request: a resend would bill it twice.
    constexpr int64_t kStaleRetryWindowNs = 1'000'000'000;

    // A pooled upstream idles holding the request that carried the client's key, so its
    // buffer is scrubbed before it serves anyone else: GATEWAY-INTERNALS.md §9.
    using llmbridge::net::secure_clear;

    // Defined in gateway.cpp, beside the reasoning for it being a sequencer.
    extern std::atomic<uint64_t> g_seq;

    // May this streaming upstream go back into the pool? Every clause is load-bearing, and
    // any doubt closes it:
    //   stream_keep_alive  the provider did not say Connection: close
    //   stream_chunked     a close-delimited body cannot tell "finished" from "died"
    //   chunkdec.done()    the terminal chunk was consumed: a real message boundary
    //   rbuf empty         leftover bytes would be read as the next response
    //   !close_after_resp  aborted, corrupt or timed-out streams are never pooled
    inline bool stream_upstream_reusable(const Connection* client, const Connection* u) noexcept
    {
        return client != nullptr && u != nullptr && !u->doomed && u->fd >= 0
               && client->stream_keep_alive && client->stream_chunked
               && client->chunkdec.done() && u->rbuf.empty() && !client->close_after_resp;
    }

    /// Can reuse only if we framed the reply so the body has an end marker, the
    /// stream reached that marker, and the caller wanted the connection kept.
    [[nodiscard]] inline bool stream_client_reusable(const Connection* c) noexcept
    {
        return c->stream_chunked_out && !c->close_after_resp && c->msg.keep_alive;
    }

    /// Clear the per-stream state so the next request on this connection starts clean.
    inline void stream_reset_for_next(Connection* c) noexcept
    {
        c->streaming = false;
        c->stream_ended = false;
        c->stream_chunked = false;
        c->stream_chunked_out = false;
        c->stream_keep_alive = false;
        c->wants_usage = false;
        c->sse_xlate.reset();
        c->chunkdec = net::http::ChunkDecoder{};
        c->stream_tail.clear();
        c->sse_scratch.clear();
        // The counts the stream produced.
        c->usage_in = c->usage_out = c->usage_cached = c->usage_cache_write = -1;
        c->usage_cw_5m = c->usage_cw_1h = -1;
        c->usage_reasoning = c->usage_audio_in = c->usage_audio_out = -1;
        c->usage_accepted_pred = c->usage_rejected_pred = c->usage_tool_prompt = -1;
        c->ts_first_token = 0;
        c->ts_first_thinking = 0;
        c->ts_last_chunk = 0;
        c->max_chunk_gap_ns = 0;
        c->served_tier_len = 0;
        c->served_tier_tries = 0;
        c->served_model_len = 0;
        c->served_model_tries = 0;
        c->venue_req_id_len = 0;
    }

} // namespace llmbridge::detail
