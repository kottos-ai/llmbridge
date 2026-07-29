// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "provider/sse.hpp"

#include <ctime>

#include "provider/json.hpp"

namespace llmbridge::provider
{
    namespace
    {
        // Anthropic stop_reason -> OpenAI finish_reason. Returns a static literal.
        const char* map_stop_reason(std::string_view sr)
        {
            if (sr == "max_tokens") return "length";
            if (sr == "tool_use") return "tool_calls";
            // end_turn, stop_sequence, and anything else -> "stop"
            return "stop";
        }

        // Append a raw (already JSON-escaped) span, neutralizing control bytes.
        // Our lenient JSON parser accepts a literal control char (e.g. 0x0A)
        // inside a string, which strict JSON forbids — and a raw newline emitted
        // into our SSE output would let a hostile upstream inject fake events
        // into the client stream ("\n\ndata: ..."). Escaping <0x20 as \u00XX
        // closes that hole; everything else is a bulk copy, same pattern as
        // json::append_escaped.
        void append_sanitized(std::string& out, std::string_view raw)
        {
            static const char* hex = "0123456789abcdef";
            size_t start = 0;
            for (size_t i = 0; i < raw.size(); ++i)
            {
                const unsigned char c = static_cast<unsigned char>(raw[i]);
                if (c >= 0x20) continue;                 // plain byte; keep scanning
                out.append(raw.data() + start, i - start); // flush the plain run
                out += "\\u00";
                out += hex[(c >> 4) & 0xF];
                out += hex[c & 0xF];
                start = i + 1;
            }
            out.append(raw.data() + start, raw.size() - start);
        }

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
        const long long secs = _created_secs >= 0 ? _created_secs
                                                   : static_cast<long long>(std::time(nullptr));
        _created = std::to_string(secs);
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
        out += "}]}\n\n";
    }

    void AnthropicToOpenAiSse::dispatch(std::string_view data, std::string& out)
    {
        if (data == "[DONE]") { if (!_done) { out += "data: [DONE]\n\n"; _done = true; } return; }

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
                    _finish = map_stop_reason(sr);
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
            if (!_done) { out += "data: [DONE]\n\n"; _done = true; }
        }
        // content_block_start / content_block_stop / ping / unknown: ignored for
        // the text-only slice (no OpenAI-side output).
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
        out += "data: [DONE]\n\n";
        _done = true;
        return true;
    }
} // namespace llmbridge::provider
