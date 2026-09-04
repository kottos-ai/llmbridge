// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Everything between a framed client request and the bytes for a venue: dialect
// resolution, the rebuild of a byte-forwarded request, the translated request with
// its credential headers, and the response translated back. The credential path
// is in here, in one file.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/dialect.hpp"
#include "gateway/gateway.hpp"

namespace llmbridge::detail
{
    // Log-friendly name for the dialect. Kept next to the log sites, not on
    // UpstreamDialect itself: the enum is part of the public API and does not need
    // a printing concern attached to it.
    /// [[maybe_unused]] because this is called only from an LB_DEBUG line, and at
    /// the default info floor that line compiles to nothing, leaving an
    /// unreferenced static. Clang makes that fatal under -Werror
    /// (-Wunneeded-internal-declaration); GCC says nothing, which is how it
    /// reached CI. Any helper used only from a compiled-out log line needs this.
    ///
    /// The request line ("POST /v1/chat/completions HTTP/1.1") for a log line,
    /// bounded. Safe to log: the start line carries no credential, unlike every
    /// header after it. Truncated hard so a hostile long URI cannot dominate the
    /// log, and cut at the first CR so it can never run into a header.
    inline std::string_view request_line(std::string_view buf) noexcept
    {
        constexpr size_t kMax = 128;
        const size_t eol = buf.find('\r');
        size_t n = eol == std::string_view::npos ? buf.size() : eol;
        if (n > kMax) n = kMax;
        return buf.substr(0, n);
    }

    /// Status code off the front of a response we are about to send. Reads the
    /// buffer we built ourselves, so the "HTTP/1.x NNN" shape is guaranteed;
    /// returns 0 if it is not, which is worth seeing in a log instead of hiding.
    inline int status_of(std::string_view resp) noexcept
    {
        if (resp.size() < 12 || resp.compare(0, 5, "HTTP/") != 0) return 0;
        const char a = resp[9], b = resp[10], c = resp[11];
        if (a < '0' || a > '9' || b < '0' || b > '9' || c < '0' || c > '9') return 0;
        return (a - '0') * 100 + (b - '0') * 10 + (c - '0');
    }

    [[nodiscard]] inline int64_t span_since(int64_t start, int64_t end) noexcept
    {
        return (start > 0 && end >= start) ? end - start : 0;
    }

    constexpr std::string_view kContinue = "HTTP/1.1 100 Continue\r\n\r\n";

    TranslationPlan resolve_dialect(const Connection* c, const Upstream& venue) noexcept;
    const char* translate_failure(std::string_view body) noexcept;
    const char* dialect_name(UpstreamDialect m) noexcept;
    std::string build_http(std::string_view start_line, std::string_view body,
                           std::string_view extra = {});
    std::string build_http_request(std::string_view start_line, std::string_view body,
                                   std::string_view host, std::string_view extra);
    [[nodiscard]] bool expects_continue(std::string_view head) noexcept;
    std::string request_without(std::string_view msg, size_t header_len,
                                const std::vector<std::string>& strip,
                                std::string_view host_hdr, std::string_view base_path,
                                std::string_view body_override = {});
    bool build_translated_request(const Upstream& up, UpstreamDialect mode,
                                  std::string_view body, std::string_view client_hdrs,
                                  const std::vector<std::string>& strip,
                                  std::string& out, const char*& why,
                                  std::string_view model_override = {},
                                  bool* wants_stream_usage = nullptr);
    std::string xlate_resp(UpstreamDialect mode, std::string_view body);
    std::string host_header_for(const Upstream& u);
    std::string aws_region_for(const Upstream& u);
    std::string peer_of(int fd) noexcept;
} // namespace llmbridge::detail
