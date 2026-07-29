// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Internal helpers shared by the two translators when producing OpenAI-shaped
// output: the whole-body path (translate.cpp) and the streaming path (sse.cpp).
// NOT public API — lives in src/, not include/. Kept in one place so the two
// paths can't drift: a new Anthropic stop_reason (or a change to how `created`
// is stamped) must land identically for streaming and non-streaming.

#include <ctime>
#include <string>
#include <string_view>

namespace llmbridge::provider::detail
{
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
