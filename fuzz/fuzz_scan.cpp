// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// libFuzzer target for the bounded scans over provider bytes (provider/openai.hpp):
// usage, the error type and one string, run on every response the gateway forwards.
// Beyond no crash and no UB, it asserts that a stream's usage does not depend on how
// its bytes were split into reads.

#include "provider/openai.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace
{
    inline void must(bool cond)
    {
        if (!cond) __builtin_trap();
    }

    bool same(const llmbridge::provider::openai::Usage& a,
              const llmbridge::provider::openai::Usage& b)
    {
        return a.in == b.in && a.out == b.out && a.cached == b.cached &&
               a.cache_write == b.cache_write && a.cache_write_5m == b.cache_write_5m &&
               a.cache_write_1h == b.cache_write_1h && a.reasoning == b.reasoning &&
               a.audio_in == b.audio_in && a.audio_out == b.audio_out &&
               a.accepted_prediction == b.accepted_prediction &&
               a.rejected_prediction == b.rejected_prediction && a.tool_prompt == b.tool_prompt;
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    namespace oai = llmbridge::provider::openai;
    const std::string_view in(reinterpret_cast<const char*>(data), size);
    (void)oai::scan_usage(in);
    (void)oai::scan_usage(in, 0);
    (void)oai::scan_string(in, "\"service_tier\"", true);
    (void)oai::scan_string(in, "\"service_tier\"", false);
    (void)oai::scan_error_type(in);

    oai::StreamUsage whole;
    whole.feed(in);
    oai::StreamUsage split;
    const size_t parts = size < 16 ? size : 16;
    size_t off = 0;
    for (size_t k = 0; k < parts; ++k)
    {
        const size_t end = (k + 1 == parts) ? size : (size * (k + 1)) / parts;
        split.feed(in.substr(off, end - off));
        off = end;
    }
    // An object past the carry cap is dropped by whichever run sees it unfinished.
    if (size < 32 * 1024) must(same(whole.usage(), split.usage()));
    return 0;
}
