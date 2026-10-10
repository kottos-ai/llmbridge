// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The request sink: header capture and the model read at framing, the record at
// completion.

#include "gateway/gateway.hpp"

#include "loop.hpp"
#include "net/sha256.hpp"
#include "request.hpp"
#include "stream.hpp"

#include <cstring>
#include <ctime>
#include <string_view>

namespace llmbridge
{
    using namespace detail;

    // The header names that carry a credential, which is the set this gateway extracts
    // and re-emits: authorization and x-api-key (Bearer / Anthropic), x-goog-api-key
    // (Gemini), and api-key (Azure).
    static bool is_credential_header(std::string_view lowered) noexcept
    {
        return lowered == "authorization" || lowered == "x-api-key" ||
               lowered == "x-goog-api-key" || lowered == "api-key";
    }

    void Gateway::set_request_sink(RequestSink* sink, std::vector<std::string> capture)
    {
        _sink = sink;
        _sink_capture_names.clear();
        for (std::string& h : capture)
        {
            if (_sink_capture_names.size() == kSinkCaptureMax) break;
            for (char& ch : h)
                ch = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
            if (is_credential_header(h))
            {
                LB_ERROR("sink capture refused for header ", h.c_str(),
                         ": it carries a credential, which must never reach a sink");
                continue;
            }
            _sink_capture_names.push_back(std::move(h));
        }
    }

    void Gateway::sink_capture(Connection* c) noexcept
    {
        // The request buffer is reused long before completion, so the sink's header
        // values are copied now, bounded, and the wall clock (the cross-process
        // merge key; the ts_* stamps are monotonic) is taken in the same breath.
        timespec tw{};
        clock_gettime(CLOCK_REALTIME, &tw);
        c->wall_t0 = static_cast<int64_t>(tw.tv_sec) * 1000000000 + tw.tv_nsec;
        const std::string_view head(c->rbuf.data(), c->msg.header_len);
        for (size_t i = 0; i < kSinkCaptureMax; ++i)
        {
            c->sink_cap_len[i] = 0;
            if (i >= _sink_capture_names.size()) continue;
            const std::string_view v = net::http::find_header(head, _sink_capture_names[i]);
            const size_t n = v.size() < kSinkCaptureBytes ? v.size() : kSinkCaptureBytes;
            // Guarded, because an absent header is the common case and find_header
            // returns a null view for it: memcpy's arguments are declared non-null
            // even when the length is 0, so the unguarded form is undefined
            // behaviour.
            if (n) std::memcpy(c->sink_cap[i], v.data(), n);
            c->sink_cap_len[i] = static_cast<uint8_t>(n);
        }
    }

    // The one body read on the byte-forward path, and it happens only with a sink or
    // a policy installed. A stock build parses nothing here, as before.
    // Split out of sink_capture because a policy needs it too and needs it before the
    // decision, while sink_capture's other work is only ever read at the end.
    const char* Gateway::capture_model(Connection* c) noexcept
    {
        c->sink_model_len = 0;
        c->prefix_hash = 0;
        const std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
        // One walk of the top level for everything a policy or the sink reads: a key
        // sent last, as the OpenAI SDK sends them, costs a whole-body walk per read.
        const provider::TopLevelFacts top =
            detail::body_facts(std::string_view(c->rbuf.data(), c->msg.header_len), body);
        c->asked_stream = top.stream;
        c->asked_usage = top.include_usage;
        // What a policy or the sink reads here must be what the provider reads. Our
        // readers take the first raw match; a provider unescapes and may take the last.
        switch (top.keys)
        {
            case provider::KeyCheck::Ok: break;
            case provider::KeyCheck::DuplicateKey: return "duplicate top-level key";
            case provider::KeyCheck::EscapedKey: return "escaped top-level key";
            case provider::KeyCheck::TooManyKeys: return "too many top-level keys";
        }
        const std::string_view model = top.model;
        if (!model.empty() && model.size() <= sizeof(c->sink_model))
        {
            std::memcpy(c->sink_model, model.data(), model.size());
            c->sink_model_len = static_cast<uint8_t>(model.size());
        }
        if (_wants_prefix_hash && !body.empty())
        {
            const size_t n = body.size() < kPrefixHashBytes ? body.size() : kPrefixHashBytes;
            // noexcept end to end and no allocation, so there is nothing to catch: the
            // path is exception-free by construction
            c->prefix_hash = net::Sha256::truncated(net::Sha256::hash(body.substr(0, n)));
        }
        return nullptr;
    }

    void Gateway::sink_emit(Connection* c, int status, bool streamed) noexcept
    {
        RequestRecord r;
        r.seq = c->req_seq;
        r.wall_t0_ns = c->wall_t0;
        r.client_upload_ns = c->client_upload_ns;
        r.client_conn_reused = c->client_conn_reused;
        r.client_conn_setup_ns = c->client_conn_setup_ns;
        r.request_bytes = c->msg.total_len;
        r.client_encoded = c->msg.encoded;
        r.ts_req_recvd = c->ts_req_recvd;
        r.ts_req_built = c->ts_req_built;
        r.ts_wire_ready = c->ts_wire_ready;
        r.ts_up_sent = c->ts_up_sent;
        r.ts_up_recvd = c->ts_up_recvd;
        r.ts_first_token = c->ts_first_token;
        r.ts_first_thinking = c->ts_first_thinking;
        r.max_chunk_gap_ns = c->max_chunk_gap_ns;
        r.quota_exhausted = c->quota_exhausted;
        r.retry_after_s = c->retry_after_s;
        r.ts_done = now_ns();
        r.tag = c->policy_tag;
        r.status = status;
        r.upstream_index = c->upstream_slot;
        r.upstream_ip = c->upstream_ip;
        r.attempts = c->failover_attempts;
        r.streamed = streamed;
        r.error_reply = !streamed && c->close_after_resp;
        r.truncated = streamed && c->close_after_resp;
        r.translated = c->translate_body;
        r.backend = _active_backend;
        r.model = std::string_view(c->sink_model, c->sink_model_len);
        r.asked_tier = std::string_view(c->asked_tier, c->asked_tier_len);
        r.upstream_error = std::string_view(c->upstream_error, c->upstream_error_len);
        r.venue_req_id = std::string_view(c->venue_req_id, c->venue_req_id_len);
        r.prefix_hash = c->prefix_hash;
        r.served_tier = std::string_view(c->served_tier, c->served_tier_len);
        r.served_model = std::string_view(c->served_model, c->served_model_len);
        r.from_pool = c->upstream_pooled;
        // A stream's counts from its translator or its own usage events; -1 is "not
        // reported" and not "zero".
        const BodyUsage u = streamed ? stream_tokens(c) : c->tok;
        r.tokens_in = static_cast<int32_t>(u.in);
        r.tokens_out = static_cast<int32_t>(u.out);
        r.cached_tokens = static_cast<int32_t>(u.cached);
        r.cache_write_tokens = static_cast<int32_t>(u.cache_write);
        r.cache_write_5m_tokens = static_cast<int32_t>(u.cache_write_5m);
        r.cache_write_1h_tokens = static_cast<int32_t>(u.cache_write_1h);
        r.reasoning_tokens = static_cast<int32_t>(u.reasoning);
        r.audio_in_tokens = static_cast<int32_t>(u.audio_in);
        r.audio_out_tokens = static_cast<int32_t>(u.audio_out);
        r.accepted_prediction_tokens = static_cast<int32_t>(u.accepted_prediction);
        r.rejected_prediction_tokens = static_cast<int32_t>(u.rejected_prediction);
        r.tool_prompt_tokens = static_cast<int32_t>(u.tool_prompt);
        for (size_t i = 0; i < kSinkCaptureMax; ++i)
            r.captured[i] = std::string_view(c->sink_cap[i], c->sink_cap_len[i]);
        _sink->on_request(r);
        // Consumed. A framing error never reaches the per-request reset (it fails
        // before framing succeeds), so without this its 400's record would carry the
        // Previous request's captures and tag on a keep-alive connection.
        c->sink_cap_len[0] = c->sink_cap_len[1] = 0;
        c->wall_t0 = 0;
        c->policy_tag = 0;
        // Streaming usage, for the same reason and found by the same argument: a
        // keep-alive client's second stream inherited the first one's token counts,
        // because nothing cleared them between requests.
        c->stream_usage.reset();
        c->sse_scratch.clear();
        c->sse_scratch.shrink_to_fit();
        // The non-streaming counters. They are assigned only where a body is scanned,
        // so a keep-alive request that fails before that (an upstream non-200, a translate failure)
        //  emitted a record carrying the token counts of the request before it.
        c->tok = {};
        c->ts_first_token = 0;
        // Per request, like the stamps around it: a pooled connection serving the next
        // caller must not report the last one's tier.
        c->tier_override = {};
        c->asked_tier_len = 0;
        c->upstream_error_len = 0;
        c->venue_req_id_len = 0;
        c->served_tier_len = 0;
        c->served_tier_tries = 0;
        c->served_model_len = 0;
        c->served_model_tries = 0;
        c->upstream_pooled = false;
        c->upstream_ip = 0;
        c->ts_first_thinking = 0;
        c->ts_last_chunk = 0;
        c->max_chunk_gap_ns = 0;
        c->quota_exhausted = 0;
        c->retry_after_s = 0;
        // Defensive, not a fixed bug: the only reader of `wants_usage` runs on a
        // translated Anthropic request, and that translation writes the field.
        c->wants_usage = false;
    }

} // namespace llmbridge
