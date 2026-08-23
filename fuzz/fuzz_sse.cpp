// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// libFuzzer target for the incremental Anthropic->OpenAI SSE translator
// (provider/sse.hpp): untrusted upstream bytes in, client-facing SSE out. This
// fuzzer checks more than "does not crash": it enforces the two invariants the
// translator promises, so libFuzzer actively hunts for a violation, not just a
// segfault. Build (Clang):
//   cmake -B build-fuzz -DLLMBRIDGE_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target fuzz_sse
//   ./build-fuzz/bin/fuzz_sse -max_total_time=120 fuzz/corpus/sse
//
// Invariants asserted on every input:
//   (1) Output strictness. The emitted stream never contains a bare C0 control
//       byte other than the '\n' we frame with (append_sanitized's guarantee).
//   (2) Fragmentation-invariance. Feeding the same bytes one-at-a-time yields
//       byte-identical output to a single feed(), whenever neither run trips a
//       buffer cap. This is the property most likely to break as state grows.

#include "provider/sse.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{
    bool output_is_strict(const std::string& s)
    {
        for (unsigned char c : s)
            if (c < 0x20 && c != '\n') return false;
        return true;
    }

    // A failed invariant is a bug: trap so libFuzzer records a crashing input.
    inline void must(bool cond)
    {
        if (!cond) __builtin_trap();
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    using llmbridge::provider::AnthropicToOpenAiSse;
    const std::string_view in(reinterpret_cast<const char*>(data), size);

    // Pin `created` so the two runs are comparable; otherwise a wall-clock
    // stamp read on a 1-second boundary would differ for benign reasons.
    constexpr long long kCreated = 1'700'000'000;

    // (a) one-shot feed
    std::string whole;
    bool ok_whole;
    {
        AnthropicToOpenAiSse t(kCreated);
        ok_whole = t.feed(in, whole);
        ok_whole = t.finish(whole) && ok_whole;
    }
    must(output_is_strict(whole));

    // (b) fragmentation in up to 32 chunks (byte-by-byte for tiny inputs). This
    // bounds the number of instrumented feed() calls per exec so the fuzzer stays
    // fast, while still exercising cross-chunk state at fuzzer-evolved boundaries.
    // Exhaustive 1-byte fragmentation is covered by the unit tests.
    std::string frag;
    bool ok_frag = true;
    {
        AnthropicToOpenAiSse t(kCreated);
        const size_t parts = size < 32 ? size : 32;
        size_t off = 0;
        for (size_t k = 0; k < parts; ++k)
        {
            const size_t end = (k + 1 == parts) ? size : (size * (k + 1)) / parts;
            if (end <= off) continue;
            if (!t.feed(std::string_view(in.data() + off, end - off), frag)) { ok_frag = false; break; }
            off = end;
        }
        if (ok_frag) ok_frag = t.finish(frag);
    }
    must(output_is_strict(frag));

    // (c) fragmentation must not change the output, but only when neither run
    // hit a cap (a cap can stop the two feeders at different points).
    if (ok_whole && ok_frag) must(whole == frag);

    return 0;
}
