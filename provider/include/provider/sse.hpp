// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Incremental SSE translation — the streaming analog of translate.hpp. Where
// translate.hpp is whole-body in / whole-body out, this is a *stateful pump*:
// raw upstream Server-Sent-Events bytes in, OpenAI-shaped SSE chunks out, one
// arbitrarily-fragmented network read at a time.
//
// Why stateful: a single TCP read can split an SSE event mid-line or mid-JSON,
// and OpenAI's chunk protocol carries cross-event context (a stable id/model, a
// once-emitted role delta, the final finish_reason). So you hold ONE translator
// per in-flight response and feed() it as bytes arrive, then finish() at EOF.
//
// Scope (Phase B, slice 1): Anthropic Messages stream -> OpenAI chat.completion
// .chunk stream, TEXT ONLY — message_start, content_block_delta/text_delta,
// message_delta (stop_reason), message_stop. Tool-call deltas, vision, and the
// reverse direction are later slices. I/O-agnostic: operates on in-memory
// buffers; the gateway owns the sockets and back-pressure.

#include <string>
#include <string_view>

namespace llmbridge::provider
{
    class AnthropicToOpenAiSse
    {
    public:
        // Feed raw upstream SSE bytes; append translated OpenAI SSE to `out`. An
        // incomplete trailing event is buffered internally until the next call.
        // Returns false only on a hard internal error — odd-but-skippable upstream
        // data (unknown event types, unparseable data payloads) is ignored, never
        // fatal, so one malformed frame can't tear down a live stream.
        bool feed(std::string_view bytes, std::string& out);

        // Signal upstream EOF. If the stream didn't already end with message_stop,
        // emit a terminal finish chunk followed by "data: [DONE]".
        bool finish(std::string& out);

    private:
        void dispatch(std::string_view data, std::string& out);
        void emit_head(std::string& out);                    // up to `"delta":{`
        void emit_tail(std::string& out, const char* finish); // from `}` on; null => finish_reason:null

        std::string _pending;   // bytes not yet forming a complete event (frag buffer)
        std::string _cur_data;  // concatenated `data:` lines of the in-progress event
        bool _have_data = false;

        // Cross-chunk context (copied out of the frag buffer, which churns).
        std::string _id = "chatcmpl-llmbridge"; // overwritten by message_start's id
        std::string _model;                     // from message_start
        std::string _created;                   // epoch seconds, set once
        const char* _finish = nullptr;          // mapped stop_reason (static literal)
        bool _role_emitted = false;
        bool _finish_emitted = false;
        bool _done = false;
    };
} // namespace llmbridge::provider
