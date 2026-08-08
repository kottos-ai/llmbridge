// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Incremental SSE translation: the streaming analog of translate.hpp. Where
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
// .chunk stream, TEXT ONLY: message_start, content_block_delta/text_delta,
// message_delta (stop_reason), message_stop. Tool-call deltas, vision, and the
// reverse direction are later slices. I/O-agnostic: operates on in-memory
// buffers; the gateway owns the sockets and back-pressure.

#include <string>
#include <vector>
#include <string_view>

namespace llmbridge::provider
{
    class AnthropicToOpenAiSse
    {
    public:
        // The upstream is untrusted network input, so both internal buffers are
        // hard-capped: an endless line (no '\n') or an endless event (no blank
        // line) is a malfunctioning-or-malicious peer, not a workload. Generous
        // vs. real Anthropic events (KBs), tight vs. a memory-exhaustion attempt.
        static constexpr size_t kMaxPending = 1 << 20;  // 1 MiB: longest single line
        static constexpr size_t kMaxEvent = 4 << 20;    // 4 MiB: one event's data

        // `created_secs` is the OpenAI `created` epoch stamp, held constant across
        // every chunk of the stream (OpenAI's own invariant). Default (-1) means
        // "stamp it once, lazily, from the wall clock": the production path. Pass
        // a fixed value to make the output fully deterministic (tests, and so the
        // gateway can align a stream's `created` with its non-streaming path).
        // `include_usage` mirrors OpenAI's `stream_options.include_usage`: when set,
        // every chunk carries `"usage": null` and ONE extra chunk, with empty `choices`
        // plus the real token counts, is emitted just before `data: [DONE]`.
        // Anthropic supplies the numbers itself (input in message_start, cumulative
        // output in message_delta), so this is pure re-shaping, never estimation.
        explicit AnthropicToOpenAiSse(long long created_secs = -1, bool include_usage = false)
            : _created_secs(created_secs), _include_usage(include_usage)
        {
        }

        // Feed raw upstream SSE bytes; append translated OpenAI SSE to `out`. An
        // incomplete trailing event is buffered internally until the next call.
        // Odd-but-skippable upstream data (unknown event types, unparseable data
        // payloads) is ignored, never fatal, so one malformed frame can't tear
        // down a live stream. Returns false permanently, only when a buffer
        // cap is exceeded: the caller must treat that as a protocol failure and
        // drop the upstream connection.
        bool feed(std::string_view bytes, std::string& out);

        // Signal upstream EOF. If the stream didn't already end with message_stop,
        // emit a terminal finish chunk followed by "data: [DONE]".
        bool finish(std::string& out);

    private:
        void dispatch(std::string_view data, std::string& out);
        void ensure_created();                               // stamp _created once
        void emit_head(std::string& out);                    // up to `"delta":{`
        void emit_tail(std::string& out, const char* finish); // from `}` on; null => finish_reason:null
        void emit_tool_open(std::string& out, int ord, std::string_view id, std::string_view name);
        void emit_tool_args(std::string& out, int ord, std::string_view frag);
        int tool_ordinal_for(long long block_index);          // Anthropic index -> OpenAI ordinal
        const char* default_finish() const noexcept;          // "tool_calls" once any call was emitted
        void emit_usage(std::string& out);                    // the final usage-only chunk
        void emit_done(std::string& out);                     // usage chunk (if any) + [DONE]

        std::string _pending;   // bytes not yet forming a complete line (frag buffer)
        std::string _cur_data;  // concatenated `data:` lines of the in-progress event
        bool _have_data = false;
        bool _failed = false;   // sticky: set on cap overflow, feed() refuses further work
        // ── Streamed tool calls ──────────────────────────────────────────────
        // Anthropic indexes EVERY content block (text blocks included); OpenAI's
        // tool_calls[].index counts only tool calls. The two diverge the moment a
        // text block precedes a call, so a mapping is required, using Anthropic's
        // index directly would emit tool_calls[1] with no tool_calls[0] and break
        // client-side reassembly.
        //
        // Sparse and tiny: the vector is indexed by Anthropic block index, holds -1
        // for "not a tool block", and is capped so a hostile index cannot make us
        // allocate. Blocks beyond the cap are ignored instead of trusted.
        static constexpr size_t kMaxBlocks = 256;
        std::vector<int> _block_tool_ord;  // block index -> OpenAI ordinal, or -1
        int _next_tool_ord = 0;
        // A tool call was opened and the message has not reported a stop_reason.
        // At EOF that means truncated arguments; see finish().
        bool _tool_open = false;

        // Cross-chunk context (copied out of the frag buffer, which churns).
        std::string _id = "chatcmpl-llmbridge"; // overwritten by message_start's id
        std::string _model;                     // from message_start
        std::string _created;                   // epoch seconds as text, set once
        long long _created_secs = -1;           // fixed stamp, or -1 => wall clock
        const char* _finish = nullptr;          // mapped stop_reason (static literal)
        bool _role_emitted = false;
        bool _finish_emitted = false;
        bool _done = false;

        // Usage passthrough (only emitted when _include_usage).
        bool _include_usage = false;
        bool _usage_emitted = false;
        long long _in_tok = 0;  // message_start:  usage.input_tokens
        long long _out_tok = 0; // message_delta:  usage.output_tokens (cumulative)
    };
} // namespace llmbridge::provider
