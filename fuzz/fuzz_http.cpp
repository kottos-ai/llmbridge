// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// libFuzzer target for the hand-rolled HTTP/1.1 framer (net/http.hpp). The framer
// parses untrusted bytes off the socket on the hot path; it must never crash or
// over-read, and must return a bounded, self-consistent Message. Build (Clang):
//   cmake -B build-fuzz -DLLMBRIDGE_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target fuzz_http
//   ./build-fuzz/bin/fuzz_http -max_total_time=120

#include "net/http.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    llmbridge::http::Message m;
    const auto st =
        llmbridge::http::parse(std::string_view(reinterpret_cast<const char*>(data), size), m);

    // Structural invariants the framer must uphold for any input.
    if (st == llmbridge::http::ParseStatus::Complete)
    {
        assert(m.total_len == m.header_len + m.body_len);
        assert(m.body_len <= llmbridge::http::kMaxBodyLen);
        assert(m.total_len <= size);
    }
    return 0;
}
