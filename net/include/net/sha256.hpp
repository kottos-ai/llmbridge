// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/// SHA-256 (FIPS 180-4), implemented here, not taken from OpenSSL.
namespace llmbridge::net
{
    class Sha256
    {
      public:
        static constexpr size_t kDigestBytes = 32;
        static constexpr size_t kBlockBytes = 64;
        using Digest = std::array<uint8_t, kDigestBytes>;

        Sha256() noexcept { reset(); }

        /// Feed the next bytes of the message. Any split gives the same digest.
        void update(const uint8_t* data, size_t len) noexcept;
        void update(std::string_view data) noexcept
        {
            update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        }

        /// The digest of everything fed so far, after which the object is reset and
        /// ready for a new message. Calling it twice therefore gives the digest of
        /// this message and then of the empty one.
        [[nodiscard]] Digest finish() noexcept;

        void reset() noexcept;

        /// One shot, for a caller that already holds the whole message.
        [[nodiscard]] static Digest hash(std::string_view data) noexcept
        {
            Sha256 s;
            s.update(data);
            return s.finish();
        }

        /// The leading 8 bytes as a big-endian integer. What a caller wants when the
        /// field it must fit is 64 bits wide: truncating a digest is the standard way
        /// to narrow one, and it keeps the uniformity the full digest has.
        [[nodiscard]] static uint64_t truncated(const Digest& d) noexcept;

      private:
        /// The SHA-NI path keeps the state in registers across them,
        /// and the per-call dispatch is paid once for the run
        /// instead of once per 64 bytes.
        void compress_blocks(const uint8_t* p, size_t blocks) noexcept;

        std::array<uint32_t, 8> _h{};
        std::array<uint8_t, kBlockBytes> _buf{};
        uint64_t _bits = 0;  ///< message length in bits, which is what the pad encodes
        size_t _held = 0;    ///< bytes sitting in _buf, always below kBlockBytes
    };

    /// Lowercase hex, 64 characters. For logs and test vectors, never a hot path.
    [[nodiscard]] std::string hex(const Sha256::Digest& d);
} // namespace llmbridge::net
