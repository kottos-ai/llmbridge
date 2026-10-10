// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Frozen copy of net/http.hpp as of v0.70.0, the reference fuzz_http_diff compares
// the header walker against. Kept for one release; do not edit.

#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace llmbridge::net::http_legacy
{
    struct Message
    {
        size_t header_len = 0; // bytes up to and including the CRLFCRLF
        size_t body_len = 0;   // Content-Length value (0 if absent)
        size_t total_len = 0;  // header_len + body_len: full message size
        bool keep_alive = true;
        bool encoded = false;  // Content-Encoding present and not identity
        bool http_1_1 = true;
    };

    enum class FrameStatus
    {
        NeedMore, // not fully buffered yet: feed more bytes and re-run
        Complete, // the thing this call frames is fully present
        Error     // malformed; refuse the message (see the fail-closed policy)
    };

    namespace detail
    {
        constexpr char lc(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }

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

        inline bool line_is(std::string_view line, std::string_view name) noexcept
        {
            if (line.size() < name.size()) return false;
            for (size_t i = 0; i < name.size(); ++i)
                if (lc(line[i]) != name[i]) return false;
            return true;
        }

        inline std::string_view ltrim(std::string_view v) noexcept
        {
            size_t i = 0;
            while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
            return v.substr(i);
        }

        inline std::string_view trim_ows(std::string_view v) noexcept
        {
            size_t b = 0, e = v.size();
            while (b < e && (v[b] == ' ' || v[b] == '\t')) ++b;
            while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t')) --e;
            return v.substr(b, e - b);
        }

        inline bool parse_strict_length(std::string_view v, size_t& out) noexcept
        {
            v = trim_ows(v);
            if (v.empty()) return false;
            for (const char c : v)
                if (c < '0' || c > '9') return false;
            size_t n = 0;
            const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
            if (ec != std::errc{} || p != v.data() + v.size()) return false; // overflow
            out = n;
            return true;
        }

        inline bool is_zero_count(std::string_view v) noexcept
        {
            size_t n = 0;
            return parse_strict_length(v, n) && n == 0;
        }

        inline bool block_line_endings_ok(std::string_view h) noexcept
        {
            for (size_t i = 0; i < h.size(); ++i)
            {
                if (h[i] == '\r' && (i + 1 >= h.size() || h[i + 1] != '\n')) return false;
                if (h[i] == '\n' && (i == 0 || h[i - 1] != '\r')) return false;
            }
            return true;
        }

        inline bool line_ok(std::string_view line, size_t& colon) noexcept
        {
            if (line.empty()) return false;
            if (line[0] == ' ' || line[0] == '\t') return false; // obs-fold
            colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) return false;
            return !(line[colon - 1] == ' ' || line[colon - 1] == '\t');
        }
    } // namespace detail

    inline std::string_view find_header(std::string_view headers, std::string_view name) noexcept
    {
        if (!name.empty() && name.back() == ':') name.remove_suffix(1);
        if (name.empty()) return {};
        size_t start = 0;
        while (start < headers.size())
        {
            size_t eol = headers.find("\r\n", start);
            if (eol == std::string_view::npos) eol = headers.size();
            const std::string_view line = headers.substr(start, eol - start);
            if (line.empty()) return {};
            if (line.size() > name.size() && line[name.size()] == ':')
            {
                bool same = true;
                for (size_t i = 0; i < name.size() && same; ++i)
                    same = detail::lc(line[i]) == detail::lc(name[i]);
                if (same) return detail::ltrim(line.substr(name.size() + 1));
            }
            start = eol + 2;
        }
        return {};
    }

    inline constexpr size_t kMaxHeaderLen = 32 * 1024;

    inline constexpr size_t kMaxBodyLen = 16 * 1024 * 1024;

    inline FrameStatus parse_request(std::string_view buf, Message& out) noexcept
    {
        const size_t hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string_view::npos)
        {
            if (buf.size() > kMaxHeaderLen) return FrameStatus::Error;
            return FrameStatus::NeedMore;
        }
        out.header_len = hdr_end + 4;
        if (out.header_len > kMaxHeaderLen) return FrameStatus::Error;

        out.body_len = 0;
        out.keep_alive = true;
        {
            const size_t rl_end = buf.find("\r\n");
            const std::string_view rl = buf.substr(0, rl_end == std::string_view::npos ? 0 : rl_end);
            const size_t sp = rl.rfind(' ');
            const std::string_view ver = sp == std::string_view::npos ? std::string_view{}
                                                                     : rl.substr(sp + 1);
            out.http_1_1 = ver == "HTTP/1.1";
            if (ver == "HTTP/1.0") out.keep_alive = false;
        }
        bool have_cl = false; // to detect a conflicting duplicate Content-Length
        std::string_view headers = buf.substr(0, hdr_end);

        if (!detail::block_line_endings_ok(headers)) return FrameStatus::Error;

        size_t pos = headers.find("\r\n");
        if (pos == std::string_view::npos) pos = headers.size(); // no headers
        while (pos < headers.size())
        {
            size_t start = pos + 2;
            size_t eol = headers.find("\r\n", start);
            if (eol == std::string_view::npos) eol = headers.size();
            std::string_view line = headers.substr(start, eol - start);

            size_t colon = 0;
            if (!detail::line_ok(line, colon)) return FrameStatus::Error;
            const std::string_view value = line.substr(colon + 1);

            if (detail::line_is(line, "content-length:"))
            {
                size_t n = 0;
                if (!detail::parse_strict_length(value, n)) return FrameStatus::Error;
                if (have_cl && n != out.body_len) return FrameStatus::Error;
                out.body_len = n;
                have_cl = true;
            }
            else if (detail::line_is(line, "transfer-encoding:"))
            {
                return FrameStatus::Error;
            }
            else if (detail::line_is(line, "content-encoding:"))
            {
                const std::string_view v = detail::ltrim(value);
                out.encoded = !v.empty() && !detail::contains_ci(v, "identity");
            }
            else if (detail::line_is(line, "connection:"))
            {
                std::string_view v = detail::ltrim(value);
                if (v.size() >= 5 && detail::line_is(v, "close")) out.keep_alive = false;
                else if (v.size() >= 10 && detail::line_is(v, "keep-alive")) out.keep_alive = true;
            }
            pos = eol;
        }

        if (out.body_len > kMaxBodyLen) return FrameStatus::Error;

        out.total_len = out.header_len + out.body_len;
        if (buf.size() < out.total_len) return FrameStatus::NeedMore;
        return FrameStatus::Complete;
    }

    struct ResponseHead
    {
        size_t header_len = 0;         // bytes up to and including CRLFCRLF
        int status = 0;                // e.g. 200
        bool keep_alive = true;
        bool chunked = false;          // Transfer-Encoding: chunked
        bool event_stream = false;     // Content-Type: text/event-stream
        bool has_content_length = false;
        bool encoded = false;          // Content-Encoding present and not identity
        size_t content_length = 0;
        enum class Quota : uint8_t { None = 0, Requests = 1, InputTokens = 2,
                                     OutputTokens = 3, Tokens = 4 };
        Quota quota_exhausted = Quota::None;
        uint16_t retry_after_s = 0;
    };

    inline FrameStatus parse_response_head(std::string_view buf, ResponseHead& out) noexcept
    {
        const size_t hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string_view::npos)
            return buf.size() > kMaxHeaderLen ? FrameStatus::Error : FrameStatus::NeedMore;
        out.header_len = hdr_end + 4;
        if (out.header_len > kMaxHeaderLen) return FrameStatus::Error;

        std::string_view headers = buf.substr(0, hdr_end);

        if (headers.size() < 9 || headers.compare(0, 7, "HTTP/1.") != 0 ||
            (headers[7] != '0' && headers[7] != '1') || headers[8] != ' ')
            return FrameStatus::Error;

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

        if (!detail::block_line_endings_ok(headers)) return FrameStatus::Error;

        size_t pos = headers.find("\r\n");
        if (pos == std::string_view::npos) pos = headers.size();
        while (pos < headers.size())
        {
            size_t start = pos + 2;
            size_t eol = headers.find("\r\n", start);
            if (eol == std::string_view::npos) eol = headers.size();
            std::string_view line = headers.substr(start, eol - start);

            size_t colon = 0;
            if (!detail::line_ok(line, colon)) return FrameStatus::Error;
            const std::string_view value = line.substr(colon + 1);

            if (detail::line_is(line, "content-length:"))
            {
                size_t n = 0;
                if (!detail::parse_strict_length(value, n)) return FrameStatus::Error;
                if (out.has_content_length && n != out.content_length) return FrameStatus::Error;
                out.content_length = n;
                out.has_content_length = true;
            }
            else if (detail::line_is(line, "retry-after:"))
            {
                size_t n = 0;
                if (detail::parse_strict_length(value, n))
                    out.retry_after_s = n > 65535 ? 65535 : static_cast<uint16_t>(n);
            }
            else if (detail::line_is(line, "anthropic-ratelimit-requests-remaining:") ||
                     detail::line_is(line, "x-ratelimit-remaining-requests:"))
            {
                if (detail::is_zero_count(value))
                    out.quota_exhausted = ResponseHead::Quota::Requests;
            }
            else if (detail::line_is(line, "anthropic-ratelimit-input-tokens-remaining:"))
            {
                if (detail::is_zero_count(value))
                    out.quota_exhausted = ResponseHead::Quota::InputTokens;
            }
            else if (detail::line_is(line, "anthropic-ratelimit-output-tokens-remaining:"))
            {
                if (detail::is_zero_count(value))
                    out.quota_exhausted = ResponseHead::Quota::OutputTokens;
            }
            else if (detail::line_is(line, "x-ratelimit-remaining-tokens:"))
            {
                if (detail::is_zero_count(value))
                    out.quota_exhausted = ResponseHead::Quota::Tokens;
            }
            else if (detail::line_is(line, "transfer-encoding:"))
            {
                if (detail::contains_ci(value, "chunked")) out.chunked = true;
            }
            else if (detail::line_is(line, "content-encoding:"))
            {
                const std::string_view v = detail::ltrim(value);
                out.encoded = !v.empty() && !detail::contains_ci(v, "identity");
            }
            else if (detail::line_is(line, "content-type:"))
            {
                if (detail::contains_ci(value, "text/event-stream")) out.event_stream = true;
            }
            else if (detail::line_is(line, "connection:"))
            {
                std::string_view v = detail::ltrim(value);
                if (v.size() >= 5 && detail::line_is(v, "close")) out.keep_alive = false;
            }
            pos = eol;
        }
        if (out.chunked && out.has_content_length) return FrameStatus::Error;
        if (out.has_content_length && out.content_length > kMaxBodyLen) return FrameStatus::Error;
        return FrameStatus::Complete;
    }

    class ChunkDecoder
    {
    public:
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
            _consumed += i; // bytes of `in` this call actually took
            return _st != St::Error;
        }

        [[nodiscard]] bool done() const noexcept { return _st == St::Done; }

        [[nodiscard]] size_t consumed() const noexcept { return _consumed; }
        [[nodiscard]] bool failed() const noexcept { return _st == St::Error; }

    private:
        enum class St { Size, Data, DataCRLF, Trailer, Done, Error };
        bool fail() noexcept { _st = St::Error; return false; }

        St _st = St::Size;
        size_t _remaining = 0; // bytes left in the current chunk's data
        size_t _consumed = 0;  // input bytes taken so far (see consumed())
        std::string _line;     // accumulates a size/CRLF/trailer line across reads
    };

    struct ParsedResponse
    {
        FrameStatus status = FrameStatus::NeedMore;
        ResponseHead head{};
        std::string_view body{}; // valid only when status == Complete
        size_t total_len = 0;    // bytes of `buf` this message occupies

        [[nodiscard]] bool complete() const noexcept { return status == FrameStatus::Complete; }
        [[nodiscard]] bool failed() const noexcept { return status == FrameStatus::Error; }
    };

    struct ResponseDecoder
    {
        ChunkDecoder dec;
        std::string body; // decoded payload, accumulated across feeds
        size_t fed = 0;   // post-header bytes already handed to `dec`

        void reset() noexcept
        {
            dec = ChunkDecoder{};
            body.clear();
            fed = 0;
        }
    };

    [[nodiscard]] inline ParsedResponse parse_response(std::string_view buf,
                                                       ResponseDecoder& st) noexcept
    {
        ParsedResponse r;
        const FrameStatus hs = parse_response_head(buf, r.head);
        if (hs == FrameStatus::NeedMore) return r; // status stays NeedMore
        if (hs == FrameStatus::Error) { r.status = FrameStatus::Error; return r; }

        if (r.head.chunked && r.head.has_content_length) { r.status = FrameStatus::Error; return r; }

        if (r.head.status < 200) { r.status = FrameStatus::Error; return r; }

        if (!r.head.chunked && !r.head.has_content_length &&
            r.head.status != 204 && r.head.status != 304)
        {
            r.status = FrameStatus::Error;
            return r;
        }

        if (!r.head.chunked)
        {
            if (r.head.content_length > kMaxBodyLen) { r.status = FrameStatus::Error; return r; }
            const size_t need = r.head.header_len + r.head.content_length;
            if (buf.size() < need) return r; // NeedMore
            r.body = buf.substr(r.head.header_len, r.head.content_length); // no copy
            r.total_len = need;
            r.status = FrameStatus::Complete;
            return r;
        }

        const std::string_view after = buf.substr(r.head.header_len);
        if (!st.dec.done() && st.fed < after.size())
        {
            if (!st.dec.feed(after.substr(st.fed), st.body)) { r.status = FrameStatus::Error; return r; }
            st.fed = st.dec.consumed();
        }
        if (st.body.size() > kMaxBodyLen) { r.status = FrameStatus::Error; return r; }
        if (!st.dec.done()) return r; // NeedMore
        r.body = st.body;
        r.total_len = r.head.header_len + st.dec.consumed();
        r.status = FrameStatus::Complete;
        return r;
    }
} // namespace llmbridge::net::http_legacy