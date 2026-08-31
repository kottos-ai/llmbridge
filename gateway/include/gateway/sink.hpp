// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

/// One completed request's metadata, handed to an optional sink.
/// A stock build installs no sink and pays one predicted-false branch per request.
namespace llmbridge
{
    /// Header values the gateway may copy for the sink, chosen at set_request_sink.
    /// Bounded and copied at framing, because by completion the request buffer has
    /// been reused; a longer value is truncated to the cap.
    inline constexpr size_t kSinkCaptureMax = 2;
    inline constexpr size_t kSinkCaptureBytes = 48;

    /// Metadata only, like RequestFacts: no body, no credential, no header beyond
    /// the ones the integrator asked to capture. Timestamps are the gateway's
    /// monotonic stamps and 0 when never stamped,
    /// which is the case for replies the gateway itself generated.
    struct RequestRecord
    {
        uint64_t seq = 0;         ///< the global request sequencer
        int64_t wall_t0_ns = 0;   ///< CLOCK_REALTIME at framing; the cross-process merge key
        /// How long the request took to arrive: t0 minus the first byte seen. Zero
        /// when it arrived in one read.
        int64_t client_upload_ns = 0;
        /// Total request bytes on the wire, head plus body. Paired with
        /// client_upload_ns it gives effective upload throughput, which is what
        /// separates a full pipe from a congestion window still opening.
        uint64_t request_bytes = 0;
        int64_t ts_req_recvd = 0;  ///< t0
        int64_t ts_req_built = 0;  ///< t1
        int64_t ts_wire_ready = 0; ///< t2; == t1 when the connection was pooled
        int64_t ts_up_sent = 0;    ///< t3
        int64_t ts_up_recvd = 0;   ///< t4: ttfb
        int64_t ts_first_token = 0; ///< streaming only: ttft, the first token shown
        /// Streaming only, and 0 unless the model emitted reasoning. The first thinking delta.
        int64_t ts_first_thinking = 0;
        int64_t ts_done = 0;       ///< reply fully flushed, or the stream finished
        uint64_t tag = 0;         ///< Decision::tag, verbatim
        int status = 0;           ///< as sent to the client
        int upstream_index = -1;  ///< venue that served (or last tried)
        int attempts = 0;         ///< failovers before this outcome
        int32_t tokens_in = -1;   ///< streaming only, from the provider's usage; -1 unknown
        int32_t tokens_out = -1;
        int32_t cached_tokens = -1; ///< input tokens the provider served from cache; -1 unknown
        /// Input tokens written to the provider's prompt cache this request.
        int32_t cache_write_tokens = -1;
        /// That write split by the entry's lifetime, from `usage.cache_creation`. The
        /// two are priced differently, Anthropic billing the five-minute entry at 1.25x
        /// the input rate and the one-hour entry at 2x.
        int32_t cache_write_5m_tokens = -1;
        int32_t cache_write_1h_tokens = -1;
        bool streamed = false;
        /// The client sent a Content-Encoding other than identity. Every body-reading
        /// caller here assumes JSON, so this request was parsed as something it is not.
        bool client_encoded = false;
        bool error_reply = false; ///< the gateway generated the reply; stamps unset
        bool truncated = false;   ///< stream ended without a clean finish
        bool translated = false;  ///< dialect translation ran for the serving venue
        uint8_t backend = 0;      ///< 1 = epoll, 2 = io_uring
        /// The model the client asked for, empty when the body named none.
        std::string_view model;
        /// Valid only during on_request: they point into the connection. Copy out.
        std::string_view captured[kSinkCaptureMax];
    };

    /// Called on the loop thread once per completed request, streams included.
    /// noexcept and allocation-free expected: it sits after every reply.
    class RequestSink
    {
      public:
        virtual ~RequestSink() = default;
        virtual void on_request(const RequestRecord&) noexcept = 0;

      protected:
        RequestSink() = default;
        RequestSink(const RequestSink&) = default;
        RequestSink& operator=(const RequestSink&) = default;
    };
} // namespace llmbridge
