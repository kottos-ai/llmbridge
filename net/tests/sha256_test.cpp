// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
#include "net/sha256.hpp"

#include <gtest/gtest.h>

#include <string>

using llmbridge::net::hex;
using llmbridge::net::Sha256;

namespace
{
    std::string digest_of(std::string_view s) { return hex(Sha256::hash(s)); }
} // namespace

// The published vectors are the only thing that proves the constants above were
// transcribed correctly, so they come first and the rest of this file assumes them.
TEST(Sha256, MatchesThePublishedVectors)
{
    EXPECT_EQ(digest_of(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(digest_of("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(digest_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(digest_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256, AMillionAsAreTheLongVector)
{
    Sha256 s;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) s.update(chunk);
    EXPECT_EQ(hex(s.finish()),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// The whole point of the incremental interface: a caller feeding a body as it
// arrives must get the digest of the body, whatever the arrival pattern. Block
// boundaries are where a hand-rolled buffer goes wrong, so they are walked
// explicitly, never sampled.
TEST(Sha256, AnySplitGivesTheSameDigest)
{
    std::string msg;
    for (int i = 0; i < 300; ++i) msg.push_back(static_cast<char>('A' + i % 26));
    const std::string want = hex(Sha256::hash(msg));

    for (size_t cut = 0; cut <= msg.size(); ++cut)
    {
        Sha256 s;
        s.update(std::string_view(msg).substr(0, cut));
        s.update(std::string_view(msg).substr(cut));
        EXPECT_EQ(hex(s.finish()), want) << "split at " << cut;
    }
    for (size_t chunk : {1u, 7u, 63u, 64u, 65u, 127u, 128u})
    {
        Sha256 s;
        for (size_t i = 0; i < msg.size(); i += chunk)
            s.update(std::string_view(msg).substr(i, chunk));
        EXPECT_EQ(hex(s.finish()), want) << "chunked by " << chunk;
    }
}

// Lengths either side of every padding boundary, against digests from an independent
// implementation. 55 bytes leaves room for the length field, 56 does not and forces a
// second block, 64 is a whole block with nothing left over, and the same happens again
// at every multiple. Comparing the incremental path against the one-shot path would
// only prove they agree with each other, which a padding bug does too.
TEST(Sha256, LengthsAroundEveryPaddingBoundary)
{
    struct Case { size_t n; const char* want; };
    static constexpr Case kCases[] = {
        {   0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {   1, "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
        {  54, "675f28acc0b90a72d1c3a570fe83ac565555db358cf01826dc8eefb2bf7ca0f3"},
        {  55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {  56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {  57, "2fe741af801cc238602ac0ec6a7b0c3a8a87c7fc7d7f02a3fe03d1c12eac4d8f"},
        {  63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {  64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {  65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        { 111, "60780e9451bdc43cf4530ffc95cbb0c4eb24dae2c39f55f334d679e076c08065"},
        { 112, "09373f127d34e61dbbaa8bc4499c87074f2ddb10e1b465f506d7d70a15011979"},
        { 113, "13aaa9b5fb739cdb0e2af99d9ac0a409390adc4d1cb9b41f1ef94f8552060e92"},
        { 119, "da18797ed7c3a777f0847f429724a2d8cd5138e6ed2895c3fa1a6d39d18f7ec6"},
        { 120, "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c"},
        { 127, "92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976"},
        { 128, "471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
        { 129, "5099c6a56203f9687f7d33f4bfdf576d31dc91f6b695ecea38b2770c87631135"},
        { 191, "d280f473c251cb75c91880ea0eca2a2f1cda3152bef54a38c4a3aedad615c819"},
        { 192, "8b4a544837a1a0280fa8a7c82865c27a1064b3cc6281fda0753566b9bb104a87"},
        { 255, "857df204175f077a9986709897f00ee0bcc0449585248e4b42498337e9329999"},
        { 256, "5bc31b283cef0072274e97d74916552954c935794536cab632641e5ea071379d"},
        {1000, "4e4c294b331f7a2099a379bec34b9f9fc03dc46ab465d998f4d683da53487e6d"},
    };
    for (const Case& c : kCases)
    {
        std::string msg(c.n, '\0');
        for (size_t i = 0; i < c.n; ++i) msg[i] = static_cast<char>(i % 251);
        EXPECT_EQ(digest_of(msg), c.want) << "length " << c.n;
    }
}

TEST(Sha256, ResetsAfterFinishSoTheObjectIsReusable)
{
    Sha256 s;
    s.update("abc");
    EXPECT_EQ(hex(s.finish()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Reused, not read again: a second finish on an untouched object is the empty
    // message, which is a visible wrong answer and not stale state.
    EXPECT_EQ(hex(s.finish()),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    s.update("abc");
    EXPECT_EQ(hex(s.finish()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, TruncationTakesTheLeadingBytesBigEndian)
{
    const auto d = Sha256::hash("abc");
    EXPECT_EQ(Sha256::truncated(d), 0xba7816bf8f01cfeaull);
}

TEST(Sha256, EmptyAndNullUpdatesChangeNothing)
{
    Sha256 s;
    s.update(nullptr, 0);
    s.update(std::string_view{});
    s.update("abc");
    s.update(nullptr, 0);
    EXPECT_EQ(hex(s.finish()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
