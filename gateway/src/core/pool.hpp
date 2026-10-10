// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The keep-alive pool: one intrusive list per venue, newest first out, capped in
// total, O(1) to remove from and O(expired) to reap.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/conn.hpp"

namespace llmbridge
{
    class UpstreamPool
    {
      public:
        enum class Refusal : uint8_t
        {
            None,
            Closed,       ///< already closed, or no descriptor
            NotKeepAlive, ///< the response or the client asked to close
            BodyOpen,     ///< no message boundary was reached
            Leftover,     ///< bytes past the response would read as the next one
            Unsent,       ///< our request was still going out
            Full,         ///< `cap` connections are pooled already
        };

        void init(size_t venues, size_t cap)
        {
            _lists.assign(venues, List{});
            _cap = cap;
        }
        void set_cap(size_t cap) noexcept { _cap = cap; }
        [[nodiscard]] size_t cap() const noexcept { return _cap; }
        [[nodiscard]] size_t size() const noexcept { return _count; } // connections, not venues

        /// The newest connection to `venue`, unlinked, or null.
        Connection* acquire(int venue) noexcept;

        /// The one reusability check. On None the connection is scrubbed (its request
        /// carried a credential), stamped and pooled, and `u` has no peer.
        Refusal release(Connection& u, bool keep_alive, bool body_ended, bool request_sent,
                        int64_t now) noexcept;

        /// O(1); a no-op for a connection not in the pool.
        void remove(Connection& u) noexcept;

        /// Unlink and hand `close` every connection pooled before `cutoff`, oldest
        /// first; touches only those and one live one per venue.
        template <class Close> void reap(int64_t cutoff, Close&& close)
        {
            for (List& l : _lists)
                while (l.oldest && l.oldest->ts_pooled < cutoff)
                {
                    Connection* u = l.oldest;
                    remove(*u);
                    close(u);
                }
        }

        template <class F> void for_each(F&& f) const
        {
            for (const List& l : _lists)
                for (Connection* u = l.newest; u; u = u->pool_older) f(u);
        }

      private:
        struct List
        {
            Connection* newest = nullptr;
            Connection* oldest = nullptr;
        };
        std::vector<List> _lists;
        size_t _cap = 0;
        size_t _count = 0;
    };
} // namespace llmbridge
