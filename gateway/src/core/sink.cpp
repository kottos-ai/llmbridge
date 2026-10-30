// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The request sink: header capture and the model read at framing, a stream's note at
// its clean end, the record at completion.

#include "engine.hpp"

#include "core/limits.hpp"
#include "net/sha256.hpp"
#include "request.hpp"
#include "stream.hpp"

#include <cstring>
#include <ctime>
#include <string_view>

namespace llmbridge::detail
{

    // The header names that carry a credential, which is the set this gateway extracts
    // and re-emits: authorization and x-api-key (Bearer / Anthropic), x-goog-api-key
    // (Gemini), api-key (Azure), and the key family.
    static bool is_credential_header(std::string_view lowered) noexcept
    {
        return lowered == "authorization" || lowered == "x-api-key" ||
               lowered == "x-goog-api-key" || lowered == "api-key" ||
               lowered.starts_with(kKeyHeaderPrefix);
    }

    void Engine::set_request_sink(RequestSink* sink, std::vector<std::string> capture)
    {
        _sink = sink;
        _sink_capture_names.clear();
        for (std::string& h : capture)
        {
            if (_sink_capture_names.size() == kSinkCaptureMax) break;
            for (char& ch : h)
                ch = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
            // Checked as find_header will read it, without its colon. One left inside
            // names no header, yet would match into a value.
            while (!h.empty() && (h.back() == ':' || h.back() == ' ' || h.back() == '\t')) h.pop_back();
            if (is_credential_header(h) || h.find(':') != std::string::npos)
            {
                LB_ERROR("sink capture refused for header ", h.c_str(),
                         ": it can carry a credential, which must never reach a sink");
                continue;
            }
            _sink_capture_names.push_back(std::move(h));
        }
    }

    void Engine::sink_capture(ClientConn* c) noexcept
    {
        // The request buffer is reused long before completion, so the sink's header
        // values are copied now, bounded, and the wall clock (the cross-process
        // merge key; the ts_* stamps are monotonic) is taken in the same breath.
        timespec tw{};
        clock_gettime(CLOCK_REALTIME, &tw);
        c->req.f.wall_t0 = static_cast<int64_t>(tw.tv_sec) * 1000000000 + tw.tv_nsec;
        const std::string_view head(c->rbuf.data(), c->msg.header_len);
        c->req.f.responses_api = responses_api(head); // read at the reply, when rbuf has moved on
        c->req.f.comment_unsafe = comment_unsafe_client(head);
        for (size_t i = 0; i < kSinkCaptureMax && i < _sink_capture_names.size(); ++i)
            c->req.f.captured[i].set(net::http::find_header(head, _sink_capture_names[i]));
    }

    // The one body read on the byte-forward path, and it happens only with a sink or
    // a policy installed. A stock build parses nothing here, as before.
    // Split out of sink_capture because a policy needs it too and needs it before the
    // decision, while sink_capture's other work is only ever read at the end.
    const char* Engine::capture_model(ClientConn* c) noexcept
    {
        const std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
        // One walk of the top level for everything a policy or the sink reads: a key
        // sent last, as the OpenAI SDK sends them, costs a whole-body walk per read.
        const provider::TopLevelFacts top =
            detail::body_facts(std::string_view(c->rbuf.data(), c->msg.header_len), body);
        c->req.f.asked_stream = top.stream;
        c->req.f.asked_usage = top.include_usage;
        c->req.f.asked_cache_ttl.set(top.cache_ttl);
        // What a policy or the sink reads here must be what the provider reads. Our
        // readers take the first raw match; a provider unescapes and may take the last.
        switch (top.keys)
        {
            case provider::KeyCheck::Ok: break;
            case provider::KeyCheck::DuplicateKey: return "duplicate top-level key";
            case provider::KeyCheck::EscapedKey: return "escaped top-level key";
            case provider::KeyCheck::TooManyKeys: return "too many top-level keys";
        }
        // A longer name is left out, not cut: a prefix could name another model.
        if (top.model.size() <= sizeof c->req.f.model.b) c->req.f.model.set(top.model);
        if (_wants_prefix_hash && !body.empty())
            c->req.f.prefix_hash = prefix_hash_of(body.substr(0, body.size() < kPrefixHashBytes ? body.size() : kPrefixHashBytes));
        return nullptr;
    }

    uint64_t Engine::prefix_hash_of(std::string_view lead) noexcept
    {
        // noexcept end to end and no allocation, so there is nothing to catch: the path
        // is exception-free by construction
        return net::Sha256::truncated(net::Sha256::hash(lead));
    }

    RequestRecord Engine::record_of(const ClientConn* c, int status, bool streamed, BodyUsage& u) const noexcept
    {
        RequestRecord r;
        r.seq = c->req.f.req_seq;
        r.wall_t0_ns = c->req.f.wall_t0;
        r.client_upload_ns = c->req.f.client_upload_ns;
        r.client_conn_reused = c->req.f.client_conn_reused;
        r.client_conn_setup_ns = c->req.f.client_conn_setup_ns;
        r.request_bytes = c->msg.total_len;
        r.client_encoded = c->msg.encoded;
        r.ts_req_recvd = c->req.f.ts_req_recvd;
        r.ts_req_built = c->req.f.ts_req_built;
        r.ts_wire_ready = c->req.f.ts_wire_ready;
        r.ts_up_sent = c->req.f.ts_up_sent;
        r.ts_up_recvd = c->req.f.ts_up_recvd;
        r.ts_first_token = c->req.f.ts_first_token;
        r.ts_first_thinking = c->req.f.ts_first_thinking;
        r.max_chunk_gap_ns = c->req.f.max_chunk_gap_ns;
        r.quota_exhausted = c->req.f.quota_exhausted;
        r.retry_after_s = c->req.f.retry_after_s;
        r.ts_done = now_ns();
        r.tag = c->req.f.policy_tag;
        r.status = status;
        r.upstream_index = c->upstream_slot;
        r.upstream_ip = c->req.f.upstream_ip;
        r.upstream_srtt_us = c->req.f.upstream_srtt_us;
        r.upstream_min_rtt_us = c->req.f.upstream_min_rtt_us;
        r.attempts = c->req.f.failover_attempts;
        r.streamed = streamed;
        r.error_reply = !streamed && (c->close_after_resp || c->req.f.reply_kept);
        r.truncated = streamed && c->close_after_resp;
        r.translated = c->req.f.translate_body;
        r.backend = _active_backend;
        r.model = c->req.f.model.view();
        r.asked_tier = c->req.f.asked_tier.view();
        r.upstream_error = c->req.f.upstream_error.view();
        r.venue_req_id = c->req.f.venue_req_id.view();
        r.prefix_hash = c->req.f.prefix_hash;
        r.served_tier = c->req.f.served_tier.view();
        r.served_model = c->req.f.served_model.view();
        r.from_pool = c->req.f.upstream_pooled;
        // A stream's counts from its translator or its own usage events; -1 is "not
        // reported" and not "zero".
        u = streamed ? stream_tokens(c) : c->req.f.tok;
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
        r.image_in_tokens = static_cast<int32_t>(u.image_in);
        r.web_search_requests = static_cast<int32_t>(u.web_search);
        if (c->req.f.search_queries >= 0) r.web_search_requests = c->req.f.search_queries; // Gemini's grounding
        r.tool_prompt_input = c->req.f.tool_prompt_input;
        r.stream_error = c->req.f.stream_error;
        r.comment_unsafe = c->req.f.comment_unsafe;
        r.web_fetch_requests = static_cast<int32_t>(u.web_fetch);
        r.code_exec_requests = static_cast<int32_t>(u.code_exec);
        r.file_search_calls = static_cast<int32_t>(u.file_search);
        r.venue_cost_pdollars = u.venue_cost_pdollars;
        r.compaction_in_tokens = static_cast<int32_t>(u.compaction_in);
        r.compaction_out_tokens = static_cast<int32_t>(u.compaction_out);
        r.compaction_cached_tokens = static_cast<int32_t>(u.compaction_cached);
        r.prior_in_tokens = static_cast<int32_t>(u.prior_in);
        r.prior_out_tokens = static_cast<int32_t>(u.prior_out);
        r.prior_cached_tokens = static_cast<int32_t>(u.prior_cached);
        r.prior_cache_write_tokens = static_cast<int32_t>(u.prior_cache_write);
        r.prior_attempts = static_cast<int32_t>(u.prior_attempts);
        // An attempt still held was judged nowhere: unverified, as one no block named.
        if (u.prior_attempts >= 0)
        {
            r.prior_billed_attempts = static_cast<int32_t>(u.prior_billed > 0 ? u.prior_billed : 0);
            r.prior_unverified_attempts =
                static_cast<int32_t>((u.prior_unverified > 0 ? u.prior_unverified : 0) + u.prior_held_n);
        }
        r.prior_model = u.prior_model.view(); // into the caller's `u`, as are the next two
        r.served_speed = u.speed.view();
        r.served_geo = u.inference_geo.view();
        r.served_latency = c->req.f.served_latency.view();
        r.asked_cache_ttl = c->req.f.asked_cache_ttl.view();
        for (size_t i = 0; i < kSinkCaptureMax; ++i)
            r.captured[i] = c->req.f.captured[i].view();
        r.ts_body_end = c->req.f.ts_body_end;
        r.ts_body_sent = c->req.f.ts_body_sent;
        r.body_work_ns = c->req.f.body_work_ns;
        r.body_streamed = c->req.f.body_streamed;
        r.decided_on_head = c->req.f.decided_on_head;
        r.confirm_outcome = c->req.f.confirm_outcome;
        r.model_substituted = c->req.f.model_substituted;
        r.service_tier_spliced = (c->req.f.members_spliced & provider::BodyScan::kServiceTier) != 0;
        r.speed_spliced = (c->req.f.members_spliced & provider::BodyScan::kSpeed) != 0;
        r.inference_geo_spliced = (c->req.f.members_spliced & provider::BodyScan::kInferenceGeo) != 0;
        r.hedged = c->req.f.hedged;
        r.hedge_group = c->req.f.hedge_group;
        r.hedge_leg = c->req.f.hedge_leg;
        r.hedge_won = c->req.f.hedge_won;
        r.hedge_cut = c->req.f.hedge_cut;
        return r;
    }

    void Engine::sink_emit(ClientConn* c, int status, bool streamed) noexcept
    {
        BodyUsage u;
        const RequestRecord r = record_of(c, status, streamed, u);
        c->req.f.sunk = true;
        _sink->on_request(r);
    }

    // Whether a venue's last four bytes, oldest first, end in a blank line: two line ends in a
    // row, each LF, CRLF or CR, as the SSE specification allows them, mixed included.
    static bool ends_blank_line(const char (&t)[4]) noexcept
    {
        if (t[3] == '\n' && t[2] == '\r') return t[1] == '\n' || t[1] == '\r';
        if (t[3] == '\n') return t[2] == '\n';
        return t[3] == '\r' && (t[2] == '\r' || t[2] == '\n');
    }

    // Why `note` cannot follow `c`'s stream as one SSE comment line that dispatches nothing,
    // or null when it can. A line break or a NUL would let the rest be parsed as an event,
    // and a venue's bytes that stop inside an event would take the note into it.
    static const char* note_refusal(const ClientConn* c, std::string_view note) noexcept
    {
        if (note.size() > kReplyNoteMax) return "longer than kReplyNoteMax";
        for (const char ch : note)
            if (ch == '\r' || ch == '\n' || ch == '\0') return "holding a CR, LF or NUL";
        if (!c->req.f.sse_translating && !ends_blank_line(c->req.f.stream_tail))
            return "the venue's stream ended inside an event";
        return nullptr;
    }

    void Engine::sink_note(const ClientConn* c, std::string& out) noexcept
    {
        BodyUsage u;
        const std::string_view note = _sink->reply_note(record_of(c, 200, /*streamed=*/true, u));
        if (note.empty()) return;
        // Dropped whole. The reason is logged, never the text: a note that failed is no safer in a log.
        if (const char* why = note_refusal(c, note))
        {
            LB_WARN(ReqId{c->req.f.req_seq}, " sink reply_note dropped: ", why, " (", note.size(), " bytes)");
            return;
        }
        const size_t at = out.size();
        out.append(": ", 2).append(note).append("\n\n", 2);
        chunk_wrap(c, out, at);
    }

} // namespace llmbridge::detail
