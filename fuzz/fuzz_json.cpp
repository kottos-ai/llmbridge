// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// libFuzzer target for the hand-rolled JSON parser (provider/json.hpp). This is the
// highest-risk surface, since translate mode feeds client-controlled bytes into
// it. The parser must NEVER crash, over-read, or overflow the stack on any input;
// it may only set ok=false. Build (Clang):
//   cmake -B build-fuzz -DLLMBRIDGE_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target fuzz_json
//   ./build-fuzz/bin/fuzz_json -max_total_time=120

#include "provider/json.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    bool ok = false;
    llmbridge::provider::json::Value v =
        llmbridge::provider::json::parse(std::string_view(reinterpret_cast<const char*>(data), size), ok);
    // Touch the result so nothing is optimised away; no assertion. The invariant
    // is simply "does not crash / ASAN-clean on any input".
    if (ok && v.is_object()) (void)v.find("model");
    return 0;
}
