// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Minimal HTTP/1.1 message framing for the llmbridge proxy hot path.
//
// Scope (Phase A): just enough to (a) know when a full request/response has
// arrived in a buffer, and (b) decide keep-alive. Content-Length framing only.
// Because we frame solely by Content-Length, any `Transfer-Encoding` header is
// REJECTED rather than ignored — ignoring it (while an upstream honours it) is
// the classic request-smuggling desync, dangerous with pooled upstreams. A
// conflicting duplicate Content-Length is likewise rejected (RFC 9112 §6). No
// allocation, no copy: everything is index math over a caller-owned buffer — a
// zero-alloc, hand-written framer.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace llmbridge::http
{
    struct Message
    {
        size_t header_len = 0; // bytes up to and including the CRLFCRLF
        size_t body_len = 0;   // Content-Length value (0 if absent)
        size_t total_len = 0;  // header_len + body_len — full message size
        bool keep_alive = true;
    };

    enum class ParseStatus
    {
        NeedMore, // headers not fully buffered, or body still arriving
        Complete, // a full message is present in buf[0, total_len)
        Error     // malformed (header too large, bad Content-Length)
    };

    namespace detail
    {
        // ASCII lower-case, branch-light.
        constexpr char lc(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }

        // Case-insensitive: does the header line starting at `line` begin with
        // `name` (which must be given lower-case, including the trailing ':')?
        inline bool line_is(std::string_view line, std::string_view name) noexcept
        {
            if (line.size() < name.size()) return false;
            for (size_t i = 0; i < name.size(); ++i)
                if (lc(line[i]) != name[i]) return false;
            return true;
        }

        // Trim leading spaces/tabs from a header value.
        inline std::string_view ltrim(std::string_view v) noexcept
        {
            size_t i = 0;
            while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
            return v.substr(i);
        }
    } // namespace detail

    // Cap on header section size — a guard against an unbounded slow-loris
    // style buffer growth. 32 KiB is comfortably above any sane chat-completion
    // request/response header block.
    inline constexpr size_t kMaxHeaderLen = 32 * 1024;

    // Cap on Content-Length. Without this, a `Content-Length: 9999999999` header
    // followed by a slow byte trickle grows the receive buffer without bound —
    // a trivial memory-exhaustion DoS. 16 MiB is far above any chat-completion
    // body (including base64 vision images); genuine large/streaming payloads are
    // a Phase C concern with their own backpressure.
    inline constexpr size_t kMaxBodyLen = 16 * 1024 * 1024;

    // Parse framing info out of `buf`. Idempotent and cheap to re-run as more
    // bytes arrive: returns NeedMore until the full message is buffered.
    inline ParseStatus parse(std::string_view buf, Message& out) noexcept
    {
        // Locate end of header block (CRLF CRLF).
        const size_t hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string_view::npos)
        {
            if (buf.size() > kMaxHeaderLen) return ParseStatus::Error;
            return ParseStatus::NeedMore;
        }
        out.header_len = hdr_end + 4;
        if (out.header_len > kMaxHeaderLen) return ParseStatus::Error;

        // Walk header lines after the request/status line, pulling the two
        // headers we care about. Default keep-alive for HTTP/1.1.
        out.body_len = 0;
        out.keep_alive = true;
        bool have_cl = false; // to detect a conflicting duplicate Content-Length
        std::string_view headers = buf.substr(0, hdr_end);

        size_t pos = headers.find("\r\n");
        if (pos == std::string_view::npos) pos = headers.size(); // no headers
        while (pos < headers.size())
        {
            size_t start = pos + 2;
            size_t eol = headers.find("\r\n", start);
            if (eol == std::string_view::npos) eol = headers.size();
            std::string_view line = headers.substr(start, eol - start);

            if (detail::line_is(line, "content-length:"))
            {
                std::string_view v = detail::ltrim(line.substr(15));
                size_t n = 0;
                auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
                if (ec != std::errc{}) return ParseStatus::Error;
                // Conflicting duplicate Content-Length → reject (smuggling vector,
                // RFC 9112 §6.3). An identical repeat is harmless; collapse it.
                if (have_cl && n != out.body_len) return ParseStatus::Error;
                out.body_len = n;
                have_cl = true;
            }
            else if (detail::line_is(line, "transfer-encoding:"))
            {
                // We frame by Content-Length only; refuse TE outright rather than
                // risk a TE/CL desync against a TE-honouring upstream.
                return ParseStatus::Error;
            }
            else if (detail::line_is(line, "connection:"))
            {
                std::string_view v = detail::ltrim(line.substr(11));
                // Only "close" flips the default; "keep-alive" is the default.
                if (v.size() >= 5 && detail::line_is(v, "close")) out.keep_alive = false;
            }
            pos = eol;
        }

        // Reject a hostile / absurd body length before we ever buffer toward it.
        if (out.body_len > kMaxBodyLen) return ParseStatus::Error;

        out.total_len = out.header_len + out.body_len;
        if (buf.size() < out.total_len) return ParseStatus::NeedMore;
        return ParseStatus::Complete;
    }
} // namespace llmbridge::http