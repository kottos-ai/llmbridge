// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
#include "net/sha256.hpp"

#include <cstring>

#include "sha256_tables.hpp"

namespace llmbridge::net
{
    namespace
    {
        constexpr uint32_t rotr(uint32_t x, unsigned n) noexcept
        {
            return (x >> n) | (x << (32 - n));
        }
        constexpr uint32_t ch(uint32_t x, uint32_t y, uint32_t z) noexcept
        {
            return (x & y) ^ (~x & z);
        }
        constexpr uint32_t maj(uint32_t x, uint32_t y, uint32_t z) noexcept
        {
            return (x & y) ^ (x & z) ^ (y & z);
        }
        constexpr uint32_t bsig0(uint32_t x) noexcept
        {
            return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
        }
        constexpr uint32_t bsig1(uint32_t x) noexcept
        {
            return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
        }
        constexpr uint32_t ssig0(uint32_t x) noexcept
        {
            return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
        }
        constexpr uint32_t ssig1(uint32_t x) noexcept
        {
            return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
        }

        // Read and write big-endian explicitly. The standard defines the message
        // schedule and the digest in big-endian words, and this file runs on
        // little-endian hosts, so a memcpy of the native layout is a wrong answer
        // that only shows up as a failed vector.
        constexpr uint32_t be32(const uint8_t* p) noexcept
        {
            return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
        }
        void put_be32(uint8_t* p, uint32_t v) noexcept
        {
            p[0] = static_cast<uint8_t>(v >> 24);
            p[1] = static_cast<uint8_t>(v >> 16);
            p[2] = static_cast<uint8_t>(v >> 8);
            p[3] = static_cast<uint8_t>(v);
        }
    } // namespace

    void Sha256::reset() noexcept
    {
        // Section 5.3.3: the first 32 bits of the fractional parts of the square
        // roots of the first eight primes.
        _h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        _buf = {};
        _bits = 0;
        _held = 0;
    }

    namespace
    {
        void compress_one(uint32_t h[8], const uint8_t* block) noexcept
        {
            uint32_t w[64];
            for (size_t i = 0; i < 16; ++i) w[i] = be32(block + i * 4);
            for (size_t i = 16; i < 64; ++i)
                w[i] = ssig1(w[i - 2]) + w[i - 7] + ssig0(w[i - 15]) + w[i - 16];

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
            for (size_t i = 0; i < 64; ++i)
            {
                const uint32_t t1 = hh + bsig1(e) + ch(e, f, g) + kK[i] + w[i];
                const uint32_t t2 = bsig0(a) + maj(a, b, c);
                hh = g; g = f; f = e; e = d + t1;
                d = c; c = b; b = a; a = t1 + t2;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }

        void compress_blocks_scalar(uint32_t h[8], const uint8_t* p, size_t blocks) noexcept
        {
            for (; blocks != 0; --blocks, p += Sha256::kBlockBytes) compress_one(h, p);
        }

        using BlockFn = void (*)(uint32_t*, const uint8_t*, size_t) noexcept;

        // Resolved once, before main. A hash run from another translation unit's
        // static initialiser can still beat this one, and zero initialisation is
        // what makes the null check in compress_blocks defined instead of a read
        // of a garbage pointer.
        const BlockFn g_blocks =
            sha256_ni_available() ? &sha256_ni_blocks : &compress_blocks_scalar;
    } // namespace

    void Sha256::compress_blocks(const uint8_t* p, size_t blocks) noexcept
    {
        const BlockFn f = g_blocks != nullptr ? g_blocks : &compress_blocks_scalar;
        f(_h.data(), p, blocks);
    }

    void Sha256::update(const uint8_t* data, size_t len) noexcept
    {
        if (data == nullptr || len == 0) return;
        _bits += static_cast<uint64_t>(len) * 8;
        // Top up a partial block first, then take whole blocks straight from the
        // caller's buffer, so a large body is never copied through _buf.
        if (_held != 0)
        {
            const size_t want = kBlockBytes - _held;
            const size_t take = len < want ? len : want;
            std::memcpy(_buf.data() + _held, data, take);
            _held += take;
            data += take;
            len -= take;
            if (_held < kBlockBytes) return;
            compress_blocks(_buf.data(), 1);
            _held = 0;
        }
        if (len >= kBlockBytes)
        {
            const size_t blocks = len / kBlockBytes;
            compress_blocks(data, blocks);
            data += blocks * kBlockBytes;
            len -= blocks * kBlockBytes;
        }
        if (len != 0)
        {
            std::memcpy(_buf.data(), data, len);
            _held = len;
        }
    }

    Sha256::Digest Sha256::finish() noexcept
    {
        // Section 5.1.1: a 1 bit, then zeros, then the length as 64 big-endian bits,
        // padded so the total is a whole number of blocks. The length is captured
        // before the padding is appended, because the padding is not message.
        const uint64_t bits = _bits;
        uint8_t one = 0x80;
        update(&one, 1);
        _bits = bits;
        while (_held != kBlockBytes - 8)
        {
            uint8_t zero = 0;
            update(&zero, 1);
            _bits = bits;
        }
        for (int i = 7; i >= 0; --i)
        {
            uint8_t b = static_cast<uint8_t>(bits >> (i * 8));
            _buf[_held++] = b;
        }
        compress_blocks(_buf.data(), 1);

        Digest out{};
        for (size_t i = 0; i < 8; ++i) put_be32(out.data() + i * 4, _h[i]);
        reset();
        return out;
    }

    uint64_t Sha256::truncated(const Digest& d) noexcept
    {
        uint64_t v = 0;
        for (size_t i = 0; i < 8; ++i) v = (v << 8) | d[i];
        return v;
    }

    std::string hex(const Sha256::Digest& d)
    {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string out(Sha256::kDigestBytes * 2, '\0');
        for (size_t i = 0; i < Sha256::kDigestBytes; ++i)
        {
            out[i * 2] = kDigits[d[i] >> 4];
            out[i * 2 + 1] = kDigits[d[i] & 0x0f];
        }
        return out;
    }
} // namespace llmbridge::net
