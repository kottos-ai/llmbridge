// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Anthropic Messages SSE in, OpenAI chat.completion.chunk SSE out, one fragmented read at a
// time: text and tool-call deltas. Stateful, because a read can split an event and OpenAI
// chunks carry cross-event context: one translator per response, feed(), then finish().

#include <string>
#include <vector>
#include <string_view>

namespace llmbridge::provider
{
    class AnthropicToOpenAiSse
    {
    public:
        // Caps on untrusted input: an endless line or event is a bad peer, not a workload.
        static constexpr size_t kMaxPending = 1 << 20;  // 1 MiB: longest single line
        static constexpr size_t kMaxEvent = 4 << 20;    // 4 MiB: one event's data

        // `created_secs` fixes every chunk's `created` stamp (-1: the wall clock, read once).
        // `include_usage` mirrors stream_options.include_usage: `"usage": null` on each chunk,
        // then a usage chunk with empty choices before [DONE], from Anthropic's own counts.
        explicit AnthropicToOpenAiSse(long long created_secs = -1, bool include_usage = false)
            : _created_secs(created_secs), _include_usage(include_usage)
        {
        }

        // Append the translation of these bytes to `out`; an incomplete event waits for the
        // next call, and unknown or unparseable events are skipped. False, permanently,
        // only when a cap is exceeded: the caller must drop the upstream.
        bool feed(std::string_view bytes, std::string& out);

        // Upstream EOF: a terminal finish chunk and [DONE], unless message_stop already sent them.
        bool finish(std::string& out);

        /// Provider-reported token counts so far; final only once the stream ends.
        [[nodiscard]] long long input_tokens() const noexcept { return _in_tok; }
        [[nodiscard]] long long output_tokens() const noexcept { return _out_tok; }
        /// usage.cache_read_input_tokens from message_start; 0 when none was reported.
        [[nodiscard]] long long cached_tokens() const noexcept { return _cached_tok; }
        /// usage.cache_creation_input_tokens: billed above the input rate, so kept apart.
        [[nodiscard]] long long cache_write_tokens() const noexcept { return _cache_write_tok; }
        /// The cache write split by entry lifetime, from usage.cache_creation.
        [[nodiscard]] long long cache_write_5m_tokens() const noexcept { return _cw_5m; }
        [[nodiscard]] long long cache_write_1h_tokens() const noexcept { return _cw_1h; }

        /// True once a text delta or a tool call's name or arguments has been emitted.
        [[nodiscard]] bool content_started() const noexcept { return _content_started; }


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
        // Anthropic indexes every content block; OpenAI's tool_calls[].index counts only
        // calls, so block indices map to ordinals (-1: not a tool). Capped so a hostile index
        // cannot make us allocate; blocks past the cap are ignored.
        static constexpr size_t kMaxBlocks = 256;
        std::vector<int> _block_tool_ord;  // block index -> OpenAI ordinal, or -1
        int _next_tool_ord = 0;
        // A tool call is open with no stop_reason yet: at EOF its arguments are truncated.
        bool _tool_open = false;

        // Cross-chunk context (copied out of the frag buffer, which churns).
        std::string _id = "chatcmpl-llmbridge"; // overwritten by message_start's id
        std::string _model;                     // from message_start
        std::string _created;                   // epoch seconds as text, set once
        long long _created_secs = -1;           // fixed stamp, or -1 => wall clock
        const char* _finish = nullptr;          // mapped stop_reason (static literal)
        bool _role_emitted = false;
        bool _content_started = false; // first text/tool delta emitted; see content_started()
        bool _finish_emitted = false;
        bool _done = false;

        // Usage passthrough (only emitted when _include_usage).
        bool _include_usage = false;
        bool _usage_emitted = false;
        long long _in_tok = 0;  // message_start:  usage.input_tokens
        long long _out_tok = 0; // message_delta:  usage.output_tokens (cumulative)
        long long _cached_tok = 0; // message_start: usage.cache_read_input_tokens
        long long _cache_write_tok = 0; // message_start: usage.cache_creation_input_tokens
        long long _cw_5m = -1; // usage.cache_creation.ephemeral_5m_input_tokens
        long long _cw_1h = -1; // usage.cache_creation.ephemeral_1h_input_tokens
    };
} // namespace llmbridge::provider
