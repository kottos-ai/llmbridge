// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "net/uring.hpp"

#ifdef LLMBRIDGE_HAVE_URING

#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace llmbridge::net::uring
{
    namespace
    {
        // Raw syscalls — glibc doesn't wrap these, and we don't pull in liburing.
        int sys_io_uring_setup(unsigned entries, io_uring_params* p) noexcept
        {
            return static_cast<int>(::syscall(__NR_io_uring_setup, entries, p));
        }
        int sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                               unsigned flags) noexcept
        {
            return static_cast<int>(
                ::syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
                          static_cast<sigset_t*>(nullptr), _NSIG / 8));
        }
        int sys_io_uring_register(int fd, unsigned opcode, void* arg, unsigned nr) noexcept
        {
            return static_cast<int>(::syscall(__NR_io_uring_register, fd, opcode, arg, nr));
        }
    } // namespace

    bool available() noexcept
    {
        io_uring_params p;
        std::memset(&p, 0, sizeof(p));
        const int fd = sys_io_uring_setup(4, &p);
        if (fd < 0) return false;
        ::close(fd);
        return true;
    }

    bool Ring::init(unsigned entries, unsigned flags) noexcept
    {
        io_uring_params p;
        std::memset(&p, 0, sizeof(p));
        p.flags = flags;

        const int fd = sys_io_uring_setup(entries, &p);
        if (fd < 0) return false;
        _ring_fd = fd;

        _sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
        _cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);

        // Modern kernels map SQ and CQ rings in one region; size to the larger.
        const bool single_mmap = (p.features & IORING_FEAT_SINGLE_MMAP) != 0;
        if (single_mmap)
        {
            if (_cq_ring_sz > _sq_ring_sz) _sq_ring_sz = _cq_ring_sz;
            _cq_ring_sz = _sq_ring_sz;
        }

        _sq_ring = ::mmap(nullptr, _sq_ring_sz, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE, _ring_fd, IORING_OFF_SQ_RING);
        if (_sq_ring == MAP_FAILED) { _sq_ring = nullptr; teardown(); return false; }

        if (single_mmap)
        {
            _cq_ring = _sq_ring;
        }
        else
        {
            _cq_ring = ::mmap(nullptr, _cq_ring_sz, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_POPULATE, _ring_fd, IORING_OFF_CQ_RING);
            if (_cq_ring == MAP_FAILED) { _cq_ring = nullptr; teardown(); return false; }
        }

        _sqes_sz = p.sq_entries * sizeof(io_uring_sqe);
        _sqes = static_cast<io_uring_sqe*>(
            ::mmap(nullptr, _sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                   _ring_fd, IORING_OFF_SQES));
        if (_sqes == MAP_FAILED) { _sqes = nullptr; teardown(); return false; }

        char* sq = static_cast<char*>(_sq_ring);
        _sq_head = reinterpret_cast<unsigned*>(sq + p.sq_off.head);
        _sq_tail = reinterpret_cast<unsigned*>(sq + p.sq_off.tail);
        _sq_array = reinterpret_cast<unsigned*>(sq + p.sq_off.array);
        _sq_mask = *reinterpret_cast<unsigned*>(sq + p.sq_off.ring_mask);
        _sq_entries = *reinterpret_cast<unsigned*>(sq + p.sq_off.ring_entries);

        char* cq = static_cast<char*>(_cq_ring);
        _cq_head = reinterpret_cast<unsigned*>(cq + p.cq_off.head);
        _cq_tail = reinterpret_cast<unsigned*>(cq + p.cq_off.tail);
        _cq_mask = *reinterpret_cast<unsigned*>(cq + p.cq_off.ring_mask);
        _cqes = reinterpret_cast<io_uring_cqe*>(cq + p.cq_off.cqes);

        // We fill SQEs in ring order, so the index array is the identity map and
        // stays constant — set it once, then submit just advances the SQ tail.
        for (unsigned i = 0; i < _sq_entries; ++i) _sq_array[i] = i;
        _sqe_tail = *_sq_tail; // align our cursor with the kernel's (usually 0)
        return true;
    }

    int Ring::submit_and_wait(unsigned min_complete) noexcept
    {
        // Publish any SQEs filled since the last submit (identity array, so just
        // advance the tail with a release store so the kernel sees the new SQEs).
        const unsigned to_submit = _sqe_tail - *_sq_tail;
        if (to_submit) __atomic_store_n(_sq_tail, _sqe_tail, __ATOMIC_RELEASE);

        if (to_submit == 0 && min_complete == 0) return 0; // nothing to do

        unsigned flags = 0;
        if (min_complete > 0) flags |= IORING_ENTER_GETEVENTS;

        int ret;
        do
        {
            ret = sys_io_uring_enter(_ring_fd, to_submit, min_complete, flags);
        } while (ret < 0 && errno == EINTR);

        return ret < 0 ? -errno : ret;
    }

    void Ring::teardown() noexcept
    {
        if (_sqes && _sqes != MAP_FAILED) ::munmap(_sqes, _sqes_sz);
        if (_cq_ring && _cq_ring != _sq_ring) ::munmap(_cq_ring, _cq_ring_sz);
        if (_sq_ring && _sq_ring != MAP_FAILED) ::munmap(_sq_ring, _sq_ring_sz);
        if (_ring_fd >= 0) ::close(_ring_fd);
        _sqes = nullptr;
        _cq_ring = nullptr;
        _sq_ring = nullptr;
        _ring_fd = -1;
    }

    Ring::~Ring() { teardown(); }

    // ── BufRing (provided-buffer ring) ──────────────────────────────────────

    bool BufRing::init(Ring& ring, unsigned bgid, unsigned count, unsigned buf_size) noexcept
    {
        if (count == 0 || (count & (count - 1)) != 0) return false; // must be power of two
        _ring_fd = ring.ring_fd();
        _bgid = bgid;
        _count = count;
        _mask = count - 1;
        _buf_size = buf_size;

        _ring_sz = static_cast<size_t>(count) * sizeof(io_uring_buf);
        _ring = ::mmap(nullptr, _ring_sz, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (_ring == MAP_FAILED) { _ring = nullptr; return false; }

        _bufs_sz = static_cast<size_t>(count) * buf_size;
        _bufs = static_cast<char*>(
            ::mmap(nullptr, _bufs_sz, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
        if (_bufs == MAP_FAILED) { _bufs = nullptr; teardown(); return false; }

        io_uring_buf_reg reg;
        std::memset(&reg, 0, sizeof(reg));
        reg.ring_addr = reinterpret_cast<uint64_t>(_ring);
        reg.ring_entries = count;
        reg.bgid = static_cast<uint16_t>(bgid);
        if (sys_io_uring_register(_ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0)
        {
            teardown();
            return false;
        }

        // Publish all buffers. (bufs[0].resv aliases the ring tail; writing
        // addr/len/bid doesn't touch it, then we store the tail with release.)
        auto* br = static_cast<io_uring_buf_ring*>(_ring);
        for (unsigned i = 0; i < count; ++i)
        {
            br->bufs[i].addr = reinterpret_cast<uint64_t>(_bufs + static_cast<size_t>(i) * buf_size);
            br->bufs[i].len = buf_size;
            br->bufs[i].bid = static_cast<uint16_t>(i);
        }
        _tail = count;
        __atomic_store_n(&br->tail, static_cast<uint16_t>(_tail), __ATOMIC_RELEASE);
        return true;
    }

    void BufRing::recycle(unsigned bid) noexcept
    {
        auto* br = static_cast<io_uring_buf_ring*>(_ring);
        const unsigned idx = _tail & _mask;
        br->bufs[idx].addr = reinterpret_cast<uint64_t>(_bufs + static_cast<size_t>(bid) * _buf_size);
        br->bufs[idx].len = _buf_size;
        br->bufs[idx].bid = static_cast<uint16_t>(bid);
        ++_tail;
        __atomic_store_n(&br->tail, static_cast<uint16_t>(_tail), __ATOMIC_RELEASE);
    }

    void BufRing::teardown() noexcept
    {
        if (_bufs && _bufs != MAP_FAILED) ::munmap(_bufs, _bufs_sz);
        if (_ring && _ring != MAP_FAILED) ::munmap(_ring, _ring_sz);
        _bufs = nullptr;
        _ring = nullptr;
    }

    BufRing::~BufRing() { teardown(); }
} // namespace llmbridge::net::uring

#endif // LLMBRIDGE_HAVE_URING
