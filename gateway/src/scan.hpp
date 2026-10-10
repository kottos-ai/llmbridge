// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Substring search over provider bytes, for the stream markers. The JSON-aware scans
// (usage, error type, one string) are provider::openai's, key-anchored.

#include <cstddef>
#include <cstring>
#include <string_view>

#include "provider/openai.hpp"

namespace llmbridge::detail
{
    using BodyUsage = provider::openai::Usage;
    using provider::openai::scan_error_type;
    using provider::openai::scan_string;
    using provider::openai::scan_usage;

    [[nodiscard]] inline size_t find_fast(std::string_view hay, std::string_view needle,
                                          size_t from = 0) noexcept
    {
        if (from > hay.size()) return std::string_view::npos;
        if (needle.empty()) return from;
        if (needle.size() > hay.size() - from) return std::string_view::npos;
        const void* p = ::memmem(hay.data() + from, hay.size() - from, needle.data(), needle.size());
        return p ? static_cast<size_t>(static_cast<const char*>(p) - hay.data())
                 : std::string_view::npos;
    }
} // namespace llmbridge::detail
