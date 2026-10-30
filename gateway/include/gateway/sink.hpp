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
    inline constexpr size_t kSinkCaptureMax = 3;
    inline constexpr size_t kSinkCaptureBytes = 48;
    /// The longest RequestSink::reply_note written; a longer one is dropped whole.
    inline constexpr size_t kReplyNoteMax = 1024;

    /// What Policy::confirm() made of a decision taken on the head of a body.
    enum class ConfirmOutcome : uint8_t
    {
        None,    ///< decided on the whole body, or confirm never ran
        Kept,    ///< the last byte went as decided
        Redone,  ///< the venue reset, the whole body sent again under a new decision
        Refused, ///< refused at the last byte, a venue holding part of the body reset
    };

    /// Metadata only, like RequestFacts: no body, no credential, no header beyond
    /// the ones the integrator asked to capture. Timestamps are the gateway's
    /// monotonic stamps and 0 when never stamped,
    /// which is the case for replies the gateway itself generated.
    struct RequestRecord
    {
        uint64_t seq = 0;         ///< the global request sequencer
        int64_t wall_t0_ns = 0;   ///< CLOCK_REALTIME at framing; the cross-process merge key
        /// How long the request took to arrive: its first byte to its last, which is t0,
        /// or `ts_body_end` for a streamed body. Zero when it arrived in one read.
        int64_t client_upload_ns = 0;
        /// Total request bytes on the wire, head plus body. Paired with
        /// client_upload_ns it gives effective upload throughput, which is what
        /// separates a full pipe from a congestion window still opening.
        uint64_t request_bytes = 0;
        bool client_conn_reused = false;
        /// Inbound leg, first request on the connection only, else 0: accept to its last
        /// byte, t0 or a streamed body's `ts_body_end`.
        /// Contains the TLS handshake, the client's idle time, and the request upload
        /// (`client_upload_ns`, subtract it). Excludes the TCP handshake because it
        /// completes before `accept()`. For handshake alone prefer the `accept(TLS)`.
        int64_t client_conn_setup_ns = 0;
        int64_t ts_req_recvd = 0;  ///< t0
        int64_t ts_req_built = 0;  ///< t1
        int64_t ts_wire_ready = 0; ///< t2; == t1 when the connection was pooled
        int64_t ts_up_sent = 0;    ///< t3: the head, and the body there at dispatch, in the kernel
        int64_t ts_up_recvd = 0;   ///< t4: ttfb
        int64_t ts_first_token = 0; ///< streaming only: the first token shown; ttft from ts_body_end
        /// Streaming only, and 0 unless the model emitted reasoning. The first thinking delta.
        int64_t ts_first_thinking = 0;
        /// Streaming only: the longest gap between two chunks after the first visible
        /// token, in nanoseconds.
        int64_t max_chunk_gap_ns = 0;
        /// Which quota the provider said was exhausted, and how long it asked us to
        /// wait, from the response headers.
        uint8_t quota_exhausted = 0;   ///< net::http::ResponseHead::Quota
        uint16_t retry_after_s = 0;
        int64_t ts_done = 0;       ///< reply fully flushed, or the stream finished
        uint64_t tag = 0;         ///< Decision::tag, verbatim
        int status = 0;           ///< as sent to the client
        int upstream_index = -1;  ///< venue that served (or last tried)
        uint32_t upstream_ip = 0;
        /// The kernel's round-trip estimates to the serving venue, read at its response
        /// head, microseconds; 0 when there was no head or no sample.
        uint32_t upstream_srtt_us = 0;
        uint32_t upstream_min_rtt_us = 0;
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
        /// The rest of the venue's token accounting. Reasoning is inside `tokens_out`
        /// (OpenAI's and the Responses API's reasoning_tokens, Anthropic's
        /// thinking_tokens, Gemini's thoughtsTokenCount); audio in and out are priced at
        /// their own rates; the prediction pair is OpenAI's predicted outputs, rejected
        /// ones billed and never shown; tool_prompt_tokens is Gemini's server-tool
        /// addition to the prompt; image_in_tokens is part of `tokens_in`.
        int32_t reasoning_tokens = -1;
        int32_t audio_in_tokens = -1;
        int32_t audio_out_tokens = -1;
        int32_t accepted_prediction_tokens = -1;
        int32_t rejected_prediction_tokens = -1;
        int32_t tool_prompt_tokens = -1;
        int32_t image_in_tokens = -1;
        /// Server tools billed per call: Anthropic's server_tool_use counts, and the
        /// Responses API's search calls counted from its output.
        int32_t web_search_requests = -1;
        int32_t web_fetch_requests = -1;
        int32_t code_exec_requests = -1;
        int32_t file_search_calls = -1;
        /// Gemini: URL context or code execution ran, so tool_prompt_tokens bill as input;
        /// web_search_requests holds its grounding queries. DESIGN.md "Gemini clients and Vertex".
        bool tool_prompt_input = false;
        bool stream_error = false; ///< a 200 stream then sent an error event; upstream_error names it
        /// The client fails a stream on an SSE comment line, so a reply_note would break it:
        /// google-genai for Python (`gl-python/` in x-goog-api-client) on a Gemini route.
        bool comment_unsafe = false;
        /// The venue's own price for this request in picodollars (DeepInfra's
        /// estimated_cost), a check on ours.
        int64_t venue_cost_pdollars = -1;
        /// Anthropic's `usage.iterations`, billed beside the counts above, which leave
        /// them out: a compaction pass on the served model, and attempts declined before
        /// a fallback, on `prior_model`. Inputs include their cached part, as tokens_in does.
        int32_t compaction_in_tokens = -1;
        int32_t compaction_out_tokens = -1;
        int32_t compaction_cached_tokens = -1;
        int32_t prior_in_tokens = -1;
        int32_t prior_out_tokens = -1;
        int32_t prior_cached_tokens = -1;
        int32_t prior_cache_write_tokens = -1;
        int32_t prior_attempts = -1;
        std::string_view prior_model;
        /// What served, as the venue says: Anthropic's `usage.speed` and
        /// `usage.inference_geo`, Bedrock's latency header. Each prices the request.
        std::string_view served_speed;
        std::string_view served_geo;
        std::string_view served_latency;
        /// The client's `prompt_cache_options.ttl`, which prices a write its venue
        /// reports without a lifetime.
        std::string_view asked_cache_ttl;
        bool streamed = false;
        /// The venue connection was reused from the pool. Not the same as
        /// `client_conn_reused`, which is the client's leg.
        bool from_pool = false;
        /// The client sent a Content-Encoding other than identity. Every body-reading
        /// caller here assumes JSON, so this request was parsed as something it is not.
        bool client_encoded = false;
        bool error_reply = false; ///< the gateway generated the reply; stamps unset
        bool truncated = false;   ///< stream ended without a clean finish
        bool translated = false;  ///< dialect translation ran for the serving venue
        uint8_t backend = 0;      ///< 1 = epoll, 2 = io_uring
        /// The model the client asked for, empty when the body named none.
        std::string_view model;
        /// The `service_tier` the client's own body carried, empty when it named none.
        std::string_view asked_tier;
        std::string_view served_tier;
        /// The model the venue's reply names, which is what the client was served.
        std::string_view served_model;
        /// The venue's own name for this failure, read from its error body when it
        /// answered 4xx or 5xx: `invalid_request_error`, `context_length_exceeded`,
        /// `overloaded_error`.
        std::string_view upstream_error;
        /// The venue's own id for this request.
        std::string_view venue_req_id;
        uint64_t prefix_hash = 0;
        /// Valid only during on_request: they point into the connection. Copy out.
        std::string_view captured[kSinkCaptureMax];
        /// A body forwarded to the venue as it arrived (LATENCY.md 2b). Held, these
        /// read t0, t3, 0 and false, so one formula serves every request.
        int64_t ts_body_end = 0;  ///< its last byte in from the client; 0 if cut short
        int64_t ts_body_sent = 0; ///< its last byte in the kernel: the venue's wait begins
        int64_t body_work_ns = 0; ///< scanning, copying and sending its parts after t3
        /// A venue got part of the body before its last byte, a redone attempt's too.
        bool body_streamed = false;
        /// The policy decided on the head (Policy::decides_on_head), and its confirm.
        /// With `body_streamed`, a confirm that redid or refused wasted a partial upload.
        bool decided_on_head = false;
        ConfirmOutcome confirm_outcome = ConfirmOutcome::None;
        /// Decision::substitute_model replaced the client's `model` (`model` above), spliced
        /// into a streamed body or rewritten in a held one; `served_model` is the venue's.
        bool model_substituted = false;
        /// Decision::service_tier, speed and inference_geo spliced into a streamed body at a
        /// confirm that kept it (GATEWAY-INTERNALS.md 12e); false when rewritten in a held
        /// body, or not set. `asked_tier` is the client's own tier the splice blanked.
        bool service_tier_spliced = false;
        bool speed_spliced = false;
        bool inference_geo_spliced = false;
        /// A hedged request (Decision::hedge) leaves one record per leg, each `hedged` and with
        /// its own `seq`. `hedge_group` is the client request's seq on every one of them, read
        /// only when `hedged` (a seq may be 0), and `hedge_leg` 0 is the decision's own venue.
        /// `hedge_won`: the leg that served the client. `hedge_cut`: reset unanswered, with
        /// `status` 0 and no token counts, because another leg won.
        bool hedged = false;
        uint64_t hedge_group = 0;
        uint8_t hedge_leg = 0;
        bool hedge_won = false;
        bool hedge_cut = false;
    };

    /// Called on the loop thread once per completed request, streams included.
    /// noexcept and allocation-free expected: it sits after every reply.
    class RequestSink
    {
      public:
        virtual ~RequestSink() = default;
        virtual void on_request(const RequestRecord&) noexcept = 0;
        /// Text for the client at the clean end of a streamed reply, written as one SSE
        /// comment line, `: <note>`, after the last event and before the stream's end.
        /// Called on the loop thread with the record as it stands then (status 200,
        /// streamed), before on_request; the view must stay valid until the next call.
        /// Empty sends nothing. A note over kReplyNoteMax bytes, or holding a CR, LF or
        /// NUL, is dropped whole, never cut or cleaned.
        virtual std::string_view reply_note(const RequestRecord&) noexcept { return {}; }

      protected:
        RequestSink() = default;
        RequestSink(const RequestSink&) = default;
        RequestSink& operator=(const RequestSink&) = default;
    };
} // namespace llmbridge
