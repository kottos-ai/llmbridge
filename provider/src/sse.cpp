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
    } // namespace

    // Chunk envelope: everything up to the open of the delta object. Callers then
    // append the delta body (e.g. "content":"...") and call emit_tail().
    void AnthropicToOpenAiSse::emit_head(std::string& out)
    {
        if (_created.empty()) _created = std::to_string(static_cast<long long>(std::time(nullptr)));
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
                if (const std::string_view id = m->str_or("id"); !id.empty()) _id.assign(id);
                _model.assign(m->str_or("model"));
            }
            if (_created.empty()) _created = std::to_string(static_cast<long long>(std::time(nullptr)));
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
                out += d->str_or("text"); // raw escaped span -> zero-copy passthrough
                out += '"';
                emit_tail(out, nullptr);
            }
        }
        else if (type == "message_delta")
        {
            if (const json::Value* d = v.find("delta"))
                if (const std::string_view sr = d->str_or("stop_reason"); !sr.empty())
                    _finish = map_stop_reason(sr);
            if (!_finish_emitted) // finish chunk: empty delta + finish_reason
            {
                emit_head(out);
                emit_tail(out, _finish ? _finish : "stop");
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
        _pending.append(bytes);

        size_t pos = 0;
        while (true)
        {
            const size_t nl = _pending.find('\n', pos);
            if (nl == std::string::npos) break; // no complete line yet; keep the remainder

            std::string_view line(_pending.data() + pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1); // tolerate CRLF
            pos = nl + 1;

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
                if (_have_data) _cur_data.push_back('\n'); // SSE: multi-line data joined by \n
                _cur_data.append(d);
                _have_data = true;
            }
            // "event:" lines and ":" comments are ignored — we dispatch on the
            // data payload's own "type" field, which is authoritative for Anthropic.
        }
        _pending.erase(0, pos);
        return true;
    }

    bool AnthropicToOpenAiSse::finish(std::string& out)
    {
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
