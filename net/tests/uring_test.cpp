// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Phase-0 acceptance for the raw io_uring wrapper: prove the ring sets up, and
// that submit/reap drive real completions (NOP and a socketpair SEND/RECV).

#include <gtest/gtest.h>

#ifdef LLMBRIDGE_HAVE_URING

#include "net/uring.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

using llmbridge::net::uring::BufRing;
using llmbridge::net::uring::Ring;
using llmbridge::net::uring::available;

namespace
{
    // Try the single-issuer/defer-taskrun flags (ideal for one worker); fall back
    // to plain setup if the kernel is older than those features.
    bool init_best(Ring& r, unsigned entries)
    {
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_DEFER_TASKRUN)
        if (r.init(entries, IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN)) return true;
#endif
        return r.init(entries, 0);
    }
} // namespace

TEST(Uring, AvailableProbeWorks)
{
    // On this build the header exists; availability depends on the kernel/sandbox.
    if (!available()) GTEST_SKIP() << "io_uring not available in this environment";
    Ring r;
    EXPECT_TRUE(init_best(r, 8));
    EXPECT_TRUE(r.valid());
    EXPECT_GE(r.ring_fd(), 0);
}

TEST(Uring, NopCompletes)
{
    if (!available()) GTEST_SKIP();
    Ring r;
    ASSERT_TRUE(init_best(r, 8));

    io_uring_sqe* sqe = r.get_sqe();
    ASSERT_NE(sqe, nullptr);
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = 0xABCDull;

    ASSERT_EQ(r.submit_and_wait(1), 1);

    bool seen = false;
    const unsigned n = r.for_each_cqe([&](const io_uring_cqe* cqe) {
        if (cqe->user_data == 0xABCDull) { seen = true; EXPECT_GE(cqe->res, 0); }
    });
    EXPECT_EQ(n, 1u);
    EXPECT_TRUE(seen);
}

TEST(Uring, SocketpairSendRecvRoundTrip)
{
    if (!available()) GTEST_SKIP();
    Ring r;
    ASSERT_TRUE(init_best(r, 16));

    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    char rbuf[64];
    std::memset(rbuf, 0, sizeof(rbuf));
    const char msg[] = "hello-uring";
    const unsigned mlen = sizeof(msg) - 1;

    io_uring_sqe* rsqe = r.get_sqe();
    ASSERT_NE(rsqe, nullptr);
    rsqe->opcode = IORING_OP_RECV;
    rsqe->fd = sv[0];
    rsqe->addr = reinterpret_cast<std::uint64_t>(rbuf);
    rsqe->len = sizeof(rbuf);
    rsqe->user_data = 1;

    io_uring_sqe* ssqe = r.get_sqe();
    ASSERT_NE(ssqe, nullptr);
    ssqe->opcode = IORING_OP_SEND;
    ssqe->fd = sv[1];
    ssqe->addr = reinterpret_cast<std::uint64_t>(msg);
    ssqe->len = mlen;
    ssqe->user_data = 2;

    ASSERT_GE(r.submit_and_wait(2), 1);

    int got_recv = -1, got_send = -1;
    for (int tries = 0; tries < 8 && (got_recv < 0 || got_send < 0); ++tries)
    {
        r.for_each_cqe([&](const io_uring_cqe* cqe) {
            if (cqe->user_data == 1) got_recv = cqe->res;
            else if (cqe->user_data == 2) got_send = cqe->res;
        });
        if (got_recv < 0 || got_send < 0) r.submit_and_wait(1);
    }

    EXPECT_EQ(got_send, static_cast<int>(mlen));
    EXPECT_EQ(got_recv, static_cast<int>(mlen));
    EXPECT_STREQ(rbuf, "hello-uring");

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST(Uring, SqFillsAndDrains)
{
    if (!available()) GTEST_SKIP();
    Ring r;
    ASSERT_TRUE(init_best(r, 8)); // 8 SQ slots

    // Fill the ring with NOPs until get_sqe() reports full, then drain.
    unsigned filled = 0;
    while (io_uring_sqe* sqe = r.get_sqe())
    {
        sqe->opcode = IORING_OP_NOP;
        sqe->user_data = filled++;
        if (filled > 1024) break; // safety
    }
    EXPECT_GE(filled, 8u) << "should fit at least the requested depth";

    ASSERT_GE(r.submit_and_wait(filled), 1);
    unsigned reaped = 0;
    for (int tries = 0; tries < 16 && reaped < filled; ++tries)
    {
        reaped += r.for_each_cqe([](const io_uring_cqe*) {});
        if (reaped < filled) r.submit_and_wait(1);
    }
    EXPECT_EQ(reaped, filled);

    // After draining, the ring is reusable.
    io_uring_sqe* again = r.get_sqe();
    EXPECT_NE(again, nullptr);
}

TEST(Uring, MultishotRecvWithProvidedBuffers)
{
    if (!available()) GTEST_SKIP();
    Ring r;
    ASSERT_TRUE(init_best(r, 16));
    BufRing br;
    if (!br.init(r, /*bgid=*/1, /*count=*/8, /*buf_size=*/64))
        GTEST_SKIP() << "provided-buffer rings unsupported on this kernel";

    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    // One multishot recv on sv[0], drawing buffers from group 1.
    io_uring_sqe* s = r.get_sqe();
    ASSERT_NE(s, nullptr);
    s->opcode = IORING_OP_RECV;
    s->fd = sv[0];
    s->addr = 0;
    s->len = 0;
    s->flags |= IOSQE_BUFFER_SELECT;
    s->buf_group = 1;
    s->ioprio |= IORING_RECV_MULTISHOT;
    s->user_data = 42;
    ASSERT_GE(r.submit(), 1);

    const char m1[] = "alpha";
    const char m2[] = "bravo";
    ASSERT_EQ(::write(sv[1], m1, 5), 5);
    ASSERT_EQ(::write(sv[1], m2, 5), 5);

    std::string seen;
    bool more_seen = false;
    for (int tries = 0; tries < 16 && (seen.find("alpha") == std::string::npos ||
                                       seen.find("bravo") == std::string::npos);
         ++tries)
    {
        r.submit_and_wait(1);
        r.for_each_cqe([&](const io_uring_cqe* cqe) {
            if (cqe->user_data != 42 || cqe->res <= 0) return;
            EXPECT_TRUE(cqe->flags & IORING_CQE_F_BUFFER) << "recv must select a provided buffer";
            const unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            seen.append(br.data(bid), static_cast<size_t>(cqe->res));
            br.recycle(bid); // return the buffer to the pool
            if (cqe->flags & IORING_CQE_F_MORE) more_seen = true; // multishot stayed armed
        });
    }
    EXPECT_NE(seen.find("alpha"), std::string::npos);
    EXPECT_NE(seen.find("bravo"), std::string::npos);
    EXPECT_TRUE(more_seen) << "multishot recv should remain armed (F_MORE)";

    ::close(sv[0]);
    ::close(sv[1]);
}

#else // !LLMBRIDGE_HAVE_URING

TEST(Uring, SkippedNoHeader) { GTEST_SKIP() << "linux/io_uring.h not available at build time"; }

#endif
