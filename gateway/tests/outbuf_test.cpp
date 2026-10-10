// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// OutBuf: the bytes owed to a socket and the pin an io_uring send holds on them.

#include "core/outbuf.hpp"

#include <gtest/gtest.h>

#include <string>

using llmbridge::OutBuf;

TEST(OutBuf, StagedBytesGoOutInOrder)
{
    OutBuf b;
    b.stage() = "hello ";
    b.stage() += "world";
    EXPECT_EQ(b.wire(), "hello world");
    b.sent(6);
    EXPECT_EQ(b.wire(), "world");
    EXPECT_FALSE(b.idle());
    b.sent(5);
    EXPECT_TRUE(b.idle());
    EXPECT_EQ(b.bytes(), "hello world") << "the sent bytes stay, for a resend";
}

// The front a send reads never moves: what is staged meanwhile waits behind it.
TEST(OutBuf, APinnedFrontIsNeverWritten)
{
    OutBuf b;
    b.stage() = "first";
    b.pin();
    const char* kernel_reads = b.wire().data();
    b.stage() += std::string(1 << 20, 'x'); // would reallocate the front
    EXPECT_EQ(b.wire().data(), kernel_reads);
    EXPECT_EQ(b.wire(), "first");
    EXPECT_EQ(b.unsent(), 5u + (1u << 20));
    b.sent(5);
    EXPECT_FALSE(b.pinned());
    EXPECT_EQ(b.wire().size(), 1u << 20) << "the staged half folds in once the send is done";
}

TEST(OutBuf, APartialSendKeepsOrderWithWhatStagedBehindIt)
{
    OutBuf b;
    b.stage() = "abcdef";
    b.pin();
    b.stage() += "ghi";
    b.sent(2);
    EXPECT_EQ(b.wire(), "cdefghi");
    b.stage() += "jk";
    EXPECT_EQ(b.wire(), "cdefghijk");
}

// A drained front is reset by the next stage, keeping its capacity: a stream never
// appends behind bytes already on the wire, and never reallocates to do so.
TEST(OutBuf, ADrainedFrontIsReusedNotGrown)
{
    OutBuf b;
    b.stage().assign(4096, 'a');
    const size_t cap = b.capacity();
    b.sent(4096);
    b.stage() += "next";
    EXPECT_EQ(b.wire(), "next");
    EXPECT_EQ(b.bytes(), "next");
    EXPECT_EQ(b.capacity(), cap);
}

// L11: a retry takes the request while a send may still read it. Moving it then
// hands the kernel freed memory; take() copies while pinned.
TEST(OutBuf, TakeCopiesWhilePinnedAndMovesOtherwise)
{
    OutBuf b;
    b.stage() = std::string(1000, 'r');
    b.pin();
    const char* kernel_reads = b.wire().data();
    const std::string copy = b.take();
    EXPECT_EQ(copy, std::string(1000, 'r'));
    EXPECT_EQ(b.wire().data(), kernel_reads) << "the pinned front was moved out from under a send";
    EXPECT_EQ(b.wire().size(), 1000u);

    OutBuf u;
    u.stage() = std::string(1000, 'q');
    const char* storage = u.bytes().data();
    u.sent(400);
    const std::string moved = u.take();
    EXPECT_EQ(moved.data(), storage) << "an unpinned take moves, and takes all of it";
    EXPECT_EQ(moved.size(), 1000u);
    EXPECT_TRUE(u.idle());
}

TEST(OutBuf, ScrubErasesBothHalvesAndKeepsCapacity)
{
    OutBuf b;
    b.stage() = std::string(100, 'k');
    b.pin();
    b.stage() += std::string(100, 'k');
    b.sent(100);
    b.scrub();
    EXPECT_TRUE(b.idle());
    EXPECT_TRUE(b.bytes().empty());
    EXPECT_GE(b.capacity(), 100u);
}

TEST(OutBuf, AdoptTakesStorageNotBytes)
{
    std::string warm(1 << 16, 'w');
    const char* pages = warm.data();
    OutBuf b;
    b.adopt(std::move(warm));
    EXPECT_TRUE(b.idle());
    EXPECT_EQ(b.bytes().data(), pages);
    EXPECT_GE(b.capacity(), 1u << 16);
}
