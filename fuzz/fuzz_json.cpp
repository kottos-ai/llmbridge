// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// libFuzzer target for the hand-rolled JSON parser (provider/json.hpp). This is the
// highest-risk surface, since translate mode feeds client-controlled bytes into
// it. The parser must never crash, over-read, or overflow the stack on any input;
// it may only set ok=false. Build (Clang):
//   cmake -B build-fuzz -DLLMBRIDGE_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target fuzz_json
//   ./build-fuzz/bin/fuzz_json -max_total_time=120

#include "provider/json.hpp"
#include "provider/translate.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    bool ok = false;
    llmbridge::provider::json::Value v =
        llmbridge::provider::json::parse(std::string_view(reinterpret_cast<const char*>(data), size), ok);
    // Touch the result so nothing is optimised away. The invariant is "does not
    // crash / ASAN-clean on any input", plus the three below.
    if (ok && v.is_object()) (void)v.find("model");
    namespace json = llmbridge::provider::json;
    const std::string_view in(reinterpret_cast<const char*>(data), size);
    bool any = false;
    (void)json::parse(in, any, json::Keys::Any);
    assert(!ok || any); // Keys::Unique only ever refuses more
    // And the gateway's top-level walker agrees there is nothing repeated.
    assert(!ok || llmbridge::provider::top_level_key_check(in) !=
                      llmbridge::provider::KeyCheck::DuplicateKey);
    // Any bytes escape to one JSON string that decodes back to those bytes.
    std::string lit;
    json::append_escaped(lit, in);
    bool lit_ok = false;
    const json::Value s = json::parse(lit, lit_ok);
    assert(lit_ok && s.is_string() && json::unescape_string(s.sv) == in);
    (void)s;
    // The top-level readers the gateway runs on every body when a policy or sink is
    // installed, ahead of the parser. Same invariant: any bytes, no over-read.
    const std::string_view b(reinterpret_cast<const char*>(data), size);
    (void)llmbridge::provider::top_level_facts(b);
    (void)llmbridge::provider::top_level_key_check(b);
    (void)llmbridge::provider::model_of(b);
    (void)llmbridge::provider::wants_stream(b);
    (void)llmbridge::provider::stream_usage_of(b);
    (void)llmbridge::provider::reply_model(b);
    return 0;
}
