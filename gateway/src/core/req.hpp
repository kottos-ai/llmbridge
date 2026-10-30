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
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>

#include "gateway/policy.hpp"
#include "gateway/sink.hpp"
#include "gateway/venue.hpp"
#include "net/http.hpp"
#include "provider/body_scan.hpp"
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
        /// Keeps at most N bytes; false when `v` did not fit. `v` may be this one's own view.
        bool set(std::string_view v) noexcept
        {
            n = static_cast<uint8_t>(v.size() < N ? v.size() : N);
            if (n) std::memmove(b, v.data(), n);
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

    /// What the gateway reads from a client's request head in the framing walk, as
    /// offsets from the head's start: a request put back at the front of its buffer
    /// for a failover reads the same. A header the operator strips is not seen, but for
    /// the key family, which never leaves anyway.
    struct HeadFacts
    {
        struct Span
        {
            uint32_t at = 0, len = 0;
            [[nodiscard]] std::string_view in(std::string_view head) const noexcept
            {
                return head.substr(at, len);
            }
            [[nodiscard]] bool empty() const noexcept { return len == 0; }
        };
        /// The first of each credential, trimmed.
        Span authorization, x_api_key, api_key, x_goog_api_key, anthropic_version;
        /// Each Connection line's value; more than kConnections refuse a byte-forward.
        static constexpr uint8_t kConnections = 4;
        Span connection[kConnections];
        uint8_t connections = 0;
        uint8_t expect = 0; ///< the first Expect: 0 none, 1 another, 2 100-continue
        bool content_length = false;
        Span content_type; ///< the first Content-Type value, as sent
        /// Each family line a policy could name, in order, and its trimmed value. Past
        /// kKeyHeaders the count stops at one more, and a named key is refused.
        static constexpr uint8_t kKeyHeaders = 8;
        Span key_name[kKeyHeaders], key_value[kKeyHeaders];
        uint8_t key_headers = 0;
        /// The first anthropic-beta value, trimmed, and how many lines named one, stopping at 2.
        Span anthropic_beta;
        uint8_t anthropic_betas = 0;
    };

    /// A hedge leg as Decision::hedge named it, copied: GATEWAY-INTERNALS.md 13.
    struct HedgeLegCopy
    {
        int venue = -1;
        FixedStr<255> model;
        FixedStr<32> tier, speed, geo;
        FixedStr<kMaxBetaToken> beta;
        FixedStr<kMaxKeyHeaderName> key;
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
        /// A streamed body's last byte in from the client and out to the kernel, and
        /// our work on its later parts (LATENCY.md 2b); held, t0, t3 and 0.
        int64_t ts_body_end = 0;
        int64_t ts_body_sent = 0;
        int64_t body_work_ns = 0;
        /// The attempt's first send, the head and the body there at dispatch: its
        /// plaintext length, and the upstream's sent_bytes once it is in, which t3 waits for.
        uint64_t first_send_len = 0;
        uint64_t first_send_end = 0;
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
        /// Streaming: `streaming` latches in begin_stream; `stream_body` is the
        /// upstream's framing (`stream_left` bytes to go when it is a Content-Length),
        /// `stream_chunked_out` ours; `stream_ended` with `close_after_resp` set means
        /// truncated (GATEWAY-INTERNALS.md 6b). `stream_terminated`: the terminating chunk,
        /// and a sink's note before it, are staged; `stream_tail`: the last four bytes a
        /// byte-forward sent, which a note must follow as a blank line (6d).
        uint64_t stream_left = 0;
        net::http::Body stream_body = net::http::Body::None;
        bool streaming = false;
        bool stream_chunked_out = false;
        bool stream_ended = false;
        bool stream_terminated = false;
        bool stream_keep_alive = false; ///< the upstream may be pooled at the end
        char stream_tail[4] = {};
        bool sse_translating = false;
        bool responses_api = false; ///< its search calls are counted for the sink
        Phase phase = Phase::Idle;
        uint8_t quota_exhausted = 0;
        uint16_t retry_after_s = 0;
        uint8_t served_tier_tries = 0; ///< reads searched, so a venue without it is given up on
        uint8_t served_model_tries = 0;
        /// Copied from the policy's Decision, or from Retry on failover; never longer.
        FixedStr<255> model_override;
        FixedStr<32> tier_override;
        FixedStr<32> speed_override;
        FixedStr<32> geo_override;
        FixedStr<kMaxBetaToken> beta_override; ///< added to anthropic-beta at an x-api-key venue
        FixedStr<kMaxKeyHeaderName> key_header; ///< the family header holding this attempt's key
        FixedStr<64> model;          ///< the client's, for the policy and the sink
        FixedStr<16> asked_tier;     ///< the client's own service_tier
        FixedStr<8> asked_cache_ttl; ///< the client's prompt_cache_options.ttl
        FixedStr<32> upstream_error; ///< the venue's name for a failure
        FixedStr<64> venue_req_id;
        FixedStr<32> served_tier;    ///< 32: Vertex's PROVISIONED_THROUGHPUT is 22
        FixedStr<16> served_latency; ///< Bedrock's performance config, from its header
        FixedStr<64> served_model;   ///< cut at 64: longer ids are Bedrock ARNs
        FixedStr<kSinkCaptureBytes> captured[kSinkCaptureMax];
        HeadFacts head;
        /// A body forwarded as it arrives (GATEWAY-INTERNALS.md 12): bytes of it staged
        /// upstream and in from the client, dropped ones too, more still due, the request
        /// still at the front of rbuf for a resend, and each chunk checked by BodyScan
        /// first (an OpenAI client). `body_streamed` stays set once the request is done.
        uint32_t body_sent = 0;
        uint32_t body_in = 0;
        bool body_streaming = false;
        bool body_in_rbuf = false;
        bool body_scanned = false;
        bool body_streamed = false;
        /// A policy that decides on the head (GATEWAY-INTERNALS.md 12c): decided before the
        /// body was whole, its last byte withheld until confirm(), parked unsent in rbuf to
        /// its last byte, the decision's hold, and what confirm made of it. `body_parts`:
        /// the attempt went in parts, so a resend rebuilds it from rbuf.
        bool decided_on_head = false;
        bool confirm_pending = false;
        bool body_parked = false;
        bool held = false;
        bool body_parts = false;
        bool reply_kept = false; ///< our error reply keeps the connection; the rest is dropped
        ConfirmOutcome confirm_outcome = ConfirmOutcome::None;

        /// A body override is in force, so the body is rewritten when sent whole; streamed,
        /// only the members can be spliced in (member_overrides), never a model.
        [[nodiscard]] bool rewrites_body() const noexcept
        {
            return !((model_in_path || model_override.empty()) && tier_override.empty() &&
                     speed_override.empty() && geo_override.empty());
        }
        /// The client names its model in the path (Gemini), so a model override or
        /// substitution rewrites the path and never the body, streamed or not.
        bool model_in_path = false;
        /// The venue answers in Gemini's shape (gemini_facts): its model, id, tier, server
        /// tools and a stream's error event are in its reply objects, not its headers.
        bool gemini_reply = false;
        bool stream_error = false;      ///< a 200 stream sent an error event
        bool tool_prompt_input = false; ///< URL context or code execution ran
        bool comment_unsafe = false;    ///< RequestRecord::comment_unsafe, read at framing
        int32_t search_queries = -1;    ///< grounding queries, the most any one event names
        bool event_line_dropped = false; ///< the Gemini line arriving is over kGeminiLineMax
        /// The model a path names in place of the client's: the override, else the
        /// substitution when the client's is its sentinel; empty sends the client's.
        [[nodiscard]] std::string_view path_model() const noexcept
        {
            if (!model_in_path) return {};
            if (!model_override.empty()) return model_override.view();
            return !subst_model.empty() && model.view() == subst_for.view() ? subst_model.view() : std::string_view{};
        }
        /// A conditional model substitution (GATEWAY-INTERNALS.md 12d), the decision's or the
        /// retry's. `subst_tracked`: BodyScan has tracked the model from the first byte, so a
        /// streamed attempt can splice; `splice`: this attempt does; `model_substituted`: the
        /// venue got the replacement, spliced or rewritten whole.
        FixedStr<kMaxSubstituteModel> subst_model;
        FixedStr<kMaxSubstituteFor> subst_for;
        bool subst_tracked = false;
        bool splice = false;
        bool model_substituted = false;
        /// The service_tier, speed and inference_geo overrides spliced into a streamed body
        /// (GATEWAY-INTERNALS.md 12e), as BodyScan bits: those it holds the client's own
        /// member back for from the first byte, so an attempt may splice any of them; and
        /// those the venue got spliced, at a confirm that kept the body.
        uint8_t members_tracked = 0;
        uint8_t members_spliced = 0;

        /// A hedge (GATEWAY-INTERNALS.md 13): the legs a decision named beside its own, sent
        /// once the body is whole, and from then `hedged` on every leg. On each leg's record:
        /// the client request's seq (which may be 0, so never the mark of a hedge), the leg,
        /// and whether it served the client or was cut. `error_owed`: a leg failed, counted in
        /// `errors` only if it reaches the client. `sunk`: this request's record has gone, or
        /// is not to go, to the sink.
        HedgeLegCopy hedge[kMaxHedgeLegs];
        uint64_t hedge_group = 0;
        uint8_t hedge_n = 0;
        uint8_t hedge_leg = 0;
        bool hedged = false;
        bool hedge_won = false;
        bool hedge_cut = false;
        bool error_owed = false;
        bool sunk = false;

        /// BodyScan's bits for the service_tier, speed and inference_geo overrides in force.
        [[nodiscard]] uint8_t member_overrides() const noexcept
        {
            return static_cast<uint8_t>((tier_override.empty() ? 0 : provider::BodyScan::kServiceTier) |
                                        (speed_override.empty() ? 0 : provider::BodyScan::kSpeed) |
                                        (geo_override.empty() ? 0 : provider::BodyScan::kInferenceGeo));
        }
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
        std::string event_line; ///< a Gemini stream's line still arriving (note_gemini_events)
        /// Client bytes still owed to a request that is over, dropped as they arrive;
        /// then close if the reply said so. Survive begin(): they precede the next frame.
        uint64_t discard = 0;
        bool close_on_drain = false;
        std::unique_ptr<provider::BodyScan> scan; ///< made at the first streamed body

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
            event_line.clear();
        }
        [[nodiscard]] bool can_redispatch() const noexcept { return f.phase == Phase::Dispatched; }
    };
} // namespace llmbridge
