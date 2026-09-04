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

    // A credential is a transient string_view over the client's request buffer,
    // written straight into the upstream bytes and never copied anywhere that
    // outlives the request. What does outlive it is the pooled upstream, which idles
    // up to _pool_idle_ns (30 s default) holding the request that carried the key,
    // so that buffer is scrubbed before it can serve another client. See
    // net/secure.hpp for why this is not a plain memset. Measured: 2.4 ns for a
    // typical ~96 B request buffer, once per request (~0.02% of one core at 84k RPS).
    using llmbridge::net::secure_clear;

    // Defined in gateway.cpp, beside the reasoning for it being a sequencer.
    extern std::atomic<uint64_t> g_seq;

    // May this streaming upstream go back into the keep-alive pool?
    //
    // Same spirit as the non-streaming rule ("pool only what will stay open"),
    // plus the framing conditions that make the end of the body knowable. Every
    // clause is load-bearing:
    //
    //   stream_keep_alive  the provider didn't say Connection: close; pooling a
    //                      conn it is about to close just buys a retry later
    //   stream_chunked     a close-delimited body has no end marker except EOF,
    //                      so "the response finished" and "the connection died"
    //                      are indistinguishable, so never reuse one
    //   chunkdec.done()    the terminal 0-length chunk was consumed, so we are
    //                      at a real message boundary instead of mid-body
    //   rbuf empty         no trailing/pipelined bytes left over; anything still
    //                      buffered would be mis-read as the next response
    //   !close_after_resp  aborted, corrupt, or idle-timed-out streams are never
    //                      pooled; we don't trust framing we already distrusted
    //
    // Conservative by construction: any doubt falls through to close, which is
    // exactly the behaviour that shipped before reuse existed.
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
        c->ts_first_token = 0;
        c->ts_first_thinking = 0;
        c->ts_last_chunk = 0;
        c->max_chunk_gap_ns = 0;
        c->served_tier_len = 0;
        c->served_tier_tries = 0;
    }

} // namespace llmbridge::detail
