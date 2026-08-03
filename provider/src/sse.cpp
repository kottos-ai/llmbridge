// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "provider/sse.hpp"

#include "openai_common.hpp" // detail::created_now / anthropic_finish_reason
#include "provider/json.hpp"

// NOTE (extract at second user): feed() below is two concerns bolted together —
// (a) dialect-agnostic SSE *framing* (line splitting, the fragmentation buffer,
// the byte caps, the O(n) resume-scan) and (b) Anthropic-specific *event mapping*
// (dispatch -> OpenAI chunks). When the second streaming dialect lands (the
// reverse direction, or Gemini/Cohere), lift (a) into a reusable SseFrameReader
// with a per-dialect on_event() callback. Deliberately NOT abstracted yet:
// with a single consumer the seams would be guesses. See CLAUDE.md — "no
// premature abstraction".

namespace llmbridge::provider
{
    namespace
    {
        // Control-byte neutralisation lives in openai_common.hpp so the SSE
        // passthrough and the error-envelope passthrough (translate.cpp) can't
        // drift: a raw newline emitted into our SSE output would let a hostile
        // upstream inject fake events ("\n\ndata: ..."), and a raw control byte
        // anywhere makes our JSON unparseable to a strict client.
        using detail::append_sanitized;

        // Sanitize into an owned string (for spans we store across events).
        std::string sanitized(std::string_view raw)
        {
            std::string s;
            s.reserve(raw.size());
            append_sanitized(s, raw);
            return s;
        }
    } // namespace

    // Stamp `created` exactly once: a fixed value if one was supplied, else the
    // wall clock. Constant across every chunk of the stream thereafter.
    void AnthropicToOpenAiSse::ensure_created()
    {
        if (!_created.empty()) return;
        _created = _created_secs >= 0 ? std::to_string(_created_secs) : detail::created_now();
    }

    // Chunk envelope: everything up to the open of the delta object. Callers then
    // append the delta body (e.g. "content":"...") and call emit_tail().
    void AnthropicToOpenAiSse::emit_head(std::string& out)
    {
        ensure_created();
        out += "data: {\"id\":\"";
        out += _id; // raw (already JSON-safe) span from message_start, or the default
        out += "\",\"object\":\"chat.completion.chunk\",\"created\":";
        out += _created;
        out += ",\"model\":\"";
        out += _model;
        out += "\",\"choices\":[{\"index\":0,\"delta\":{";
    }

    // Close the delta object + choice. `finish` == nullptr -> finish_reason:null.
    void AnthropicToOpenAiSse::emit_tail(std::string& out, const char* finish)
    {
        out += "},\"finish_reason\":";
        if (finish) { out += '"'; out += finish; out += '"'; }
        else out += "null";
        // With include_usage, OpenAI puts a null `usage` on every normal chunk;
        // the real numbers ride the dedicated final chunk (emit_usage).
        out += _include_usage ? "}],\"usage\":null}\n\n" : "}]}\n\n";
    }

    // The extra usage-only chunk OpenAI streams just before [DONE] when the client
    // set stream_options.include_usage: `choices` is empty by spec, and the counts
    // are Anthropic's own (input from message_start, cumulative output from
    // message_delta) — re-shaped, never estimated.
    void AnthropicToOpenAiSse::emit_usage(std::string& out)
    {
        if (!_include_usage || _usage_emitted) return;
        ensure_created();
        out += "data: {\"id\":\"";
        out += _id;
        out += "\",\"object\":\"chat.completion.chunk\",\"created\":";
        out += _created;
        out += ",\"model\":\"";
        out += _model;
        out += "\",\"choices\":[],\"usage\":{\"prompt_tokens\":";
        out += std::to_string(_in_tok);
        out += ",\"completion_tokens\":";
        out += std::to_string(_out_tok);
        out += ",\"total_tokens\":";
        out += std::to_string(_in_tok + _out_tok);
        out += "}}\n\n";
        _usage_emitted = true;
    }

    // Terminate the stream: the usage chunk (when requested) always precedes the
    // sentinel, exactly as OpenAI orders them.
    void AnthropicToOpenAiSse::emit_done(std::string& out)
    {
        if (_done) return;
        emit_usage(out);
        out += "data: [DONE]\n\n";
        _done = true;
    }

    void AnthropicToOpenAiSse::dispatch(std::string_view data, std::string& out)
    {
        if (data == "[DONE]") { emit_done(out); return; }

        bool ok = false;
        json::Value v = json::parse(data, ok);
        if (!ok || !v.is_object()) return; // skip a garbled frame; keep the stream alive

        const std::string_view type = v.str_or("type");

        if (type == "message_start")
        {
            if (const json::Value* m = v.find("message"))
            {
                if (const std::string_view id = m->str_or("id"); !id.empty()) _id = sanitized(id);
                _model = sanitized(m->str_or("model"));
                if (const json::Value* u = m->find("usage"))
                {
                    _in_tok = detail::to_ll(u->num_or("input_tokens", "0"));
                    _out_tok = detail::to_ll(u->num_or("output_tokens", "0"));
                }
            }
            ensure_created();
            if (!_role_emitted) // OpenAI's first chunk carries the assistant role
            {
                emit_head(out);
                out += "\"role\":\"assistant\"";
                emit_tail(out, nullptr);
                _role_emitted = true;
            }
        }
        else if (type == "content_block_delta")
        {
            const json::Value* d = v.find("delta");
            // A tool call streams its arguments as input_json_delta fragments. We
            // cannot yet reassemble them into OpenAI tool_calls deltas, and the one
            // thing we must NOT do is drop them and still report
            // finish_reason:"tool_calls" — that hands an agent loop a tool call it
            // was never given, which is worse than an error because it looks valid.
            // Fail the stream instead; see _tool_unsupported.
            if (d && d->str_or("type") == "input_json_delta") { _tool_unsupported = true; return; }
            if (d && d->str_or("type") == "text_delta")
            {
                if (!_role_emitted) // defensive: no message_start seen yet
                {
                    emit_head(out);
                    out += "\"role\":\"assistant\"";
                    emit_tail(out, nullptr);
                    _role_emitted = true;
                }
                emit_head(out);
                out += "\"content\":\"";
                append_sanitized(out, d->str_or("text")); // raw escaped span; control bytes neutralized
                out += '"';
                emit_tail(out, nullptr);
            }
        }
        else if (type == "message_delta")
        {
            // Only a stop_reason ends the message — Anthropic also sends
            // usage-only message_delta frames mid-stream, which must NOT emit a
            // premature finish chunk.
            if (const json::Value* d = v.find("delta"))
                if (const std::string_view sr = d->str_or("stop_reason"); !sr.empty())
                    _finish = detail::anthropic_finish_reason(sr);
            // Anthropic reports output_tokens cumulatively on message_delta.
            if (const json::Value* u = v.find("usage"))
                if (const std::string_view ot = u->num_or("output_tokens"); !ot.empty())
                    _out_tok = detail::to_ll(ot);
            if (_finish && !_finish_emitted) // finish chunk: empty delta + finish_reason
            {
                emit_head(out);
                emit_tail(out, _finish);
                _finish_emitted = true;
            }
        }
        else if (type == "message_stop")
        {
            if (!_finish_emitted)
            {
                emit_head(out);
                emit_tail(out, _finish ? _finish : "stop");
                _finish_emitted = true;
            }
            emit_done(out);
        }
        else if (type == "content_block_start")
        {
            // The block that announces a tool call. Same reasoning as
            // input_json_delta above: detected here so the stream fails even if the
            // provider sends no argument fragments (a zero-argument tool).
            if (const json::Value* b = v.find("content_block"))
                if (b->str_or("type") == "tool_use") _tool_unsupported = true;
        }
        // content_block_stop / ping / unknown: ignored for the text-only slice
        // (no OpenAI-side output).
    }

    // A tool call was seen: tear the stream down. Kept next to the cap-failure
    // path so both unfaithful-stream cases behave identically.
    bool AnthropicToOpenAiSse::fail_tool_unsupported() noexcept
    {
        _failed = true;
        _pending.clear(); _pending.shrink_to_fit();
        _cur_data.clear(); _cur_data.shrink_to_fit();
        return false;
    }

    bool AnthropicToOpenAiSse::feed(std::string_view bytes, std::string& out)
    {
        if (_failed) return false; // sticky: a capped stream stays dead

        // The tail retained from the previous call is known to contain no '\n',
        // so resume the newline search at the join point rather than rescanning
        // it. Without this, feeding one long line one byte at a time is O(n^2) —
        // a hostile upstream dribbling bytes could pin a core (CPU-DoS the byte
        // caps don't cover). With it, total scanning is O(bytes) regardless of
        // how the stream is fragmented.
        size_t from = _pending.size();
        _pending.append(bytes);

        size_t pos = 0;
        while (true)
        {
            const size_t nl = _pending.find('\n', from);
            if (nl == std::string::npos) break; // no complete line yet; keep the remainder

            std::string_view line(_pending.data() + pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1); // tolerate CRLF
            pos = nl + 1;
            from = pos;

            if (line.empty()) // blank line terminates an event
            {
                if (_have_data) dispatch(_cur_data, out);
                // dispatch() may have found a tool block; a stream we cannot render
                // faithfully must abort rather than finish with a misleading
                // finish_reason.
                if (_tool_unsupported) return fail_tool_unsupported();
                _cur_data.clear();
                _have_data = false;
            }
            else if (line.rfind("data:", 0) == 0) // a data field
            {
                std::string_view d = line.substr(5);
                if (!d.empty() && d.front() == ' ') d.remove_prefix(1); // one optional space
                if (_cur_data.size() + d.size() + 1 > kMaxEvent) // endless event -> refuse
                {
                    _failed = true;
                    _pending.clear(); _pending.shrink_to_fit();
                    _cur_data.clear(); _cur_data.shrink_to_fit();
                    return false;
                }
                if (_have_data) _cur_data.push_back('\n'); // SSE: multi-line data joined by \n
                _cur_data.append(d);
                _have_data = true;
            }
            // "event:" lines and ":" comments are ignored — we dispatch on the
            // data payload's own "type" field, which is authoritative for Anthropic.
        }
        _pending.erase(0, pos);

        if (_pending.size() > kMaxPending) // a single line this long is an attack, not a workload
        {
            _failed = true;
            _pending.clear(); _pending.shrink_to_fit();
            _cur_data.clear(); _cur_data.shrink_to_fit();
            return false;
        }
        return true;
    }

    bool AnthropicToOpenAiSse::finish(std::string& out)
    {
        if (_failed) return false; // don't fabricate a clean [DONE] on a capped stream
        if (_done) return true;
        if (!_finish_emitted)
        {
            emit_head(out);
            emit_tail(out, _finish ? _finish : "stop");
            _finish_emitted = true;
        }
        emit_done(out);
        return true;
    }
} // namespace llmbridge::provider
