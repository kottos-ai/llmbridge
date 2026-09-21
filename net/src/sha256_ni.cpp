// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
#include "sha256_tables.hpp"

#include <cstddef>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #if defined(__GNUC__) || defined(__clang__)
        #define LB_SHA_NI_BUILT 1
        #include <immintrin.h>
    #endif
#endif

namespace llmbridge::net
{
#if defined(LB_SHA_NI_BUILT)
    // The whole point of the target attribute is that this compiles with no -msha
    // on the command line, so the shipped binary keeps running on a CPU without the
    // instructions. Selecting the path is sha256_ni_available's job, at runtime.
    #define V128 __m128i
    #define V_LOAD(p) _mm_loadu_si128(reinterpret_cast<const __m128i*>(p))
    #define V_LOAD32(p) _mm_loadu_si128(reinterpret_cast<const __m128i*>(p))
    #define V_STORE(p, v) _mm_storeu_si128(reinterpret_cast<__m128i*>(p), (v))
    #define V_ADD32(a, b) _mm_add_epi32((a), (b))
    #define V_SHUF32(a, i) _mm_shuffle_epi32((a), (i))
    #define V_SHUF8(a, m) _mm_shuffle_epi8((a), (m))
    #define V_ALIGNR(a, b, n) _mm_alignr_epi8((a), (b), (n))
    #define V_BLEND16(a, b, m) _mm_blend_epi16((a), (b), (m))
    #define V_RNDS2(a, b, k) _mm_sha256rnds2_epu32((a), (b), (k))
    #define V_MSG1(a, b) _mm_sha256msg1_epu32((a), (b))
    #define V_MSG2(a, b) _mm_sha256msg2_epu32((a), (b))
    #define V_SETR8(...) _mm_setr_epi8(__VA_ARGS__)
    #define LB_SHA_FN sha256_ni_blocks
    #define LB_SHA_ATTR __attribute__((target("sha,sse4.1,ssse3")))
    #include "sha256_rounds.inc"

    bool sha256_ni_available() noexcept
    {
        // Both, and separately. AVX2 does not imply SHA-NI: Haswell through Skylake
        // have AVX2 and no SHA, so gating on the wrong feature costs a SIGILL on a
        // very common desktop CPU, not a slow path.
        return __builtin_cpu_supports("sha") != 0 && __builtin_cpu_supports("sse4.1") != 0;
    }
#else
    bool sha256_ni_available() noexcept { return false; }
    void sha256_ni_blocks(uint32_t[8], const uint8_t*, size_t) noexcept {}
#endif
} // namespace llmbridge::net
