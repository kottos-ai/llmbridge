// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Bytes owed to one socket, and the pin that keeps them still while the kernel reads.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "net/secure.hpp"

namespace llmbridge
{
    /// Two halves: the transport reads `wire()` out of the front; while a send pins the
    /// front, new bytes stage behind it and fold in when the send completes. Only a
    /// driver pins (io_uring at submit), so on epoll this is one buffer. The front keeps
    /// what was sent until it is staged over, cleared or taken, so a request survives
    /// for a resend. GATEWAY-INTERNALS.md 2.
    class OutBuf
    {
      public:
        /// Where new bytes go. Never behind bytes already on the wire: a drained front
        /// is reset first, keeping its capacity.
        std::string& stage() noexcept
        {
            if (_pinned) return _back;
            if (_off != 0 && _off == _front.size())
            {
                _front.clear();
                _off = 0;
            }
            return _front;
        }
        [[nodiscard]] std::string_view wire() const noexcept
        {
            return std::string_view(_front).substr(_off);
        }
        void pin() noexcept { _pinned = true; }
        /// `n` bytes left; unpins. Staged bytes join the front, behind what is unsent.
        void sent(size_t n) noexcept
        {
            _off += n;
            _pinned = false;
            if (_back.empty()) return;
            if (_off == _front.size())
            {
                _front.clear();
                _front.swap(_back);
                _off = 0;
            }
            else
            {
                _front.append(_back);
                _back.clear();
            }
        }
        [[nodiscard]] bool pinned() const noexcept { return _pinned; }
        [[nodiscard]] bool idle() const noexcept { return _off == _front.size() && _back.empty(); }
        /// Not yet on the wire, staged bytes included.
        [[nodiscard]] size_t unsent() const noexcept { return _front.size() - _off + _back.size(); }
        /// Everything staged, sent or not, for a resend: moved when unpinned, copied
        /// when a send still reads it.
        std::string take()
        {
            std::string out;
            if (_pinned) out = _front;
            else
            {
                out.swap(_front);
                _off = 0;
            }
            out.append(_back);
            _back.clear();
            return out;
        }
        /// Replace an empty, unpinned buffer's storage (a warm buffer's capacity).
        void adopt(std::string&& s) noexcept
        {
            _front = std::move(s);
            _front.clear();
            _off = 0;
        }
        /// Unpinned only: drop the bytes, keep the capacity.
        void clear() noexcept
        {
            _front.clear();
            _back.clear();
            _off = 0;
        }
        /// Erase both halves (a request carries a credential), capacity kept.
        void scrub() noexcept
        {
            net::secure_clear(_front);
            net::secure_clear(_back);
            _off = 0;
        }
        /// The front as staged, sent bytes included: the reply's status, a log size.
        [[nodiscard]] const std::string& bytes() const noexcept { return _front; }
        [[nodiscard]] size_t capacity() const noexcept { return _front.capacity(); }

      private:
        std::string _front, _back;
        size_t _off = 0;
        bool _pinned = false;
    };
} // namespace llmbridge
