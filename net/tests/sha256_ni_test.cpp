// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The SHA-NI round sequence, run on a CPU that has no SHA-NI.
//
// sha256_rounds.inc is included here against a scalar model of SHA256RNDS2,
// SHA256MSG1 and SHA256MSG2 written from the Intel SDM operand tables, so the
// exact sequence the shipped code issues is executed and checked against the
// published vectors. A wrong sequence and a wrong model would both have to be
// wrong in compensating ways to pass, which is what makes this worth having:
// most machines llmbridge ships to have these instructions and most CI runners
// do not, so without it the fast path is unexecuted until a customer runs it.
//
// It does not prove the intrinsics were spelled correctly, only the sequence.
// Nothing here substitutes for one run on real silicon.

#include "net/sha256.hpp"

#include <cstdint>
#include <cstring>

#include "../src/sha256_tables.hpp"

#include <gtest/gtest.h>

namespace
{
    using llmbridge::net::kK;

    struct U128
    {
        uint32_t w[4];  ///< w[0] is the low dword, matching lane order
    };

    uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
    uint32_t s0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    uint32_t s1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }
    uint32_t S0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    uint32_t S1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    uint32_t chf(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    uint32_t majf(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }

    U128 load128(const void* p)
    {
        U128 v{};
        std::memcpy(v.w, p, 16);
        return v;
    }
    void store128(void* p, U128 v) { std::memcpy(p, v.w, 16); }

    U128 add32(U128 a, U128 b)
    {
        return {{a.w[0] + b.w[0], a.w[1] + b.w[1], a.w[2] + b.w[2], a.w[3] + b.w[3]}};
    }

    /// PSHUFD: lane i takes the source lane named by two bits of the selector.
    U128 shuf32(U128 a, int imm)
    {
        U128 r{};
        for (int i = 0; i < 4; ++i) r.w[i] = a.w[(imm >> (2 * i)) & 3];
        return r;
    }

    /// PSHUFB, byte granularity, used here only to byte-reverse each dword.
    U128 shuf8(U128 a, U128 m)
    {
        uint8_t src[16], msk[16], out[16];
        std::memcpy(src, a.w, 16);
        std::memcpy(msk, m.w, 16);
        for (int i = 0; i < 16; ++i) out[i] = (msk[i] & 0x80) ? 0 : src[msk[i] & 15];
        U128 r{};
        std::memcpy(r.w, out, 16);
        return r;
    }

    /// PALIGNR: the pair (a:b) shifted right by n bytes, b occupying the low half.
    U128 alignr(U128 a, U128 b, int n)
    {
        uint8_t buf[32], out[16];
        std::memcpy(buf, b.w, 16);
        std::memcpy(buf + 16, a.w, 16);
        std::memcpy(out, buf + n, 16);
        U128 r{};
        std::memcpy(r.w, out, 16);
        return r;
    }

    /// PBLENDW over 8 word lanes; a set bit takes that word from b.
    U128 blend16(U128 a, U128 b, int imm)
    {
        uint16_t x[8], y[8];
        std::memcpy(x, a.w, 16);
        std::memcpy(y, b.w, 16);
        for (int i = 0; i < 8; ++i)
            if (imm & (1 << i)) x[i] = y[i];
        U128 r{};
        std::memcpy(r.w, x, 16);
        return r;
    }

    /// SHA256RNDS2: two rounds. src1 carries C,D,G,H and src2 carries A,B,E,F,
    /// both high dword first; k holds two message-plus-constant words, low first.
    U128 rnds2(U128 src1, U128 src2, U128 k)
    {
        uint32_t a = src2.w[3], b = src2.w[2], e = src2.w[1], f = src2.w[0];
        uint32_t c = src1.w[3], d = src1.w[2], g = src1.w[1], h = src1.w[0];
        for (int i = 0; i < 2; ++i)
        {
            const uint32_t wk = k.w[i];
            const uint32_t t = chf(e, f, g) + S1(e) + wk + h;
            const uint32_t na = t + majf(a, b, c) + S0(a);
            const uint32_t ne = t + d;
            h = g; g = f; f = e; e = ne;
            d = c; c = b; b = a; a = na;
        }
        return {{f, e, b, a}};
    }

    /// SHA256MSG1: adds sigma0 of the next word to each of four schedule words.
    U128 msg1(U128 a, U128 b)
    {
        return {{a.w[0] + s0(a.w[1]), a.w[1] + s0(a.w[2]), a.w[2] + s0(a.w[3]),
                 a.w[3] + s0(b.w[0])}};
    }

    /// SHA256MSG2: adds sigma1 of the two preceding words, then of the two it has
    /// just produced, which is why the last two lanes depend on the first two.
    U128 msg2(U128 a, U128 b)
    {
        U128 r{};
        r.w[0] = a.w[0] + s1(b.w[2]);
        r.w[1] = a.w[1] + s1(b.w[3]);
        r.w[2] = a.w[2] + s1(r.w[0]);
        r.w[3] = a.w[3] + s1(r.w[1]);
        return r;
    }

#define V128 U128
#define V_LOAD(p) load128(p)
#define V_LOAD32(p) load128(p)
#define V_STORE(p, v) store128((p), (v))
#define V_ADD32(a, b) add32((a), (b))
#define V_SHUF32(a, i) shuf32((a), (i))
#define V_SHUF8(a, m) shuf8((a), (m))
#define V_ALIGNR(a, b, n) alignr((a), (b), (n))
#define V_BLEND16(a, b, m) blend16((a), (b), (m))
#define V_RNDS2(a, b, k) rnds2((a), (b), (k))
#define V_MSG1(a, b) msg1((a), (b))
#define V_MSG2(a, b) msg2((a), (b))
#define V_SETR8(...) make_bytes(__VA_ARGS__)
#define LB_SHA_FN modelled_ni_blocks
#define LB_SHA_ATTR

    template <class... B>
    U128 make_bytes(B... bs)
    {
        const uint8_t v[16] = {static_cast<uint8_t>(bs)...};
        U128 r{};
        std::memcpy(r.w, v, 16);
        return r;
    }

#include "../src/sha256_rounds.inc"

    /// The digest the modelled sequence produces, padded the way Sha256 pads.
    std::string modelled_hex(const std::string& msg)
    {
        uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::string m = msg;
        const uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
        m.push_back('\x80');
        while (m.size() % 64 != 56) m.push_back('\0');
        for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>(bits >> (i * 8)));
        modelled_ni_blocks(h, reinterpret_cast<const uint8_t*>(m.data()), m.size() / 64);

        static const char* kHex = "0123456789abcdef";
        std::string out;
        for (uint32_t v : h)
            for (int i = 3; i >= 0; --i)
            {
                const uint8_t byte = static_cast<uint8_t>(v >> (i * 8));
                out.push_back(kHex[byte >> 4]);
                out.push_back(kHex[byte & 15]);
            }
        return out;
    }
} // namespace

using llmbridge::net::hex;
using llmbridge::net::Sha256;

TEST(Sha256Ni, TheSequenceMatchesThePublishedVectors)
{
    EXPECT_EQ(modelled_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(modelled_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(modelled_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// Multi-block, which is where a sequence that forgets to carry state across the
// loop still passes every single-block vector.
TEST(Sha256Ni, TheSequenceAgreesWithTheScalarPathOnEveryLength)
{
    std::string msg;
    for (size_t n = 0; n <= 600; ++n)
    {
        ASSERT_EQ(modelled_hex(msg), hex(Sha256::hash(msg))) << "at length " << n;
        msg.push_back(static_cast<char>('a' + (n % 26)));
    }
}

// Whatever this CPU is, the two paths the binary can choose between have to agree.
TEST(Sha256Ni, WhicheverBackendThisCpuSelectedAgreesWithTheVectors)
{
    EXPECT_EQ(hex(Sha256::hash("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string big(100000, 'a');
    EXPECT_EQ(hex(Sha256::hash(big)),
              "6d1cf22d7cc09b085dfc25ee1a1f3ae0265804c607bc2074ad253bcc82fd81ee");
}
