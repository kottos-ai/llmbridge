// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// A minimal, hand-rolled raw io_uring: the completion-based I/O engine that will
// back the gateway's hot path (Phase 1), replacing the readiness-based epoll loop
// to batch the ~4 syscalls/request toward <<1. Deliberately no liburing: we drive
// io_uring_setup / io_uring_enter directly and mmap the SQ/CQ rings ourselves, so
// the runtime stays dependency-free like the rest of llmbridge.
//
// Single-issuer only (one thread owns the ring), matching the one-worker design.
// The hot methods (get_sqe, for_each_cqe) are header-inline; setup/submit are
// out-of-line in uring.cpp. Compiled only where <linux/io_uring.h> exists; callers
// gate on LLMBRIDGE_HAVE_URING.

#ifdef LLMBRIDGE_HAVE_URING

#include <linux/io_uring.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace llmbridge::net::uring
{
    // Is io_uring usable in this process? Probes by setting up and tearing down a
    // tiny ring. False on old kernels or seccomp-sandboxed environments; the
    // gateway falls back to epoll in that case.
    bool available() noexcept;

    class Ring
    {
    public:
        Ring() = default;
        ~Ring();
        Ring(const Ring&) = delete;
        Ring& operator=(const Ring&) = delete;

        // Set up a ring with `entries` submission slots (the kernel rounds up to a
        // power of two). `flags` are IORING_SETUP_*, e.g. SINGLE_ISSUER |
        // DEFER_TASKRUN for a single-worker loop. Returns false on failure.
        bool init(unsigned entries, unsigned flags = 0) noexcept;

        [[nodiscard]] bool valid() const noexcept { return _ring_fd >= 0; }
        [[nodiscard]] int ring_fd() const noexcept { return _ring_fd; }

        // Acquire the next free submission entry to fill (zeroed), or nullptr if
        // the SQ is full: submit first, then retry.
        io_uring_sqe* get_sqe() noexcept
        {
            const unsigned head = __atomic_load_n(_sq_head, __ATOMIC_ACQUIRE);
            const unsigned next = _sqe_tail + 1;
            if (next - head > _sq_entries) return nullptr; // ring full
            io_uring_sqe* sqe = &_sqes[_sqe_tail & _sq_mask];
            _sqe_tail = next;
            std::memset(sqe, 0, sizeof(*sqe));
            return sqe;
        }

        // Flush prepared SQEs to the kernel; if min_complete > 0, also wait for
        // that many completions. Returns the number of SQEs consumed, or -errno.
        int submit_and_wait(unsigned min_complete) noexcept;
        int submit() noexcept { return submit_and_wait(0); }

        // Invoke fn(const io_uring_cqe*) for each ready completion, then advance the
        // CQ head so the kernel can reuse those slots. Returns the count reaped.
        template <class F>
        unsigned for_each_cqe(F&& fn) noexcept
        {
            unsigned head = *_cq_head;
            const unsigned tail = __atomic_load_n(_cq_tail, __ATOMIC_ACQUIRE);
            unsigned count = 0;
            for (; head != tail; ++head, ++count)
                fn(const_cast<const io_uring_cqe*>(&_cqes[head & _cq_mask]));
            if (count) __atomic_store_n(_cq_head, head, __ATOMIC_RELEASE);
            return count;
        }

    private:
        void teardown() noexcept;

        int _ring_fd = -1;

        void* _sq_ring = nullptr;
        std::size_t _sq_ring_sz = 0;
        void* _cq_ring = nullptr; // == _sq_ring when IORING_FEAT_SINGLE_MMAP
        std::size_t _cq_ring_sz = 0;
        io_uring_sqe* _sqes = nullptr;
        std::size_t _sqes_sz = 0;

        // SQ ring (kernel-shared head/tail + our local fill cursor).
        unsigned* _sq_head = nullptr;
        unsigned* _sq_tail = nullptr;
        unsigned* _sq_array = nullptr;
        unsigned _sq_mask = 0;
        unsigned _sq_entries = 0;
        unsigned _sqe_tail = 0; // absolute count of SQEs we've filled

        // CQ ring.
        unsigned* _cq_head = nullptr;
        unsigned* _cq_tail = nullptr;
        io_uring_cqe* _cqes = nullptr;
        unsigned _cq_mask = 0;
    };

    // A provided-buffer ring (IORING_REGISTER_PBUF_RING): a pool of `count`
    // fixed-size buffers the kernel draws from for multishot recv. One submitted
    // multishot recv per connection then delivers a completion per data arrival
    // each naming the buffer it used, so we stop re-submitting a recv (and a
    // per-connection staging buffer) per read. After consuming a buffer's bytes,
    // recycle() returns it to the pool. Single-issuer, like the ring itself.
    class BufRing
    {
    public:
        BufRing() = default;
        ~BufRing();
        BufRing(const BufRing&) = delete;
        BufRing& operator=(const BufRing&) = delete;

        // `count` must be a power of two. Returns false on failure.
        bool init(Ring& ring, unsigned bgid, unsigned count, unsigned buf_size) noexcept;

        [[nodiscard]] bool valid() const noexcept { return _ring != nullptr; }
        [[nodiscard]] unsigned bgid() const noexcept { return _bgid; }
        [[nodiscard]] unsigned buf_size() const noexcept { return _buf_size; }

        // Why the last init() returned false: which step, and the errno the kernel
        // gave. init() clears both on entry, so a caller reads them only after a
        // false return. The stage is a static string, never allocated.
        [[nodiscard]] const char* init_stage() const noexcept { return _init_stage; }
        [[nodiscard]] int init_errno() const noexcept { return _init_errno; }

        // Pointer to buffer `bid`'s bytes (the kernel just filled it on a recv CQE).
        [[nodiscard]] char* data(unsigned bid) noexcept { return _bufs + static_cast<size_t>(bid) * _buf_size; }

        // Return buffer `bid` to the kernel pool (after its bytes are consumed).
        void recycle(unsigned bid) noexcept;

    private:
        void teardown() noexcept;

        int _ring_fd = -1;
        void* _ring = nullptr; // mmap'd io_uring_buf_ring
        std::size_t _ring_sz = 0;
        char* _bufs = nullptr; // the buffer memory
        std::size_t _bufs_sz = 0;
        unsigned _bgid = 0;
        unsigned _count = 0;
        unsigned _mask = 0;
        unsigned _buf_size = 0;
        unsigned _tail = 0; // our producer cursor into the buf ring
        const char* _init_stage = ""; // last init() failure step, for diagnostics
        int _init_errno = 0;
    };
} // namespace llmbridge::net::uring

#endif // LLMBRIDGE_HAVE_URING
