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

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llmbridge::provider
{
    /// Server-sent events framing (WHATWG): lines end in CR, LF or CRLF, a `data` field
    /// may have no colon, and an event is dispatched at a blank line. Feed bytes, then
    /// call next() until it stops returning Event.
    class SseFrameReader
    {
    public:
        static constexpr size_t kMaxLine = 1 << 20;  // a longer unfinished line fails
        static constexpr size_t kMaxEvent = 4 << 20; // a longer event's data fails
        enum class Step : uint8_t { Event, More, Fail };

        /// `bytes` must stay valid until next() returns More or Fail.
        void feed(std::string_view bytes) noexcept { _in = bytes; _at = 0; }
        /// Event: `data` is the next event's data, valid until the next call.
        Step next(std::string_view& data);
        /// Reads nothing more: the stream's terminal event was seen.
        void stop() noexcept { _stopped = true; }
        void reset() noexcept;

    private:
        bool take_line(std::string_view& line);
        bool add_data(std::string_view value, bool in_place);
        Step fail() noexcept;

        std::string_view _in;
        size_t _at = 0;
        std::string _line;      // an unfinished line carried to the next feed
        std::string _data;      // the event's data, when it cannot stay a view
        std::string_view _view; // the event's single data line, in place in _in
        bool _viewing = false, _have = false, _line_owned = false;
        bool _skip_lf = false; // a CR ended the last feed; an LF opening the next is its pair
        bool _stopped = false, _failed = false;
    };

    class AnthropicToOpenAiSse
    {
    public:
        // Caps on untrusted input: an endless line or event is a bad peer, not a workload.
        static constexpr size_t kMaxPending = SseFrameReader::kMaxLine;
        static constexpr size_t kMaxEvent = SseFrameReader::kMaxEvent;

        // `created_secs` fixes every chunk's `created` stamp (-1: the wall clock, read once).
        // `include_usage` mirrors stream_options.include_usage: `"usage": null` on each chunk,
        // then a usage chunk with empty choices before [DONE], from Anthropic's own counts.
        explicit AnthropicToOpenAiSse(long long created_secs = -1, bool include_usage = false)
            : _created_secs(created_secs), _include_usage(include_usage)
        {
        }

        // Append the translation of these bytes to `out`; an incomplete event waits for the
        // next call, unknown or unparseable events are skipped, and nothing after the
        // terminal event is read. False, permanently, on an `error` event or a cap.
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

        SseFrameReader _frames;
        bool _failed = false;   // sticky: an error event or a cap; feed() refuses further work
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
