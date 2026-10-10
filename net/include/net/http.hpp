// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// HTTP/1.1 framing for the proxy hot path: index math over a caller-owned buffer.
// Requests frame by Content-Length only; responses also accept chunked. What is
// refused and why: DESIGN.md "Parsing & framing".

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace llmbridge::net::http
{
    struct Message
    {
        size_t header_len = 0; // bytes up to and including the CRLFCRLF
        size_t body_len = 0;   // Content-Length value (0 if absent)
        size_t total_len = 0;  // header_len + body_len: full message size
        bool keep_alive = true;
        bool encoded = false;  // Content-Encoding present and not identity
        bool http_1_1 = true;  // false for 1.0, which has no chunked encoding
    };

    // The framing tri-state, shared by every parse entry point in this header.
    enum class FrameStatus
    {
        NeedMore, // not fully buffered yet: feed more bytes and re-run
        Complete, // the thing this call frames is fully present
        Error     // malformed; refuse the message (see the fail-closed policy)
    };

    /// The header fields a framer acts on; every other name is Other.
    enum class Field : uint8_t
    {
        Other, ContentLength, TransferEncoding, ContentEncoding, ContentType, Connection,
        RetryAfter, RequestsRemaining, InputTokensRemaining, OutputTokensRemaining,
        TokensRemaining
    };

    /// One header line. `value` is everything after the colon, untrimmed.
    struct HeaderLine
    {
        Field field = Field::Other;
        std::string_view name, value;
    };

    namespace detail
    {
        constexpr char lc(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }

        // Case-insensitive substring search; `needle` must be lower-case.
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

        // Case-insensitive prefix test; `name` must be lower-case. Not a header
        // lookup: a name without its ':' prefix-matches longer names.
        inline bool line_is(std::string_view line, std::string_view name) noexcept
        {
            if (line.size() < name.size()) return false;
            for (size_t i = 0; i < name.size(); ++i)
                if (lc(line[i]) != name[i]) return false;
            return true;
        }

        inline bool same_ci(std::string_view a, std::string_view b) noexcept
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (lc(a[i]) != lc(b[i])) return false;
            return true;
        }

        inline std::string_view ltrim(std::string_view v) noexcept
        {
            size_t i = 0;
            while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
            return v.substr(i);
        }

        // RFC 9110 OWS (SP / HTAB) off both ends.
        inline std::string_view trim_ows(std::string_view v) noexcept
        {
            size_t b = 0, e = v.size();
            while (b < e && (v[b] == ' ' || v[b] == '\t')) ++b;
            while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t')) --e;
            return v.substr(b, e - b);
        }

        // RFC 9112 §8.6: 1*DIGIT and nothing else, unlike a bare from_chars.
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

        // Only a well-formed 0 counts as an exhausted quota.
        inline bool is_zero_count(std::string_view v) noexcept
        {
            size_t n = 0;
            return parse_strict_length(v, n) && n == 0;
        }

        // The end of the line starting at `from`: the CR of its CRLF, or the end of
        // `s`. False when the line holds a bare CR or a bare LF.
        inline bool line_end(std::string_view s, size_t from, size_t& eol) noexcept
        {
            const char* b = s.data() + from;
            size_t len = s.size() - from;
            if (const void* lf = len ? std::memchr(b, '\n', len) : nullptr)
            {
                len = static_cast<size_t>(static_cast<const char*>(lf) - b);
                if (len == 0 || b[len - 1] != '\r') return false;
                --len;
            }
            if (len && std::memchr(b, '\r', len)) return false;
            eol = from + len;
            return true;
        }

        // A header line needs a colon after a non-empty name, no obs-fold and no
        // whitespace before the colon (RFC 9112 §5.1, §5.2).
        inline bool line_ok(std::string_view line, size_t& colon) noexcept
        {
            if (line.empty()) return false;
            if (line[0] == ' ' || line[0] == '\t') return false; // obs-fold
            colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) return false;
            return !(line[colon - 1] == ' ' || line[colon - 1] == '\t');
        }

        struct FieldName
        {
            std::string_view name;
            Field field;
        };
        inline constexpr FieldName kFieldNames[] = {
            {"connection", Field::Connection},
            {"retry-after", Field::RetryAfter},
            {"content-type", Field::ContentType},
            {"content-length", Field::ContentLength},
            {"content-encoding", Field::ContentEncoding},
            {"transfer-encoding", Field::TransferEncoding},
            {"x-ratelimit-remaining-tokens", Field::TokensRemaining},
            {"x-ratelimit-remaining-requests", Field::RequestsRemaining},
            {"anthropic-ratelimit-requests-remaining", Field::RequestsRemaining},
            {"anthropic-ratelimit-input-tokens-remaining", Field::InputTokensRemaining},
            {"anthropic-ratelimit-output-tokens-remaining", Field::OutputTokensRemaining},
        };

        inline Field classify(std::string_view name) noexcept
        {
            for (const FieldName& f : kFieldNames)
                if (same_ci(name, f.name)) return f.field;
            return Field::Other;
        }
    } // namespace detail

    /// Whether the comma-separated `list` holds `lower` as a whole token, any case.
    inline bool has_token(std::string_view list, std::string_view lower) noexcept
    {
        while (!list.empty())
        {
            const size_t comma = list.find(',');
            if (detail::same_ci(detail::trim_ows(list.substr(0, comma)), lower)) return true;
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
        return false;
    }

    /// Calls `on_line(const HeaderLine&) -> bool` for each header line of `head`, the
    /// bytes before the terminating CRLFCRLF; the start line is checked, then skipped.
    /// False when another parser could split the head differently (a bare CR or LF
    /// anywhere, obs-fold, no colon, whitespace before the colon) or `on_line` refuses.
    template <class OnLine>
    bool walk_headers(std::string_view head, OnLine&& on_line) noexcept
    {
        size_t eol = 0;
        if (!detail::line_end(head, 0, eol)) return false;
        while (eol < head.size())
        {
            const size_t start = eol + 2;
            if (!detail::line_end(head, start, eol)) return false;
            const std::string_view line = head.substr(start, eol - start);
            size_t colon = 0;
            if (!detail::line_ok(line, colon)) return false;
            const std::string_view name = line.substr(0, colon);
            if (!on_line(HeaderLine{detail::classify(name), name, line.substr(colon + 1)}))
                return false;
        }
        return true;
    }

    /// Value of header `name` (any case, trailing colon optional), left-trimmed;
    /// empty when absent. First match wins, the search stops at a blank line, and a
    /// start line may be included. Lenient on purpose: a bare CR stays inside the
    /// value, so a caller re-emitting it must validate the charset first.
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
            if (line.size() > name.size() && line[name.size()] == ':' &&
                detail::same_ci(line.substr(0, name.size()), name))
                return detail::ltrim(line.substr(name.size() + 1));
            start = eol + 2;
        }
        return {};
    }

    // Header section cap: bounds slow-loris buffer growth.
    inline constexpr size_t kMaxHeaderLen = 32 * 1024;

    // Content-Length cap: bounds a `Content-Length: 9999999999` trickle.
    inline constexpr size_t kMaxBodyLen = 16 * 1024 * 1024;

    // Idempotent: re-run as bytes arrive; NeedMore until the full message is buffered.
    inline FrameStatus parse_request(std::string_view buf, Message& out) noexcept
    {
        const size_t hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string_view::npos)
            return buf.size() > kMaxHeaderLen ? FrameStatus::Error : FrameStatus::NeedMore;
        out.header_len = hdr_end + 4;
        if (out.header_len > kMaxHeaderLen) return FrameStatus::Error;

        out.body_len = 0;
        out.keep_alive = true;
        const std::string_view head = buf.substr(0, hdr_end);
        // "method SP target SP HTTP/1.x": the version follows the last space.
        const std::string_view rl = head.substr(0, head.find("\r\n"));
        const size_t sp = rl.rfind(' ');
        const std::string_view ver = sp == std::string_view::npos ? std::string_view{}
                                                                 : rl.substr(sp + 1);
        out.http_1_1 = ver == "HTTP/1.1";
        if (ver == "HTTP/1.0") out.keep_alive = false;

        bool have_cl = false, close = false, keep = false;
        const bool ok = walk_headers(head, [&](const HeaderLine& h) noexcept {
            switch (h.field)
            {
                case Field::ContentLength:
                {
                    size_t n = 0;
                    if (!detail::parse_strict_length(h.value, n)) return false;
                    if (have_cl && n != out.body_len) return false; // RFC 9112 §6.3
                    out.body_len = n;
                    have_cl = true;
                    return true;
                }
                case Field::TransferEncoding:
                    return false; // Content-Length framing only
                case Field::ContentEncoding:
                {
                    const std::string_view v = detail::ltrim(h.value);
                    out.encoded = !v.empty() && !detail::contains_ci(v, "identity");
                    return true;
                }
                case Field::Connection:
                    close |= has_token(h.value, "close");
                    keep |= has_token(h.value, "keep-alive");
                    return true;
                default:
                    return true;
            }
        });
        if (!ok || out.body_len > kMaxBodyLen) return FrameStatus::Error;
        if (close || keep) out.keep_alive = !close;

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
        /// The quota family whose remaining count is zero: Anthropic names it in the
        /// header name, OpenAI in its suffix.
        enum class Quota : uint8_t { None = 0, Requests = 1, InputTokens = 2,
                                     OutputTokens = 3, Tokens = 4 };
        Quota quota_exhausted = Quota::None;
        /// `Retry-After` in seconds, 0 when absent, saturated at 65535.
        uint16_t retry_after_s = 0;
    };

    // Unlike parse_request, accepts chunked (SSE and most non-streamed replies) and
    // reports the framing. NeedMore until the CRLFCRLF is buffered.
    inline FrameStatus parse_response_head(std::string_view buf, ResponseHead& out) noexcept
    {
        const size_t hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string_view::npos)
            return buf.size() > kMaxHeaderLen ? FrameStatus::Error : FrameStatus::NeedMore;
        out.header_len = hdr_end + 4;
        if (out.header_len > kMaxHeaderLen) return FrameStatus::Error;

        const std::string_view head = buf.substr(0, hdr_end);
        // "HTTP/1.x SP" first, so leading junk is never absorbed into the status line.
        if (head.size() < 9 || head.compare(0, 7, "HTTP/1.") != 0 ||
            (head[7] != '0' && head[7] != '1') || head[8] != ' ')
            return FrameStatus::Error;

        const std::string_view rest = detail::ltrim(head.substr(9));
        int code = 0;
        size_t k = 0;
        for (; k < rest.size() && k < 3 && rest[k] >= '0' && rest[k] <= '9'; ++k)
            code = code * 10 + (rest[k] - '0');
        if (k == 3) out.status = code;

        out.keep_alive = head[7] == '1'; // 1.0 closes unless it says keep-alive
        bool close = false, keep = false;
        using Quota = ResponseHead::Quota;
        const auto quota = [&out](std::string_view v, Quota q) noexcept {
            if (detail::is_zero_count(v)) out.quota_exhausted = q;
            return true;
        };
        const bool ok = walk_headers(head, [&](const HeaderLine& h) noexcept {
            switch (h.field)
            {
                case Field::ContentLength:
                {
                    size_t n = 0;
                    if (!detail::parse_strict_length(h.value, n)) return false;
                    if (out.has_content_length && n != out.content_length) return false;
                    out.content_length = n;
                    out.has_content_length = true;
                    return true;
                }
                case Field::RetryAfter:
                {
                    size_t n = 0; // seconds only; an HTTP-date reads as absent
                    if (detail::parse_strict_length(h.value, n))
                        out.retry_after_s = n > 65535 ? 65535 : static_cast<uint16_t>(n);
                    return true;
                }
                case Field::RequestsRemaining: return quota(h.value, Quota::Requests);
                case Field::InputTokensRemaining: return quota(h.value, Quota::InputTokens);
                case Field::OutputTokensRemaining: return quota(h.value, Quota::OutputTokens);
                case Field::TokensRemaining: return quota(h.value, Quota::Tokens);
                case Field::TransferEncoding:
                    if (detail::contains_ci(h.value, "chunked")) out.chunked = true;
                    return true;
                case Field::ContentEncoding:
                {
                    const std::string_view v = detail::ltrim(h.value);
                    out.encoded = !v.empty() && !detail::contains_ci(v, "identity");
                    return true;
                }
                case Field::ContentType:
                    if (detail::contains_ci(h.value, "text/event-stream")) out.event_stream = true;
                    return true;
                case Field::Connection:
                    close |= has_token(h.value, "close");
                    keep |= has_token(h.value, "keep-alive");
                    return true;
                default:
                    return true;
            }
        });
        if (!ok) return FrameStatus::Error;
        if (close || keep) out.keep_alive = !close;
        // Both framings present (RFC 9112 §6.3): refuse instead of picking one.
        if (out.chunked && out.has_content_length) return FrameStatus::Error;
        if (out.has_content_length && out.content_length > kMaxBodyLen) return FrameStatus::Error;
        return FrameStatus::Complete;
    }

    // Incremental chunked-transfer decoder, one per response: a size line or its data
    // may split across reads. An absurd chunk size or size line is an error.
    class ChunkDecoder
    {
    public:
        // Appends decoded bytes to `out`; false on a malformed stream. Bytes fed after
        // done() are ignored.
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

        // Input bytes taken across all feeds, never past the terminating chunk: where
        // a chunked message ends and any pipelined bytes begin.
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

    // A complete upstream response, framed by Content-Length or chunked. On Complete,
    // `body` aliases `buf` (Content-Length) or the decoder's buffer (chunked), and is
    // valid until either changes.
    struct ParsedResponse
    {
        FrameStatus status = FrameStatus::NeedMore;
        ResponseHead head{};
        std::string_view body{}; // valid only when status == Complete
        size_t total_len = 0;    // bytes of `buf` this message occupies

        [[nodiscard]] bool complete() const noexcept { return status == FrameStatus::Complete; }
        [[nodiscard]] bool failed() const noexcept { return status == FrameStatus::Error; }
    };

    // Per-connection chunked decode state, fed only new bytes so a body arriving in
    // many reads costs O(body), not O(reads * body).
    struct ResponseDecoder
    {
        ChunkDecoder dec;
        std::string body; // decoded payload, accumulated across feeds
        size_t fed = 0;   // post-header bytes already handed to `dec`

        // Between responses on a pooled connection; keeps capacity.
        void reset() noexcept
        {
            dec = ChunkDecoder{};
            body.clear();
            fed = 0;
        }
    };

    // Re-runnable like parse_request: NeedMore until the whole message is present.
    [[nodiscard]] inline ParsedResponse parse_response(std::string_view buf,
                                                       ResponseDecoder& st) noexcept
    {
        ParsedResponse r;
        const FrameStatus hs = parse_response_head(buf, r.head);
        if (hs == FrameStatus::NeedMore) return r; // status stays NeedMore
        if (hs == FrameStatus::Error) { r.status = FrameStatus::Error; return r; }

        if (r.head.chunked && r.head.has_content_length) { r.status = FrameStatus::Error; return r; }

        // An interim 1xx is not the answer, and framing one as it orphans the real
        // response on a pooled connection.
        if (r.head.status < 200) { r.status = FrameStatus::Error; return r; }

        // No framing header means read-until-close (RFC 9112 §6.3 rule 8), which a
        // pooled connection cannot do; 204 and 304 carry no body by definition.
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

        // Feed only the bytes that arrived since the last call; `st` is reset between
        // responses on a pooled connection.
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
} // namespace llmbridge::net::http
