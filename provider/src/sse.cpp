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
            // A tool call streams its arguments as input_json_delta fragments,
            // addressed by the block index they belong to.
            if (d && d->str_or("type") == "input_json_delta")
            {
                const int ord = tool_ordinal_for(detail::to_ll(v.num_or("index", "-1")));
                if (ord >= 0) emit_tool_args(out, ord, d->str_or("partial_json"));
                return; // never falls through to the text path
            }
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
            const json::Value* b = v.find("content_block");
            if (!b || b->str_or("type") != "tool_use") return; // text block: nothing to emit
            const long long idx = detail::to_ll(v.num_or("index", "-1"));
            if (idx < 0 || static_cast<size_t>(idx) >= kMaxBlocks) return; // refuse to grow
            if (static_cast<size_t>(idx) >= _block_tool_ord.size())
                _block_tool_ord.resize(static_cast<size_t>(idx) + 1, -1);
            const int ord = _next_tool_ord++;
            _block_tool_ord[static_cast<size_t>(idx)] = ord;
            if (!_role_emitted) // a stream can open with a tool call and no text
            {
                emit_head(out);
                out += R"("role":"assistant")";
                emit_tail(out, nullptr);
                _role_emitted = true;
            }
            emit_tool_open(out, ord, b->str_or("id"), b->str_or("name"));
        }
        // content_block_stop / ping / unknown: ignored for the text-only slice
        // (no OpenAI-side output).
    }

    // Anthropic block index -> OpenAI tool_calls ordinal. Returns -1 for a block
    // that is not a tool call, or one past the cap.
    int AnthropicToOpenAiSse::tool_ordinal_for(long long block_index)
    {
        if (block_index < 0 || static_cast<size_t>(block_index) >= kMaxBlocks) return -1;
        const size_t i = static_cast<size_t>(block_index);
        return i < _block_tool_ord.size() ? _block_tool_ord[i] : -1;
    }

    // The FIRST chunk of a tool call: carries id, name and an empty arguments
    // string. OpenAI clients key off `id` being present to start a new call, then
    // concatenate `arguments` fragments from the chunks that follow.
    void AnthropicToOpenAiSse::emit_tool_open(std::string& out, int ord, std::string_view id,
                                              std::string_view name)
    {
        emit_head(out);
        out += R"("tool_calls":[{"index":)";
        out += std::to_string(ord);
        out += R"(,"id":")";
        detail::append_sanitized(out, id); // raw span; control bytes neutralised
        out += R"(","type":"function","function":{"name":")";
        detail::append_sanitized(out, name);
        out += R"(","arguments":""}}])";
        emit_tail(out, nullptr);
    }

    // A fragment of the arguments. Anthropic's `partial_json` and OpenAI's
    // `arguments` are BOTH JSON strings whose contents are JSON text, escaped the
    // same way — so the raw span forwards verbatim, with no decode/re-encode round
    // trip that could alter a customer's argument bytes.
    void AnthropicToOpenAiSse::emit_tool_args(std::string& out, int ord, std::string_view frag)
    {
        if (frag.empty()) return; // nothing to say; don't emit an empty chunk
        emit_head(out);
        out += R"("tool_calls":[{"index":)";
        out += std::to_string(ord);
        out += R"(,"function":{"arguments":")";
        detail::append_sanitized(out, frag);
        out += R"("}}])";
        emit_tail(out, nullptr);
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
        emit_done(out);
        return true;
    }
} // namespace llmbridge::provider
