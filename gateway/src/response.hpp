// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The bytes a client is answered with when the gateway itself builds the reply:
// error envelopes, a relayed provider status, the SSE head, and the opt-in
// timing and usage headers.

#include <cstdint>
#include <string>
#include <string_view>

namespace llmbridge::detail
{
    // Client-facing SSE response head. The stream is close-delimited: the body
    // ends when we close the socket, so no client-side chunk framing is needed.
    constexpr std::string_view kSseHead =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    /// The same reply framed so the connection survives it.
    constexpr std::string_view kSseHeadChunked =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";

    void append_timing_headers(std::string& out, int64_t t0, int64_t gateway_us,
                               int64_t connect_us, int64_t upwrite_us,
                               int64_t upstream_us, const char* upstream_key, uint64_t seq,
                               int64_t client_upload_us);
    void append_usage_headers(std::string& out, std::string_view translated_body);
    std::string build_error(int code, const char* detail = nullptr);
    std::string sse_head_with_timing(std::string_view extra, std::string_view base);
    std::string build_http_status(int status, std::string_view reason, std::string_view body);
    const char* reason_for(int status);
} // namespace llmbridge::detail
