// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Internal helpers shared by the two translators when producing OpenAI-shaped
// output: the whole-body path (translate.cpp) and the streaming path (sse.cpp).
// Not public API: it lives in src/, not include/. Kept in one place so the two
// paths can't drift: a new Anthropic stop_reason (or a change to how `created`
// is stamped) must land identically for streaming and non-streaming.

#include <charconv>
#include <ctime>
#include <string>
#include <string_view>

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

    // Parse a JSON number's raw text. Returns 0 for empty/garbage; usage counts
    // are advisory, so a malformed one must not fail a translation.
    inline long long to_ll(std::string_view s)
    {
        long long v = 0;
        std::from_chars(s.data(), s.data() + s.size(), v);
        return v;
    }

    // Current epoch seconds as text for the OpenAI `created` field. A bare 0
    // confuses some SDK clients; we synthesize "now", which is exactly OpenAI's
    // semantics. (time() is a fast vDSO read on Linux.)
    inline std::string created_now()
    {
        return std::to_string(static_cast<long long>(std::time(nullptr)));
    }

    // Anthropic Messages stop_reason -> OpenAI finish_reason. Returns a static
    // literal. Shared by translate.cpp (non-streaming message_delta) and sse.cpp
    // (streaming message_delta), which must agree.
    inline const char* anthropic_finish_reason(std::string_view stop_reason)
    {
        if (stop_reason == "max_tokens") return "length";
        if (stop_reason == "tool_use") return "tool_calls";
        // end_turn, stop_sequence, and anything else -> "stop"
        return "stop";
    }
} // namespace llmbridge::provider::detail
