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
        long long in = -1, out = -1, cached = -1, cache_write = -1;
        long long cache_write_5m = -1, cache_write_1h = -1; // the write by entry lifetime
        long long reasoning = -1, audio_in = -1, audio_out = -1;
        long long accepted_prediction = -1, rejected_prediction = -1, tool_prompt = -1;
    };

    enum class Shape : uint8_t { Completion, Chunk };

    /// The one writer of an OpenAI response object, appending to `out`. `id` and `model`
    /// are JSON string contents, already escaped.
    class Envelope
    {
    public:
        /// Writes through `"choices":[`.
        Envelope(std::string& out, Shape s, std::string_view id, long long created,
                 std::string_view model);
        /// Resumes an object another call opened.
        Envelope(std::string& out, Shape s) noexcept : _out(out), _shape(s) {}
        /// Opens choices[0] through its `message` or `delta` object.
        Envelope& choice();
        /// Closes the open choice; an empty `finish` writes null.
        Envelope& end_choice(std::string_view finish);
        /// Ends the object. `usage` null writes none, or `"usage":null` when `usage_null`;
        /// a `total` below 0 is in + out.
        void close(const Usage* usage, bool usage_null = false, long long total = -1);

    private:
        std::string& _out;
        Shape _shape;
    };

    /// `{"error":{"message":..,"type":..,"code":null}}`, control bytes escaped.
    void write_error(std::string& out, std::string_view message, std::string_view type);

    /// Bytes at the end of a body worth searching: a usage block sits within ~600.
    inline constexpr size_t kUsageWindow = 2048;

    /// The last usage object in the final `window` bytes (0: all of `body`), read in
    /// the OpenAI, Anthropic or Gemini shape. Allocates nothing.
    Usage scan_usage(std::string_view body, size_t window = kUsageWindow) noexcept;

    /// The string value of key `quoted` (quotes included) in `bytes`, at its first or
    /// last occurrence as a key; empty when absent, not a plain string, or cut off.
    std::string_view scan_string(std::string_view bytes, std::string_view quoted,
                                 bool last) noexcept;

    /// The `code`, else the `type`, an error object names within the first 512 bytes.
    std::string_view scan_error_type(std::string_view body) noexcept;

    /// A byte-forwarded stream's usage, fed each read once. Prompt counts keep their
    /// first statement, output counts their last, as an Anthropic stream restates them.
    class StreamUsage
    {
    public:
        void feed(std::string_view bytes);
        void reset() noexcept { _carry.clear(); _seen = 0; _u = Usage{}; }
        [[nodiscard]] const Usage& usage() const noexcept { return _u; }

    private:
        void merge(const Usage& u) noexcept;
        std::string _carry; // an unfinished usage object, or the bytes a split key may need
        size_t _seen = 0;   // where in _carry the next search starts
        Usage _u;
    };
} // namespace llmbridge::provider::openai
