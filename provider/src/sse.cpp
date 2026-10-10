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
#include "openai_common.hpp" // detail::now_secs / anthropic_finish_reason
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
    } // namespace

    void SseFrameReader::reset() noexcept
    {
        constexpr size_t kKeep = 64 * 1024; // capacity one large event may leave behind
        _in = {};
        _at = 0;
        _line.clear();
        _data.clear();
        if (_line.capacity() > kKeep) _line.shrink_to_fit();
        if (_data.capacity() > kKeep) _data.shrink_to_fit();
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

    void SseFrameReader::feed(std::string_view bytes) noexcept
    {
        _in = bytes;
        _at = 0;
        _cr = !bytes.empty() && std::memchr(bytes.data(), '\r', bytes.size());
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

    // Each byte is searched once however the stream is split; an unfinished line is
    // kept in _line for the next feed.
    SseFrameReader::Step SseFrameReader::next(std::string_view& data)
    {
        if (_failed) return Step::Fail;
        if (_stopped) return Step::More;
        const char* const b = _in.data();
        while (true)
        {
            if (_line_owned) { _line.clear(); _line_owned = false; }
            if (_skip_lf && _at < _in.size())
            {
                if (b[_at] == '\n') ++_at;
                _skip_lf = false;
            }
            if (_at >= _in.size() || _skip_lf) break;
            const void* lf = std::memchr(b + _at, '\n', _in.size() - _at);
            size_t k = lf ? static_cast<size_t>(static_cast<const char*>(lf) - b) : _in.size();
            if (_cr)
                if (const void* cr = std::memchr(b + _at, '\r', k - _at))
                    k = static_cast<size_t>(static_cast<const char*>(cr) - b);
            std::string_view line = _in.substr(_at, k - _at);
            if (k == _in.size())
            {
                _line.append(line);
                _at = k;
                break;
            }
            _at = k + 1;
            if (b[k] == '\r')
            {
                if (_at < _in.size()) { if (b[_at] == '\n') ++_at; }
                else _skip_lf = true;
            }
            if (!_line.empty())
            {
                _line.append(line);
                _line_owned = true;
                line = _line;
            }
            if (line.empty())
            {
                if (!_have) continue;
                _have = false;
                data = _viewing ? _view : std::string_view(_data);
                return Step::Event;
            }
            // Only `data` is read: event, id, retry and `:` comments are skipped.
            if (line.size() < 4 || line.compare(0, 4, "data") != 0 ||
                (line.size() > 4 && line[4] != ':'))
                continue;
            std::string_view value = line.substr(line.size() > 4 ? 5 : 4);
            if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            if (!add_data(value, !_line_owned)) return fail();
        }
        if (_line.size() > kMaxLine) return fail();
        if (_viewing) { _data.assign(_view); _viewing = false; } // _in goes away
        return Step::More;
    }

    void AnthropicToOpenAiSse::reset(long long created_secs, bool include_usage) noexcept
    {
        _frames.reset();
        _failed = false;
        _block_tool_ord.clear();
        _next_tool_ord = 0;
        _tool_open = false;
        _id.assign("chatcmpl-llmbridge");
        _model.clear();
        _created_len = 0;
        _created_secs = created_secs;
        _finish = nullptr;
        _role_emitted = _content_started = _finish_emitted = _done = false;
        _include_usage = include_usage;
        _usage_emitted = false;
        _in_tok = _out_tok = _cached_tok = _cache_write_tok = 0;
        _cw_5m = _cw_1h = -1;
    }

    openai::Usage AnthropicToOpenAiSse::usage() const noexcept
    {
        openai::Usage u;
        u.in = _in_tok;
        u.out = _out_tok;
        u.cached = _cached_tok;
        u.cache_write = _cache_write_tok;
        u.cache_write_5m = _cw_5m;
        u.cache_write_1h = _cw_1h;
        return u;
    }

    // Stamp `created` exactly once: a fixed value if one was supplied, else the
    // wall clock. Constant across every chunk of the stream thereafter.
    void AnthropicToOpenAiSse::ensure_created()
    {
        if (_created_len == 0)
            _created_len = openai::decimal(_created, _created_secs >= 0 ? _created_secs
                                                                         : detail::now_secs())
                               .size();
    }

    // A chunk through the open of its delta object; the caller appends the delta's
    // members and calls emit_tail().
    void AnthropicToOpenAiSse::emit_head(std::string& out)
    {
        ensure_created();
        out += "data: ";
        openai::Envelope(out, openai::Shape::Chunk, _id, {_created, _created_len}, _model).choice();
    }

    // With include_usage, OpenAI puts a null `usage` on every normal chunk; the real
    // numbers ride the dedicated final chunk (emit_usage).
    void AnthropicToOpenAiSse::emit_tail(std::string& out, const char* finish)
    {
        openai::Envelope(out, openai::Shape::Chunk)
            .end_choice(finish ? finish : "")
            .close(nullptr, _include_usage);
        out += "\n\n";
    }

    // The usage-only chunk OpenAI streams just before [DONE] when the client set
    // stream_options.include_usage: `choices` is empty by spec, and the counts are
    // Anthropic's own, re-shaped, never estimated.
    void AnthropicToOpenAiSse::emit_usage(std::string& out)
    {
        if (!_include_usage || _usage_emitted) return;
        ensure_created();
        const openai::Usage u = usage();
        out += "data: ";
        openai::Envelope(out, openai::Shape::Chunk, _id, {_created, _created_len}, _model).close(&u);
        out += "\n\n";
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

    // Strict index parse: anything not a clean, fully consumed, in-range integer is -1
    // and the event is ignored. A lenient parse read garbage as 0, which attached a
    // malformed index's argument fragments to whichever call lived at block 0.
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

        if (type == "error") // overloaded, or any failure after the head was sent
        {
            _failed = true;
            return;
        }
        if (type == "message_start")
        {
            if (const json::Value* m = v.find("message"))
            {
                // Both are echoed into every chunk, so an upstream's long one would
                // multiply each tiny delta into a large write.
                const std::string_view id = m->str_or("id"), model = m->str_or("model");
                if (id.size() > kMaxEcho || model.size() > kMaxEcho)
                {
                    _failed = true;
                    return;
                }
                if (!id.empty())
                {
                    _id.clear();
                    append_sanitized(_id, id);
                }
                _model.clear();
                append_sanitized(_model, model);
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
                if (const long long ot = json_scan::count(u->num_or("output_tokens")); ot >= 0)
                    _out_tok = ot;
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
            if (_failed) return false;
            if (_done) { _frames.stop(); return true; }
        }
        if (step == SseFrameReader::Step::Fail) _failed = true;
        return !_failed;
    }

    // Upstream EOF. Anthropic states the end in-band, so EOF before message_delta's
    // stop_reason is a cut stream however the transport closed (FIN, RST, no
    // close_notify); a fabricated finish and [DONE] would call a partial answer whole.
    bool AnthropicToOpenAiSse::finish(std::string& out)
    {
        if (_failed || !(_done || _finish)) return false;
        if (_done) return true;
        if (!_finish_emitted)
        {
            emit_head(out);
            emit_tail(out, _finish);
            _finish_emitted = true;
        }
        emit_done(out);
        return true;
    }
} // namespace llmbridge::provider
