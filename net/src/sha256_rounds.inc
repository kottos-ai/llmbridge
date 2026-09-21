// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The SHA-NI block loop, written once. sha256_ni.cpp includes it against the real
// intrinsics; the test includes it against a scalar model of the same three
// instructions, so the sequence is checked on a CPU that cannot execute it. Most
// machines this ships to have SHA-NI and most CI runners do not, which is exactly
// the split that lets an unexecuted path rot.
//
// The includer defines: LB_SHA_FN (function name), LB_SHA_ATTR (target attribute
// or nothing), and the V_* operation macros.
//
// State lives as ABEF/CDGH, not ABCD/EFGH, because that is the pairing
// SHA256RNDS2 reads: SRC1 supplies C,D,G,H and SRC2 supplies A,B,E,F.

LB_SHA_ATTR void LB_SHA_FN(uint32_t h[8], const uint8_t* p, size_t blocks) noexcept
{
    // Each dword is byte-reversed on load: the standard reads the message
    // big-endian and every host this runs on is little-endian.
    const V128 kSwap = V_SETR8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

    V128 tmp = V_LOAD(h);          // A B C D
    V128 state1 = V_LOAD(h + 4);   // E F G H
    tmp = V_SHUF32(tmp, 0xB1);     // C D A B
    state1 = V_SHUF32(state1, 0x1B); // E F G H, reversed
    V128 state0 = V_ALIGNR(tmp, state1, 8); // A B E F
    state1 = V_BLEND16(state1, tmp, 0xF0);  // C D G H

    while (blocks-- != 0)
    {
        const V128 abef = state0;
        const V128 cdgh = state1;

        V128 m0 = V_SHUF8(V_LOAD(p + 0), kSwap);
        V128 m1 = V_SHUF8(V_LOAD(p + 16), kSwap);
        V128 m2 = V_SHUF8(V_LOAD(p + 32), kSwap);
        V128 m3 = V_SHUF8(V_LOAD(p + 48), kSwap);
        V128 k;

// Two rounds per RNDS2: the low pair of k first, then the high pair, which is what
// the 0x0E shuffle moves down. Dropping that shuffle runs rounds 0 and 1 twice.
#define LB_SHA_QUAD(msg, round)                                 \
    k = V_ADD32(msg, V_LOAD32(kK + (round)));                   \
    state1 = V_RNDS2(state1, state0, k);                        \
    k = V_SHUF32(k, 0x0E);                                      \
    state0 = V_RNDS2(state0, state1, k)

// The schedule, four words ahead of the rounds consuming them. MSG2 needs the
// w[i-7] term, which is not in either operand, so it is folded in by hand: that
// is the alignr, and leaving it out is a silent wrong answer, not a crash.
#define LB_SHA_SCHED(a, b, prev)                                \
    b = V_ADD32(b, V_ALIGNR(a, prev, 4));                       \
    b = V_MSG2(b, a)

        LB_SHA_QUAD(m0, 0);
        LB_SHA_QUAD(m1, 4);
        m0 = V_MSG1(m0, m1);
        LB_SHA_QUAD(m2, 8);
        m1 = V_MSG1(m1, m2);

        LB_SHA_QUAD(m3, 12);  LB_SHA_SCHED(m3, m0, m2);  m2 = V_MSG1(m2, m3);
        LB_SHA_QUAD(m0, 16);  LB_SHA_SCHED(m0, m1, m3);  m3 = V_MSG1(m3, m0);
        LB_SHA_QUAD(m1, 20);  LB_SHA_SCHED(m1, m2, m0);  m0 = V_MSG1(m0, m1);
        LB_SHA_QUAD(m2, 24);  LB_SHA_SCHED(m2, m3, m1);  m1 = V_MSG1(m1, m2);
        LB_SHA_QUAD(m3, 28);  LB_SHA_SCHED(m3, m0, m2);  m2 = V_MSG1(m2, m3);
        LB_SHA_QUAD(m0, 32);  LB_SHA_SCHED(m0, m1, m3);  m3 = V_MSG1(m3, m0);
        LB_SHA_QUAD(m1, 36);  LB_SHA_SCHED(m1, m2, m0);  m0 = V_MSG1(m0, m1);
        LB_SHA_QUAD(m2, 40);  LB_SHA_SCHED(m2, m3, m1);  m1 = V_MSG1(m1, m2);
        LB_SHA_QUAD(m3, 44);  LB_SHA_SCHED(m3, m0, m2);  m2 = V_MSG1(m2, m3);
        LB_SHA_QUAD(m0, 48);  LB_SHA_SCHED(m0, m1, m3);  m3 = V_MSG1(m3, m0);
        // The schedule is complete here; the last three quads only consume it.
        LB_SHA_QUAD(m1, 52);  LB_SHA_SCHED(m1, m2, m0);
        LB_SHA_QUAD(m2, 56);  LB_SHA_SCHED(m2, m3, m1);
        LB_SHA_QUAD(m3, 60);

#undef LB_SHA_QUAD
#undef LB_SHA_SCHED

        state0 = V_ADD32(state0, abef);
        state1 = V_ADD32(state1, cdgh);
        p += 64;
    }

    tmp = V_SHUF32(state0, 0x1B);    // F E B A
    state1 = V_SHUF32(state1, 0xB1); // D C H G
    V_STORE(h, V_BLEND16(tmp, state1, 0xF0));
    V_STORE(h + 4, V_ALIGNR(state1, tmp, 8));
}
