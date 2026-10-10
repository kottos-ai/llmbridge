// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Internal helpers shared by the two translators when producing OpenAI-shaped
// output: the whole-body path (translate_*.cpp) and the streaming path (sse.cpp).
// Not public API: it lives in src/, not include/. Kept in one place so the two
// paths can't drift: a new Anthropic stop_reason (or a change to how `created`
// is stamped) must land identically for streaming and non-streaming.

#include <ctime>
#include <string>
#include <string_view>

#include "provider/openai.hpp"

namespace llmbridge::provider::detail
{
    // Append a raw (already JSON-escaped) span, neutralising C0 control bytes as
    // \u00XX. Our JSON parser is lenient and accepts a literal control char inside
    // a string, so a passthrough span from an untrusted upstream can carry bytes
    // that are illegal in strict JSON (RFC 8259 §7); emitting them would make our
    // own output unparseable to a strict client. Bulk-copies the clean runs, so
    // the common (control-free) case is a memcpy.
    inline void append_sanitized(std::string& out, std::string_view raw)
    {
        static const char* hex = "0123456789abcdef";
        size_t start = 0;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(raw[i]);
            if (c >= 0x20) continue;                   // ordinary byte; keep scanning
            out.append(raw.data() + start, i - start); // flush the clean run
            out += "\\u00";
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
            start = i + 1;
        }
        out.append(raw.data() + start, raw.size() - start);
    }

    // Epoch seconds for the OpenAI `created` field. A bare 0 confuses some SDK
    // clients; "now" is exactly OpenAI's semantics. (time() is a vDSO read on Linux.)
    inline long long now_secs() noexcept { return static_cast<long long>(std::time(nullptr)); }

    // Each venue's counts in the OpenAI convention, written once for the whole-body
    // translators, the stream translator and the byte-forward scans. -1: not stated.

    // Anthropic's input_tokens is the fresh part only; the cache read and write are
    // stated beside it, so the whole prompt is their sum.
    inline openai::Usage anthropic_usage(long long fresh, long long out, long long read,
                                         long long write, long long w5m, long long w1h) noexcept
    {
        openai::Usage u;
        u.cached = read > 0 ? read : 0;
        u.cache_write = write > 0 ? write : 0;
        u.in = (fresh > 0 ? fresh : 0) + u.cached + u.cache_write;
        u.out = out;
        if (w5m >= 0 || w1h >= 0)
        {
            u.cache_write_5m = w5m > 0 ? w5m : 0;
            u.cache_write_1h = w1h > 0 ? w1h : 0;
        }
        return u;
    }

    // Gemini states thinking beside candidatesTokenCount, not inside it, and bills it
    // as output, so completion_tokens is their sum and reasoning the thinking part.
    inline openai::Usage gemini_usage(long long prompt, long long candidates, long long thoughts,
                                      long long cached, long long tool_prompt) noexcept
    {
        openai::Usage u;
        u.in = prompt;
        u.out = candidates;
        if (thoughts >= 0) u.out = (candidates > 0 ? candidates : 0) + thoughts;
        u.cached = cached;
        u.reasoning = thoughts;
        u.tool_prompt = tool_prompt;
        return u;
    }

    // The counts as an OpenAI body states them: prompt and completion always, cached
    // only when above zero, so a reader of the body and of `u` agree.
    inline void as_written(openai::Usage& u) noexcept
    {
        if (u.in < 0) u.in = 0;
        if (u.out < 0) u.out = 0;
        if (u.cached <= 0) u.cached = -1;
    }

    // Anthropic Messages stop_reason -> OpenAI finish_reason. Returns a static
    // literal. Shared by translate_anthropic.cpp (non-streaming message_delta) and sse.cpp
    // (streaming message_delta), which must agree.
    inline const char* anthropic_finish_reason(std::string_view stop_reason)
    {
        if (stop_reason == "max_tokens") return "length";
        if (stop_reason == "tool_use") return "tool_calls";
        // end_turn, stop_sequence, and anything else -> "stop"
        return "stop";
    }
} // namespace llmbridge::provider::detail
