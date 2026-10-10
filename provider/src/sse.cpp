// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "provider/sse.hpp"

#include <charconv>
#include <cstring>

#include "json_scan.hpp"
#include "openai_common.hpp" // detail::created_now / anthropic_finish_reason
#include "provider/json.hpp"

namespace llmbridge::provider
{
    namespace
    {
        // Control-byte neutralisation lives in openai_common.hpp so the SSE
        // passthrough and the error-envelope passthrough (translate_body.cpp) can't
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

    void SseFrameReader::reset() noexcept
    {
        _in = {};
        _at = 0;
        _line.clear();
        _data.clear();
        _view = {};
        _viewing = _have = _line_owned = _skip_lf = _stopped = _failed = false;
    }

    SseFrameReader::Step SseFrameReader::fail() noexcept
    {
        _failed = true;
        _line.clear(); _line.shrink_to_fit();
        _data.clear(); _data.shrink_to_fit();
        return Step::Fail;
    }

    // The next whole line, or false once the input is spent (its unfinished tail is
    // kept in _line). Each byte is searched once however the stream is split.
    bool SseFrameReader::take_line(std::string_view& line)
    {
        if (_line_owned) { _line.clear(); _line_owned = false; }
        if (_skip_lf)
        {
            if (_at >= _in.size()) return false;
            if (_in[_at] == '\n') ++_at;
            _skip_lf = false;
        }
        if (_at >= _in.size()) return false;
        const char* const b = _in.data();
        const void* lf = std::memchr(b + _at, '\n', _in.size() - _at);
        const size_t end = lf ? static_cast<size_t>(static_cast<const char*>(lf) - b) : _in.size();
        const void* cr = std::memchr(b + _at, '\r', end - _at);
        const size_t k = cr ? static_cast<size_t>(static_cast<const char*>(cr) - b) : end;
        const std::string_view piece = _in.substr(_at, k - _at);
        if (k == _in.size())
        {
            _line.append(piece);
            _at = k;
            return false;
        }
        _at = k + 1;
        if (_in[k] == '\r')
        {
            if (_at < _in.size()) { if (_in[_at] == '\n') ++_at; }
            else _skip_lf = true;
        }
        if (_line.empty()) { line = piece; return true; }
        _line.append(piece);
        _line_owned = true;
        line = _line;
        return true;
    }

    bool SseFrameReader::add_data(std::string_view value, bool in_place)
    {
        if (!_have)
        {
            _have = true;
            _viewing = in_place;
            if (in_place) _view = value;
            else _data.assign(value);
        }
        else
        {
            if (_viewing) { _data.assign(_view); _viewing = false; }
            _data.push_back('\n'); // multi-line data is joined by LF
            _data.append(value);
        }
        return (_viewing ? _view.size() : _data.size()) < kMaxEvent;
    }

    SseFrameReader::Step SseFrameReader::next(std::string_view& data)
    {
        if (_failed) return Step::Fail;
        if (_stopped) return Step::More;
        std::string_view line;
        while (take_line(line))
        {
            if (line.empty())
            {
                if (!_have) continue;
                _have = false;
                data = _viewing ? _view : std::string_view(_data);
                return Step::Event;
            }
            if (line.front() == ':') continue; // a comment
            const size_t colon = line.find(':');
            if (line.substr(0, colon) != "data") continue; // event, id, retry: unused
            std::string_view value = colon == std::string_view::npos ? std::string_view{}
                                                                     : line.substr(colon + 1);
            if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            if (!add_data(value, !_line_owned)) return fail();
        }
        if (_line.size() > kMaxLine) return fail();
        if (_viewing) { _data.assign(_view); _viewing = false; } // _in goes away
        return Step::More;
    }

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
    // message_delta): re-shaped, never estimated.
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
        // Only when the provider reported cache reads, so a request that used no cache
        // emits exactly the object it always did.
        if (_cached_tok > 0)
        {
            out += ",\"prompt_tokens_details\":{\"cached_tokens\":";
            out += std::to_string(_cached_tok);
            out += "}";
        }
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

    // Strict index parse. detail::to_ll() cannot be used here: it wraps
    // std::from_chars, which on overflow or garbage leaves its output untouched
    // so `"index": 99999999999999999999` and `"index": "abc"` both come back as 0.
    // Measured: that made a malformed index alias onto block 0 and attach its
    // argument fragments to whichever call lived there, i.e. a customer's arguments
    // routed to the wrong tool. Anything not a clean, fully-consumed, in-range
    // integer is rejected as -1 and the event is ignored.
    static long long parse_block_index(const json::Value& v)
    {
        const std::string_view s = v.num_or("index");
        if (s.empty()) return -1;
        long long out = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        if (ec != std::errc{} || ptr != s.data() + s.size()) return -1; // overflow/garbage/trailing
        return out;
    }

    // The finish reason to report when the upstream never supplied a stop_reason.
    //
    // "stop" is wrong once tool calls have been emitted: every OpenAI SDK branches on
    // finish_reason == "tool_calls" to decide whether to dispatch them, so reporting
    // "stop" makes the client treat a tool call as a plain answer and silently ignore
    // it: no error, just a tool that never runs. Reachable whenever the upstream
    // sends message_stop without a preceding message_delta.
    const char* AnthropicToOpenAiSse::default_finish() const noexcept
    {
        return _next_tool_ord > 0 ? "tool_calls" : "stop";
    }

    void AnthropicToOpenAiSse::dispatch(std::string_view data, std::string& out)
    {
        if (data == "[DONE]")
        {
            // Not an Anthropic event. Anthropic ends with message_stop. Honour it as
            // a terminator, but route it through the same path so the stream still
            // gets its finish chunk (jumping straight to emit_done left the client
            // with finish_reason:null and no way to know the message ended), and do
            // Not clear _tool_open: a foreign terminator cannot vouch that a tool
            // call's arguments are complete.
            if (!_finish_emitted && !_tool_open)
            {
                emit_head(out);
                emit_tail(out, _finish ? _finish : default_finish());
                _finish_emitted = true;
            }
            if (!_tool_open) emit_done(out);
            return;
        }

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
                    const auto n = [](const json::Value* o, std::string_view k) {
                        return o ? json_scan::count(o->num_or(k)) : -1;
                    };
                    const json::Value* cc = u->find("cache_creation");
                    const openai::Usage t = detail::anthropic_usage(
                        n(u, "input_tokens"), n(u, "output_tokens"),
                        n(u, "cache_read_input_tokens"), n(u, "cache_creation_input_tokens"), n(cc, "ephemeral_5m_input_tokens"),
                        n(cc, "ephemeral_1h_input_tokens"));
                    _in_tok = t.in;
                    _out_tok = t.out > 0 ? t.out : 0;
                    _cached_tok = t.cached;
                    _cache_write_tok = t.cache_write;
                    // An absent breakdown stays -1: not the same fact as one of zero.
                    if (cc)
                    {
                        _cw_5m = t.cache_write_5m > 0 ? t.cache_write_5m : 0;
                        _cw_1h = t.cache_write_1h > 0 ? t.cache_write_1h : 0;
                    }
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
                const int ord = tool_ordinal_for(parse_block_index(v));
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
                _content_started = true; // first real token; the role delta above does not count
            }
        }
        else if (type == "message_delta")
        {
            // Only a stop_reason ends the message. Anthropic also sends
            // usage-only message_delta frames mid-stream, which must not emit a
            // premature finish chunk.
            if (const json::Value* d = v.find("delta"))
                if (const std::string_view sr = d->str_or("stop_reason"); !sr.empty())
                {
                    _finish = detail::anthropic_finish_reason(sr);
                    _tool_open = false; // the message completed; arguments are whole
                }
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
            _tool_open = false; // the message ended; arguments are whole
            if (!_finish_emitted)
            {
                emit_head(out);
                emit_tail(out, _finish ? _finish : default_finish());
                _finish_emitted = true;
            }
            emit_done(out);
        }
        else if (type == "content_block_start")
        {
            const json::Value* b = v.find("content_block");
            if (!b || b->str_or("type") != "tool_use") return; // text block: nothing to emit
            // A call with no name cannot be dispatched by the client. The
            // non-streaming translator already drops these ("unusable without a
            // name"); streaming must agree, or the same upstream produces a usable
            // response one way and a broken one the other.
            if (b->str_or("name").empty()) return;
            // Bound the ordinal counter: a reopened index would otherwise let a
            // hostile stream increment it without limit, and signed overflow is UB.
            if (static_cast<size_t>(_next_tool_ord) >= kMaxBlocks) return;
            const long long idx = parse_block_index(v);
            if (idx < 0 || static_cast<size_t>(idx) >= kMaxBlocks) return; // refuse to grow
            if (static_cast<size_t>(idx) >= _block_tool_ord.size())
                _block_tool_ord.resize(static_cast<size_t>(idx) + 1, -1);
            const int ord = _next_tool_ord++;
            _block_tool_ord[static_cast<size_t>(idx)] = ord;
            _tool_open = true; // cleared by message_delta's stop_reason
            if (!_role_emitted) // a stream can open with a tool call and no text
            {
                emit_head(out);
                out += R"("role":"assistant")";
                emit_tail(out, nullptr);
                _role_emitted = true;
            }
            emit_tool_open(out, ord, b->str_or("id"), b->str_or("name"));
            _content_started = true; // a tool call is the first token of a tool-only reply
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

    // The first chunk of a tool call: carries id, name and an empty arguments
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
    // `arguments` are both JSON strings whose contents are JSON text, escaped the
    // same way, so the raw span forwards verbatim, with no decode/re-encode round
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
        if (_done) return true;    // nothing after the terminal event is read
        _frames.feed(bytes);
        std::string_view data;
        SseFrameReader::Step step;
        while ((step = _frames.next(data)) == SseFrameReader::Step::Event)
        {
            dispatch(data, out);
            if (_done) { _frames.stop(); return true; }
        }
        if (step == SseFrameReader::Step::Fail) _failed = true;
        return !_failed;
    }

    bool AnthropicToOpenAiSse::finish(std::string& out)
    {
        if (_failed) return false; // don't fabricate a clean [DONE] on a capped stream
        // Order matters: _done first. A stream that already emitted [DONE] is over,
        // and reporting failure for it makes the gateway count an error and close
        // abruptly on a response that completed correctly. (An earlier revision of
        // this function checked _tool_open first and did exactly that whenever the
        // upstream sent message_stop without a preceding message_delta.)
        if (_done) return true;
        // A tool call still open at EOF means its arguments were cut MID-JSON. The
        // client would concatenate them into something unparseable inside a stream
        // that looked complete: the "corrupt framing fabricated a clean ending"
        // failure 0.3.0 fixed for text, which streamed tool calls reintroduced.
        // Same signal: no [DONE], and the gateway counts it as an error.
        if (_tool_open) return false;
        if (!_finish_emitted)
        {
            emit_head(out);
            emit_tail(out, _finish ? _finish : default_finish());
            _finish_emitted = true;
        }
        emit_done(out);
        return true;
    }
} // namespace llmbridge::provider
