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

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
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

    // Case-insensitive single-header lookup over a raw header block (the bytes
    // between the start line's CRLF and the terminating CRLFCRLF, or the whole
    // message — start line simply never matches a `name:` prefix). Returns the
    // FIRST occurrence's value, left-trimmed, or empty if absent. Zero-copy: the
    // view aliases `headers`.
    //
    // First-wins is a deliberate anti-smuggling stance: when a client sends a
    // duplicated header, the copies must not be interpreted differently by us
    // and by the upstream. We take the first and, because the gateway REBUILDS
    // the upstream request from a whitelist rather than echoing the block, the
    // duplicate never travels.
    //
    // ⚠ The returned value is NOT safe to re-emit as-is. Splitting on CRLF leaves
    // a BARE CR (or any other control byte) inside the value — a lenient parser
    // that treats bare CR as a line terminator then sees an injected header. An
    // earlier revision of this comment claimed the opposite; it was wrong, and the
    // injection was measured reaching an upstream. Callers that re-emit a value
    // MUST validate its charset first (see header_value_safe in gateway.cpp).
    // `name` must be LOWERCASE and INCLUDE the trailing colon ("x-api-key:") —
    // same convention as the framer's own header matching, and the colon is what
    // stops "x-api-key-2:" from prefix-matching "x-api-key".
    inline std::string_view find_header(std::string_view headers, std::string_view name) noexcept
    {
        size_t start = 0;
        while (start < headers.size())
        {
            size_t eol = headers.find("\r\n", start);
            if (eol == std::string_view::npos) eol = headers.size();
            const std::string_view line = headers.substr(start, eol - start);
            if (detail::line_is(line, name))
                return detail::ltrim(line.substr(name.size()));
            start = eol + 2;
        }
        return {};
    }

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

    // ── Streaming response support (Phase B) ────────────────────────────────
    //
    // SSE responses (Anthropic `stream:true`) are framed by chunked
    // transfer-encoding and carry no Content-Length, so the strict `parse()`
    // above (Content-Length only, TE rejected) cannot read them. The two helpers
    // below add exactly what the gateway's *response* path needs to pump a stream
    // — and ONLY the response path: client-request framing stays strict, so the
    // anti-smuggling posture is unchanged. A trusted upstream sending chunked is
    // not a smuggling vector the way an untrusted client would be.

    struct ResponseHead
    {
        size_t header_len = 0;         // bytes up to and including CRLFCRLF
        int status = 0;                // e.g. 200
        bool keep_alive = true;
        bool chunked = false;          // Transfer-Encoding: chunked
        bool event_stream = false;     // Content-Type: text/event-stream
        bool has_content_length = false;
        size_t content_length = 0;
    };

    enum class HeadStatus { NeedMore, Ok, Error };

    namespace detail
    {
        // Case-insensitive substring search (small, for header values).
        inline bool contains_ci(std::string_view hay, std::string_view needle) noexcept
        {
            if (needle.empty() || hay.size() < needle.size()) return needle.empty();
            for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
            {
                size_t j = 0;
                for (; j < needle.size(); ++j)
                    if (lc(hay[i + j]) != needle[j]) break;
                if (j == needle.size()) return true;
            }
            return false;
        }
    } // namespace detail

    // Parse a RESPONSE header block. Unlike parse(), this tolerates chunked
    // transfer-encoding (needed for SSE) and reports how the body is framed so the
    // caller can choose whole-body vs streaming handling. Returns NeedMore until
    // the CRLFCRLF is buffered.
    inline HeadStatus parse_response_head(std::string_view buf, ResponseHead& out) noexcept
    {
        const size_t hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string_view::npos)
            return buf.size() > kMaxHeaderLen ? HeadStatus::Error : HeadStatus::NeedMore;
        out.header_len = hdr_end + 4;
        if (out.header_len > kMaxHeaderLen) return HeadStatus::Error;

        std::string_view headers = buf.substr(0, hdr_end);

        // Status line: "HTTP/1.1 <code> <reason>". Pull the 3-digit code.
        size_t sp = headers.find(' ');
        if (sp != std::string_view::npos)
        {
            std::string_view rest = detail::ltrim(headers.substr(sp + 1));
            int code = 0;
            size_t k = 0;
            for (; k < rest.size() && k < 3 && rest[k] >= '0' && rest[k] <= '9'; ++k)
                code = code * 10 + (rest[k] - '0');
            if (k == 3) out.status = code;
        }

        size_t pos = headers.find("\r\n");
        if (pos == std::string_view::npos) pos = headers.size();
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
                if (ec == std::errc{}) { out.content_length = n; out.has_content_length = true; }
            }
            else if (detail::line_is(line, "transfer-encoding:"))
            {
                if (detail::contains_ci(line.substr(18), "chunked")) out.chunked = true;
            }
            else if (detail::line_is(line, "content-type:"))
            {
                if (detail::contains_ci(line.substr(13), "text/event-stream")) out.event_stream = true;
            }
            else if (detail::line_is(line, "connection:"))
            {
                std::string_view v = detail::ltrim(line.substr(11));
                if (v.size() >= 5 && detail::line_is(v, "close")) out.keep_alive = false;
            }
            pos = eol;
        }
        if (out.has_content_length && out.content_length > kMaxBodyLen) return HeadStatus::Error;
        return HeadStatus::Ok;
    }

    // Incremental HTTP/1.1 chunked-transfer decoder. Feed body bytes as they
    // arrive (a chunk header or its data may split across reads); it appends the
    // decoded payload to `out` and tracks completion (the terminating 0-length
    // chunk). Stateful — hold one per streamed response. Bounded: an absurd chunk
    // size or size-line is a hard error, not unbounded growth.
    class ChunkDecoder
    {
    public:
        // Append decoded bytes from `in` to `out`. Returns false on a malformed
        // stream (the caller should drop the connection); true otherwise. Extra
        // bytes fed after done() are ignored.
        bool feed(std::string_view in, std::string& out) noexcept
        {
            size_t i = 0;
            while (i < in.size() && _st != St::Done && _st != St::Error)
            {
                switch (_st)
                {
                    case St::Size:
                        while (i < in.size())
                        {
                            const char c = in[i++];
                            _line += c;
                            if (_line.size() > 64) return fail(); // absurd chunk-size line
                            if (_line.size() >= 2 && _line[_line.size() - 2] == '\r' && _line.back() == '\n')
                            {
                                size_t sz = 0;
                                bool any = false;
                                for (char h : _line)
                                {
                                    int d;
                                    if (h >= '0' && h <= '9') d = h - '0';
                                    else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                                    else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                                    else break; // ';' extension or '\r'
                                    sz = sz * 16 + static_cast<size_t>(d);
                                    any = true;
                                    if (sz > kMaxBodyLen) return fail(); // hostile chunk size
                                }
                                _line.clear();
                                if (!any) return fail();
                                _remaining = sz;
                                _st = (sz == 0) ? St::Trailer : St::Data;
                                break;
                            }
                        }
                        break;
                    case St::Data:
                    {
                        const size_t take = std::min(_remaining, in.size() - i);
                        out.append(in.data() + i, take);
                        i += take;
                        _remaining -= take;
                        if (_remaining == 0) _st = St::DataCRLF;
                        break;
                    }
                    case St::DataCRLF:
                        while (i < in.size() && _st == St::DataCRLF)
                        {
                            _line += in[i++];
                            if (_line.size() == 2) { if (_line == "\r\n") { _line.clear(); _st = St::Size; } else return fail(); }
                        }
                        break;
                    case St::Trailer: // 0-chunk trailers: read lines until a blank line
                        while (i < in.size() && _st == St::Trailer)
                        {
                            const char c = in[i++];
                            _line += c;
                            if (_line.size() > kMaxHeaderLen) return fail();
                            if (_line.size() >= 2 && _line[_line.size() - 2] == '\r' && _line.back() == '\n')
                            {
                                const bool blank = _line.size() == 2;
                                _line.clear();
                                if (blank) { _st = St::Done; }
                            }
                        }
                        break;
                    default:
                        break;
                }
            }
            return _st != St::Error;
        }

        [[nodiscard]] bool done() const noexcept { return _st == St::Done; }
        [[nodiscard]] bool failed() const noexcept { return _st == St::Error; }

    private:
        enum class St { Size, Data, DataCRLF, Trailer, Done, Error };
        bool fail() noexcept { _st = St::Error; return false; }

        St _st = St::Size;
        size_t _remaining = 0; // bytes left in the current chunk's data
        std::string _line;     // accumulates a size/CRLF/trailer line across reads
    };
} // namespace llmbridge::http