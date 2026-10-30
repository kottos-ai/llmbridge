// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Token usage in the OpenAI convention, the one shape every venue's counts map into,
// and the bounded scans that read it from bytes nothing else parses.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace llmbridge::provider::openai
{
    /// -1 is "not stated", never zero. `in` is the whole prompt; `cached`,
    /// `cache_write` and `tool_prompt` are parts of it. `reasoning` is part of `out`.
    struct Usage
    {
        /// A short word a venue states beside its counts, kept only when it fits whole.
        template <size_t N>
        struct Word
        {
            char s[N] = {};
            uint8_t n = 0;
            void set(std::string_view v) noexcept
            {
                n = 0;
                if (v.size() > N) return;
                for (const char c : v) s[n++] = c;
            }
            [[nodiscard]] std::string_view view() const noexcept { return {s, n}; }
        };

        long long in = -1, out = -1, cached = -1, cache_write = -1;
        long long cache_write_5m = -1, cache_write_1h = -1; // the write by entry lifetime
        long long reasoning = -1, audio_in = -1, audio_out = -1;
        long long accepted_prediction = -1, rejected_prediction = -1, tool_prompt = -1;
        long long image_in = -1; // part of `in`, OpenAI's prompt_tokens_details
        /// Server tools billed per call, not per token: Anthropic's `server_tool_use`, and
        /// the Responses API's search calls, which only its output items state.
        long long web_search = -1, web_fetch = -1, code_exec = -1, file_search = -1;
        /// The venue's own price for the request, picodollars (DeepInfra's estimated_cost).
        long long venue_cost_pdollars = -1;
        /// Anthropic's `usage.iterations`, the attempts its top-level counts leave out, in
        /// this convention. Compaction runs on the served model, always billed. Attempts
        /// declined before a fallback ran on `prior_model`, the first such attempt's, and
        /// are billed as Anthropic bills them: one that produced output always, one that
        /// declined before any output only in a billed category (refusal_billed). prior_*
        /// counts the billed ones, `prior_attempts` every one, `prior_billed` those counted,
        /// `prior_unverified` those no fallback block named a category for, left out.
        long long compaction_in = -1, compaction_out = -1, compaction_cached = -1;
        long long prior_in = -1, prior_out = -1, prior_cached = -1, prior_cache_write = -1;
        long long prior_attempts = -1, prior_billed = -1, prior_unverified = -1;
        Word<48> prior_model;
        /// Declined attempts with no output, held for bill_declined to judge by category:
        /// the `index`-th declined attempt, its prompt in this convention.
        struct HeldAttempt
        {
            long long in = 0, cached = 0, cache_write = 0;
            uint8_t index = 0;
        };
        static constexpr size_t kMaxHeld = 4;
        HeldAttempt prior_held[kMaxHeld] = {};
        uint8_t prior_held_n = 0;
        Word<16> speed, inference_geo; // Anthropic: what served, not what was asked
    };

    enum class Shape : uint8_t { Completion, Chunk };

    /// How a choice ended, in OpenAI's terms. Error is a venue's own failure: never
    /// written, the gateway answers 502 instead.
    enum class Finish : uint8_t { Stop, Length, ToolCalls, ContentFilter, Error };

    /// The `finish_reason` text for `f`; null for Error.
    constexpr const char* finish_name(Finish f) noexcept
    {
        constexpr const char* kNames[] = {"stop", "length", "tool_calls", "content_filter", nullptr};
        return kNames[static_cast<uint8_t>(f)];
    }

    /// The one writer of an OpenAI response object, appending to `out`. `id` and `model`
    /// are JSON string contents, already escaped; `created` is decimal text.
    class Envelope
    {
    public:
        /// Writes through `"choices":[`.
        Envelope(std::string& out, Shape s, std::string_view id, std::string_view created,
                 std::string_view model)
            : _out(out), _shape(s)
        {
            lit(R"({"id":")").append(id);
            if (s == Shape::Chunk) lit(R"(","object":"chat.completion.chunk","created":)");
            else lit(R"(","object":"chat.completion","created":)");
            out.append(created);
            lit(R"(,"model":")").append(model);
            lit(R"(","choices":[)");
        }
        /// Resumes an object another call opened.
        Envelope(std::string& out, Shape s) noexcept : _out(out), _shape(s) {}
        /// Opens choices[0] through its `message` or `delta` object.
        Envelope& choice()
        {
            if (_shape == Shape::Chunk) lit(R"({"index":0,"delta":{)");
            else lit(R"({"index":0,"message":{)");
            return *this;
        }
        /// Closes the open choice; an empty `finish` writes null.
        Envelope& end_choice(std::string_view finish)
        {
            if (finish.empty())
            {
                lit(R"(},"finish_reason":null})");
                return *this;
            }
            lit(R"(},"finish_reason":")").append(finish);
            lit(R"("})");
            return *this;
        }
        /// Ends the object. `usage` null writes none, or `"usage":null` when `usage_null`;
        /// a `total` below 0 is in + out.
        void close(const Usage* usage, bool usage_null = false, long long total = -1);

    private:
        template <size_t N>
        std::string& lit(const char (&s)[N])
        {
            return _out.append(s, N - 1);
        }
        std::string& _out;
        Shape _shape;
    };

    /// Decimal text of `v` in `buf`, for Envelope's `created`.
    std::string_view decimal(char (&buf)[24], long long v) noexcept;

    /// `{"error":{"message":..,"type":..,"code":null}}`, control bytes escaped.
    void write_error(std::string& out, std::string_view message, std::string_view type);

    /// Bytes at the end of a body worth searching: a usage block sits within ~600, and
    /// Anthropic's per-attempt `iterations` can triple it.
    inline constexpr size_t kUsageWindow = 4096;

    /// The last usage object in the final `window` bytes (0: all of `body`), read in
    /// the OpenAI, Anthropic or Gemini shape. Allocates nothing.
    Usage scan_usage(std::string_view body, size_t window = kUsageWindow) noexcept;

    /// The string value of key `quoted` (quotes included) in `bytes`, at its first or
    /// last occurrence as a key; empty when absent, not a plain string, or cut off.
    std::string_view scan_string(std::string_view bytes, std::string_view quoted,
                                 bool last) noexcept;

    /// The `code`, else the `type`, an error object names within the first 512 bytes;
    /// else Google's `status` (RESOURCE_EXHAUSTED) within the first 8 KiB.
    std::string_view scan_error_type(std::string_view body) noexcept;

    /// The members of one `usage` object (its value, braces included), any venue's
    /// names but Gemini's.
    Usage usage_of(std::string_view object) noexcept;

    /// Folds a later statement of a stream's usage into `into`: prompt counts keep the
    /// largest, as Anthropic restates them cumulatively and omits some; the cache split,
    /// speed and region their first, which only message_start carries; the rest their
    /// last.
    void merge_stated(Usage& into, const Usage& later) noexcept;

    /// The Responses API's search calls in `bytes`, added to `u`: a whole body's
    /// `web_search_call` and `file_search_call` items, or with `events` a stream's
    /// `.completed` events, which name each call once where its items name it thrice.
    void count_search_calls(std::string_view bytes, bool events, Usage& u) noexcept;

    /// The refusal categories of a reply's fallback blocks, in order: the k-th names why the
    /// k-th declined attempt in `usage.iterations` declined.
    struct RefusalTriggers
    {
        static constexpr size_t kMax = 8;
        uint8_t n = 0;
        bool billed[kMax] = {}; ///< refusal_billed of its category
    };

    /// Whether Anthropic bills a refusal before any output in `category` (its
    /// stop_details.category, raw; empty for null): `bio`, `frontier_llm` and
    /// `reasoning_extraction`, as of September 2026 ("How refusals are billed").
    [[nodiscard]] bool refusal_billed(std::string_view category) noexcept;

    /// Each fallback block's `"trigger":{"type":"refusal","category":...}` in `bytes`, in
    /// order, appended to `t`; one cut off at the end of `bytes` is not read.
    void scan_refusal_triggers(std::string_view bytes, RefusalTriggers& t) noexcept;

    /// Folds the held no-output declined attempts into `u` by `t`: a billed category adds
    /// it to prior_* and prior_billed; one `t` names no category for is prior_unverified,
    /// left out. Idempotent: the held attempts are judged once.
    void bill_declined(Usage& u, const RefusalTriggers& t) noexcept;

    /// A byte-forwarded stream's usage, fed each read once, merged by merge_stated.
    class StreamUsage
    {
    public:
        void feed(std::string_view bytes);
        /// `search_calls`: also count the Responses API's search-call events.
        void reset(bool search_calls = false) noexcept
        {
            _carry.clear();
            _seen = 0;
            _u = Usage{};
            _calls = search_calls;
            _tail_n = 0;
            _triggers = {};
            _trig_tail_n = 0;
        }
        /// The counts stated so far, declined attempts billed by the fallback blocks read.
        [[nodiscard]] Usage usage() const noexcept
        {
            Usage u = _u;
            bill_declined(u, _triggers);
            return u;
        }

    private:
        void count_calls(std::string_view bytes) noexcept;
        static constexpr size_t kTail = 40; // past the longest event name counted
        std::string _carry; // an unfinished usage object, or the bytes a split key may need
        size_t _seen = 0;   // where in _carry the next search starts
        Usage _u;
        bool _calls = false;
        char _tail[kTail] = {}; // the last bytes of the previous read, for a split name
        size_t _tail_n = 0;
        void note_triggers(std::string_view bytes) noexcept;
        static constexpr size_t kTrigTail = 256; // past the longest trigger object
        RefusalTriggers _triggers;
        char _trig_tail[kTrigTail] = {}; // the previous reads' last bytes, for a split trigger
        size_t _trig_tail_n = 0;
    };
} // namespace llmbridge::provider::openai
