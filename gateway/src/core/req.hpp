// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// One request's state, reset in one place: RequestCtx::begin() at framing.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>

#include "gateway/sink.hpp"
#include "gateway/venue.hpp"
#include "net/http.hpp"
#include "provider/openai.hpp"

namespace llmbridge
{
    /// A bounded copy, so nothing per-request is a view into a buffer that is reused.
    template <size_t N> struct FixedStr
    {
        static_assert(N > 0 && N <= 255);
        FixedStr() noexcept {} // b[] stays unwritten: a reset stores only n
        char b[N];
        uint8_t n = 0;
        /// Keeps at most N bytes; false when `v` did not fit.
        bool set(std::string_view v) noexcept
        {
            n = static_cast<uint8_t>(v.size() < N ? v.size() : N);
            if (n) std::memcpy(b, v.data(), n);
            return v.size() <= N;
        }
        [[nodiscard]] std::string_view view() const noexcept { return {b, n}; }
        [[nodiscard]] bool empty() const noexcept { return n == 0; }
    };

    /// Where a request stands. Retry and failover are legal only while Dispatched, before
    /// any response byte; a reply finishes only from Replying, a stream from Streaming.
    enum class Phase : uint8_t
    {
        Idle,       ///< nothing in flight
        Dispatched, ///< being sent upstream, no response byte yet
        Responding, ///< response bytes arrived
        Streaming,  ///< the SSE head is staged for the client
        Replying,   ///< a whole reply is staged for the client
    };

    /// Everything a request owns that is trivially copyable. Never assigned field by
    /// field to reset it: begin() constructs a fresh one in place.
    struct ReqState
    {
        ReqState() noexcept {}

        uint64_t req_seq = 0;    ///< the sequencer value, assigned first at framing
        uint64_t policy_tag = 0; ///< Decision::tag; see policy.hpp
        uint64_t prefix_hash = 0;
        /// CLOCK_REALTIME at framing, for the sink, and t0-t4 (LATENCY.md). t1 and t2
        /// are apart because a cold connect puts ~50 ms between them.
        int64_t wall_t0 = 0;
        int64_t ts_req_recvd = 0;
        int64_t ts_req_built = 0;
        int64_t ts_wire_ready = 0;
        int64_t ts_up_sent = 0;
        int64_t ts_up_recvd = 0;
        /// Streaming, outside t0-t6: first content token, first thinking delta, the
        /// latest chunk and the longest gap between chunks once tokens started.
        int64_t ts_first_token = 0;
        int64_t ts_first_thinking = 0;
        int64_t ts_last_chunk = 0;
        int64_t max_chunk_gap_ns = 0;
        /// Last progress on either leg: the request forwarded or sent, a response byte
        /// read, or a byte the client took. The idle sweep measures against it.
        int64_t ts_progress = 0;
        int64_t client_upload_ns = 0; ///< t0 minus the first byte: the client's network
        int64_t client_conn_setup_ns = 0;
        /// Non-streamed token counts, -1 when not stated.
        provider::openai::Usage tok{};
        uint32_t upstream_ip = 0;
        /// The kernel's RTT to the serving venue at its response head; see net::TcpRtt.
        uint32_t upstream_srtt_us = 0;
        uint32_t upstream_min_rtt_us = 0;
        int failover_attempts = 0; ///< venues already tried
        /// The translation resolved for the venue in flight; `effective_dialect` only
        /// means something when `translate_body`.
        UpstreamDialect effective_dialect = UpstreamDialect::OpenAI;
        bool translate_body = false;
        /// The client's top-level `stream` and `include_usage`, read with the model.
        bool asked_stream = false;
        bool asked_usage = false;
        bool wants_usage = false; ///< include_usage, as the translator read it
        bool client_conn_reused = false;
        bool upstream_pooled = false; ///< the upstream was already connected
        /// Streaming: `streaming` latches in *_begin_stream; `stream_chunked` is the
        /// upstream's framing, `stream_chunked_out` ours; `stream_ended` with
        /// `close_after_resp` set means truncated (GATEWAY-INTERNALS.md 6b).
        bool streaming = false;
        bool stream_chunked = false;
        bool stream_chunked_out = false;
        bool stream_ended = false;
        bool stream_keep_alive = false; ///< the upstream may be pooled at the end
        bool sse_translating = false;
        Phase phase = Phase::Idle;
        uint8_t quota_exhausted = 0;
        uint16_t retry_after_s = 0;
        uint8_t served_tier_tries = 0; ///< reads searched, so a venue without it is given up on
        uint8_t served_model_tries = 0;
        /// Copied from the policy's Decision, or from Retry on failover; never longer.
        FixedStr<255> model_override;
        FixedStr<32> tier_override;
        FixedStr<64> model;          ///< the client's, for the policy and the sink
        FixedStr<16> asked_tier;     ///< the client's own service_tier
        FixedStr<32> upstream_error; ///< the venue's name for a failure
        FixedStr<64> venue_req_id;
        FixedStr<16> served_tier;
        FixedStr<64> served_model;   ///< cut at 64: longer ids are Bedrock ARNs
        FixedStr<kSinkCaptureBytes> captured[kSinkCaptureMax];
    };
    static_assert(std::is_trivially_copyable_v<ReqState>);
    static_assert(std::is_trivially_destructible_v<ReqState>);

    /// A request: its state, plus the buffers and decoders kept across requests.
    struct RequestCtx
    {
        ReqState f;
        /// The client's original request, kept only when a failover could resend it.
        std::string saved;
        net::http::ChunkDecoder chunkdec;        ///< the upstream's chunked stream body
        provider::openai::StreamUsage stream_usage; ///< usage a byte-forwarded stream states
        std::string sse_scratch;                 ///< one stream step's decoded bytes

        /// The one per-request reset; the sequence number is the first thing set, so
        /// even a framing error's reply carries its own.
        void begin(uint64_t seq) noexcept
        {
            ::new (static_cast<void*>(&f)) ReqState;
            f.req_seq = seq;
            saved.clear();
            chunkdec = net::http::ChunkDecoder{};
            stream_usage.reset();
            sse_scratch.clear();
        }
        [[nodiscard]] bool can_redispatch() const noexcept { return f.phase == Phase::Dispatched; }
    };
} // namespace llmbridge
