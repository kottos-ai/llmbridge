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

#include "core/conn.hpp"
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
               && client->req.f.stream_keep_alive && client->req.f.stream_chunked
               && client->req.chunkdec.done() && u->rbuf.empty() && !client->close_after_resp;
    }

    /// Can reuse only if we framed the reply so the body has an end marker, the
    /// stream reached that marker, and the caller wanted the connection kept.
    [[nodiscard]] inline bool stream_client_reusable(const Connection* c) noexcept
    {
        return c->req.f.stream_chunked_out && !c->close_after_resp && c->msg.keep_alive;
    }

} // namespace llmbridge::detail

namespace llmbridge
{
    inline const Upstream& Gateway::upstream_of(const Connection* c) const noexcept
    {
        const size_t i = (c->upstream_slot >= 0) ? static_cast<size_t>(c->upstream_slot) : 0;
        return _upstreams[i < _upstreams.size() ? i : 0];
    }
} // namespace llmbridge
