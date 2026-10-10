// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// A dense set of connections: O(1) add and remove, contiguous to walk.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace llmbridge
{
    /// Each member stores its own index in `slot`, so it belongs to one registry at a
    /// time. Removal swaps the last member into the hole: order is not kept.
    template <class T> class Registry
    {
      public:
        static constexpr uint32_t kNoSlot = UINT32_MAX;

        void add(T* p)
        {
            p->slot = static_cast<uint32_t>(_items.size());
            _items.push_back(p);
        }
        /// A no-op for a member of another registry, or of none.
        void remove(T* p) noexcept
        {
            if (!holds(p)) return;
            T* last = _items.back();
            _items[p->slot] = last;
            last->slot = p->slot;
            _items.pop_back();
            p->slot = kNoSlot;
        }
        [[nodiscard]] bool holds(const T* p) const noexcept
        {
            return p->slot < _items.size() && _items[p->slot] == p;
        }
        /// Empty the registry, handing each member to `f`, which may free it.
        template <class F> void clear(F&& f)
        {
            for (T* p : _items)
            {
                p->slot = kNoSlot;
                f(p);
            }
            _items.clear();
        }

        [[nodiscard]] size_t size() const noexcept { return _items.size(); }
        [[nodiscard]] bool empty() const noexcept { return _items.empty(); }
        T* operator[](size_t i) const noexcept { return _items[i]; }
        auto begin() const noexcept { return _items.begin(); }
        auto end() const noexcept { return _items.end(); }

      private:
        std::vector<T*> _items;
    };
} // namespace llmbridge
