// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Differential target: the header walker in net/http.hpp must frame every input
// exactly as the v0.70.0 framer frozen in http_legacy.hpp does. Kept for one release.

#include "http_legacy.hpp"
#include "net/http.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace
{
    namespace now = llmbridge::net::http;
    namespace old = llmbridge::net::http_legacy;

    bool same(now::FrameStatus a, old::FrameStatus b)
    {
        return static_cast<int>(a) == static_cast<int>(b);
    }

    bool same(const now::ResponseHead& a, const old::ResponseHead& b)
    {
        return a.header_len == b.header_len && a.status == b.status &&
               a.keep_alive == b.keep_alive && a.chunked == b.chunked &&
               a.event_stream == b.event_stream && a.has_content_length == b.has_content_length &&
               a.encoded == b.encoded && a.content_length == b.content_length &&
               static_cast<int>(a.quota_exhausted) == static_cast<int>(b.quota_exhausted) &&
               a.retry_after_s == b.retry_after_s;
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const std::string_view in(reinterpret_cast<const char*>(data), size);

    now::Message m1;
    old::Message m2;
    const auto r1 = now::parse_request(in, m1);
    const auto r2 = old::parse_request(in, m2);
    assert(same(r1, r2));
    if (r1 != now::FrameStatus::Error)
        assert(m1.header_len == m2.header_len && m1.body_len == m2.body_len &&
               m1.total_len == m2.total_len && m1.keep_alive == m2.keep_alive &&
               m1.encoded == m2.encoded && m1.http_1_1 == m2.http_1_1);

    now::ResponseHead h1;
    old::ResponseHead h2;
    const auto s1 = now::parse_response_head(in, h1);
    const auto s2 = old::parse_response_head(in, h2);
    assert(same(s1, s2));
    if (s1 == now::FrameStatus::Complete) assert(same(h1, h2));

    now::ResponseDecoder d1;
    old::ResponseDecoder d2;
    const auto p1 = now::parse_response(in, d1);
    const auto p2 = old::parse_response(in, d2);
    assert(same(p1.status, p2.status));
    if (p1.complete())
        assert(same(p1.head, p2.head) && p1.body == p2.body && p1.total_len == p2.total_len);

    // Names from the input itself, so the fuzzer can steer them onto real lines.
    const size_t colon = in.find(':');
    const std::string_view names[] = {"content-length", "x-api-key", "request-id",
                                      in.substr(0, colon == std::string_view::npos ? 8 : colon)};
    for (const std::string_view name : names)
    {
        const std::string_view a = now::find_header(in, name);
        const std::string_view b = old::find_header(in, name);
        assert(a.data() == b.data() && a.size() == b.size());
    }
    return 0;
}
