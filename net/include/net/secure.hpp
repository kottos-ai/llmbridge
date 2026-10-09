// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Zero secret bytes beyond the optimizer's reach. Why and how: GATEWAY-INTERNALS.md §9b.

#include <cstddef>
#include <string>

namespace llmbridge::net
{
    /// Zero `n` bytes at `p` in a way the optimizer may not remove; p may be null if n is 0.
    void secure_clear(void* p, size_t n) noexcept;

    /// Zero a string's bytes, then clear it; the capacity is kept for the next request.
    inline void secure_clear(std::string& s) noexcept
    {
        if (!s.empty()) secure_clear(s.data(), s.size());
        s.clear();
    }
} // namespace llmbridge::net
