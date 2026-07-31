// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// secure_clear correctness.
//
// NOTE ON WHAT THESE CAN AND CANNOT PROVE. A unit test can show the bytes are
// zero, but it CANNOT show the store survives dead-store elimination — reading
// the buffer afterwards is exactly what stops the compiler eliding it. The
// no-elision property is verified by inspecting generated assembly (see
// net/secure.hpp for the command) and by the platform primitive's own contract.
// Do not "strengthen" these tests into a false proof of that property.

#include "net/secure.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using llmbridge::net::secure_clear;

TEST(SecureClear, ZeroesRawBuffer)
{
    std::vector<unsigned char> buf(64, 0xAB);
    secure_clear(buf.data(), buf.size());
    for (const unsigned char c : buf) EXPECT_EQ(c, 0u);
}

TEST(SecureClear, ZeroesStringContentsAndClears)
{
    std::string s = "sk-super-secret-key-value";
    const char* const data = s.data(); // capacity is retained, so this stays valid
    const size_t n = s.size();
    secure_clear(s);
    EXPECT_TRUE(s.empty());
    // The bytes behind the (still-owned) allocation must be zero, not the key.
    for (size_t i = 0; i < n; ++i) EXPECT_EQ(data[i], '\0') << "byte " << i;
}

TEST(SecureClear, HandlesEmptyAndNull)
{
    std::string empty;
    secure_clear(empty); // must not crash or touch anything
    EXPECT_TRUE(empty.empty());
    secure_clear(nullptr, 0);
    char c = 'x';
    secure_clear(&c, 0); // zero length must be a no-op, not a 1-byte write
    EXPECT_EQ(c, 'x');
}

TEST(SecureClear, LargeBufferFullyCleared)
{
    std::string s(64 * 1024, 'K');
    const char* const data = s.data();
    const size_t n = s.size();
    secure_clear(s);
    size_t nonzero = 0;
    for (size_t i = 0; i < n; ++i)
        if (data[i] != '\0') ++nonzero;
    EXPECT_EQ(nonzero, 0u);
}
