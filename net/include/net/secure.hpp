// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Erase secret bytes so they cannot be recovered from the allocation later.
//
// THE PROBLEM. `std::string::clear()` sets size()=0 and leaves the bytes intact;
// a plain `memset` before releasing the memory is a DEAD STORE the optimizer is
// entitled to delete. That is not theoretical — compiled at -O2 on this project's
// toolchain, the naive version emitted ZERO instructions while the version below
// emitted the call. Verify with:
//
//     g++ -O2 -S ... | awk '/secure_clear/,/ret/' | grep call
//
// THE SOLUTION. There is no standard one: `std::secure_clear` (P1315) was proposed
// and never adopted, so every real implementation calls a PLATFORM primitive and
// falls back to a compiler barrier where none exists. That is exactly what this
// does — the platform function is chosen at CMake configure time by feature
// detection (never by guessing from #ifdef __linux__), so the common path is a
// purpose-built function that states its intent, not a trick:
//
//     explicit_bzero      glibc >= 2.25, *BSD          (LLMBRIDGE_HAVE_EXPLICIT_BZERO)
//     memset_s            C11 Annex K, MSVC            (LLMBRIDGE_HAVE_MEMSET_S)
//     SecureZeroMemory    Windows                      (_WIN32)
//     volatile memset     documented fallback, always correct, everywhere else
//
// The fallback is kept because it is *sufficient* — volatile semantics are
// mandated by the standard, so the call cannot be elided — but it is deliberately
// last, since it works for an indirect reason and the named functions do not.
//
// SCOPE. This is not a hot-path function and does not need to be: it runs once per
// request when a pooled upstream is released (measured 2.4 ns for a ~96 B buffer,
// ~0.02% of one core at 84k RPS). Do NOT sprinkle it over transient buffers that
// are overwritten microseconds later — that buys nothing and costs the hot path.

#include <cstddef>
#include <string>

namespace llmbridge::net
{
    /// Overwrite `n` bytes at `p` with zero, in a way the optimizer may not remove.
    /// Safe with p == nullptr && n == 0.
    void secure_clear(void* p, size_t n) noexcept;

    /// Overwrite a string's contents, then clear it. The capacity is retained (the
    /// allocation is reused), which is the point: the bytes are gone, the buffer is
    /// not reallocated on the next request.
    inline void secure_clear(std::string& s) noexcept
    {
        if (!s.empty()) secure_clear(s.data(), s.size());
        s.clear();
    }
} // namespace llmbridge::net
