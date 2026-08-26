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
// Rejected instead of ignored. Ignoring it (while an upstream honours it) is
// the classic request-smuggling desync, dangerous with pooled upstreams. A
// conflicting duplicate Content-Length is likewise rejected (RFC 9112 §6). No
// allocation, no copy: everything is index math over a caller-owned buffer, a
// zero-alloc, hand-written framer.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
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
    };

    // The framing tri-state, shared by every parse entry point in this header.
    //
    // There used to be three of these, one per function, for one concept:
    // `ParseStatus` and `RespStatus` were character-for-character identical, and
    // `HeadStatus` differed only in spelling Complete as "Ok". A reader had to
    // learn which enum belonged to which call before reading either.
    enum class FrameStatus
    {
        NeedMore, // not fully buffered yet: feed more bytes and re-run
        Complete, // the thing this call frames is fully present
        Error     // malformed; refuse the message (see the fail-closed policy)
    };

    namespace detail
    {
        // ASCII lower-case, branch-light.
        constexpr char lc(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }

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

        // Case-insensitive prefix test: `name` must be lower-case, and callers that
        // mean a header name must include the ':' themselves, or `x-tenant` matches
        // the line `x-tenant-spoof:`. That is why this is `detail::` and why
        // find_header() below does not use it: an exact header lookup is the thing
        // callers actually want, and this primitive is one colon away from being a
        // header-forgery hole. It is also used on values (`close`), which is why it
        // cannot simply require a colon itself.
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

        // Trim optional whitespace (RFC 9110 OWS = SP / HTAB) from both ends of a
        // field value. Trailing OWS is legal and must be stripped before a strict
        // numeric test, or "Content-Length: 27 " would be rejected as non-numeric.
        inline std::string_view trim_ows(std::string_view v) noexcept
        {
            size_t b = 0, e = v.size();
            while (b < e && (v[b] == ' ' || v[b] == '\t')) ++b;
            while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t')) --e;
            return v.substr(b, e - b);
        }

        // Strict Content-Length: RFC 9112 §8.6 is 1*DIGIT, nothing else. Plain
        // std::from_chars is not enough; it stops at the first non-digit and
        // still reports success, so "0x1b" parses as 0 and "27abc" as 27. Both
        // were measured: the first framed a 27-byte body as empty. A length we
        // read differently from the upstream is a desync, so anything that is not
        // pure digits is refused outright.
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

        // Reject a header block containing a bare CR or bare LF.
        //
        // We split lines on CRLF. A parser that splits on a bare CR (or bare LF)
        // instead sees a different set of headers than we do; it may see a
        // Content-Length we never saw. Measured: "X-A: 1\rContent-Length: 27"
        // made the length invisible to this framer while a lenient upstream
        // honoured it, framing 27 bytes we never sent. On a pooled upstream that
        // disagreement is a cross-client desync, so this is a refusal and never a
        // sanitise-and-forward.
        inline bool block_line_endings_ok(std::string_view h) noexcept
        {
            for (size_t i = 0; i < h.size(); ++i)
            {
                if (h[i] == '\r' && (i + 1 >= h.size() || h[i + 1] != '\n')) return false;
                if (h[i] == '\n' && (i == 0 || h[i - 1] != '\r')) return false;
            }
            return true;
        }

        // Validate one header line and locate its colon.
        //
        // Two refusals beyond "must have a colon", both of which otherwise make a
        // header invisible to every `name:` prefix test in this file while a more
        // lenient upstream still honours it:
        //   * obs-fold: a line starting with SP/HTAB is a continuation of the
        //     previous header (RFC 9112 §5.2, deprecated, MUST-reject on receipt).
        //   * whitespace before the colon. "Content-Length : 27" (RFC 9112 §5.1
        //     forbids it precisely because it is a smuggling primitive).
        inline bool line_ok(std::string_view line, size_t& colon) noexcept
        {
            if (line.empty()) return false;
            if (line[0] == ' ' || line[0] == '\t') return false; // obs-fold
            colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) return false;
            return !(line[colon - 1] == ' ' || line[colon - 1] == '\t');
        }
    } // namespace detail

    // Case-insensitive single-header lookup over a raw header block (the bytes
    // between the start line's CRLF and the terminating CRLFCRLF, or the whole
    // message; the start line simply never matches a `name:` prefix). Returns the
    // First occurrence's value, left-trimmed, or empty if absent. Zero-copy: the
    // view aliases `headers`.
    //
    // First-wins is a deliberate anti-smuggling stance: when a client sends a
    // duplicated header, the copies must not be interpreted differently by us
    // and by the upstream. We take the first and, because the gateway rebuilds
    // the upstream request from a whitelist instead of echoing the block, the
    // duplicate never travels.
    //
    // Warning: The returned value is not safe to re-emit as-is. Splitting on CRLF leaves
    // a bare CR (or any other control byte) inside the value, a lenient parser
    // that treats bare CR as a line terminator then sees an injected header. An
    // earlier revision of this comment claimed the opposite; it was wrong, and the
    // injection was measured reaching an upstream. Callers that re-emit a value
    // Must validate its charset first (see header_value_safe in gateway.cpp).
    // `name` must be lowercase and include the trailing colon ("x-api-key:"),
    // same convention as the framer's own header matching, and the colon is what
    // stops "x-api-key-2:" from prefix-matching "x-api-key".
    /// Value of header `name`, empty when absent. A view into `headers`.
    ///
    /// Write the name however reads best: `"authorization"`, `"Authorization"` and
    /// `"authorization:"` all do the same thing.
    ///
    /// It used to take the name with a trailing colon, compare it as a bare prefix
    /// and never lower-case it, so both spellings above failed silently and
    /// `find_header(h, "x-tenant")` returned the value of a client-supplied
    /// `x-tenant-spoof:` line. Nothing in production called it, so nothing was
    /// exploitable; what it was, was a hole waiting for its first caller, and the
    /// first caller was going to be code deciding whether a request is authorised.
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
            // Stop at the blank line. Every caller passes exactly a header block today,
            // so this changes nothing for them; it exists because the one that does not
            // hands an attacker a free header. A credential written into the body was
            // otherwise found and honoured, which is a header the client controls
            // entirely and no framing check ever sees.
            if (line.empty()) return {};
            // The colon must be exactly where the name ends: that is what makes this
            // an exact field-name match and not a prefix match.
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

    // Cap on header section size: a guard against an unbounded slow-loris
    // style buffer growth. 32 KiB is comfortably above any sane chat-completion
    // request/response header block.
    inline constexpr size_t kMaxHeaderLen = 32 * 1024;

    // Cap on Content-Length. Without this, a `Content-Length: 9999999999` header
    // followed by a slow byte trickle grows the receive buffer without bound:
    // a trivial memory-exhaustion DoS. 16 MiB is far above any chat-completion
    // body (including base64 vision images); genuine large/streaming payloads are
    // a Phase C concern with their own backpressure.
    inline constexpr size_t kMaxBodyLen = 16 * 1024 * 1024;

    // Parse framing info out of `buf`. Idempotent and cheap to re-run as more
    // bytes arrive: returns NeedMore until the full message is buffered.
    inline FrameStatus parse_request(std::string_view buf, Message& out) noexcept
    {
        // Locate end of header block (CRLF CRLF).
        const size_t hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string_view::npos)
        {
            if (buf.size() > kMaxHeaderLen) return FrameStatus::Error;
            return FrameStatus::NeedMore;
        }
        out.header_len = hdr_end + 4;
        if (out.header_len > kMaxHeaderLen) return FrameStatus::Error;

        // Walk header lines after the request/status line, pulling the two
        // headers we care about. Default keep-alive for HTTP/1.1.
        out.body_len = 0;
        out.keep_alive = true;
        bool have_cl = false; // to detect a conflicting duplicate Content-Length
        std::string_view headers = buf.substr(0, hdr_end);

        // Fail closed on a header block we and an upstream would split differently.
        if (!detail::block_line_endings_ok(headers)) return FrameStatus::Error;

        size_t pos = headers.find("\r\n");
        if (pos == std::string_view::npos) pos = headers.size(); // no headers
        while (pos < headers.size())
        {
            size_t start = pos + 2;
            size_t eol = headers.find("\r\n", start);
            if (eol == std::string_view::npos) eol = headers.size();
            std::string_view line = headers.substr(start, eol - start);

            // A line we cannot unambiguously read is a line the upstream might
            // read anyway: refuse the message instead of skip the header.
            size_t colon = 0;
            if (!detail::line_ok(line, colon)) return FrameStatus::Error;
            const std::string_view value = line.substr(colon + 1);

            if (detail::line_is(line, "content-length:"))
            {
                size_t n = 0;
                if (!detail::parse_strict_length(value, n)) return FrameStatus::Error;
                // Conflicting duplicate Content-Length → reject (smuggling vector,
                // RFC 9112 §6.3). An identical repeat is harmless; collapse it.
                if (have_cl && n != out.body_len) return FrameStatus::Error;
                out.body_len = n;
                have_cl = true;
            }
            else if (detail::line_is(line, "transfer-encoding:"))
            {
                // We frame by Content-Length only; refuse TE outright instead of
                // risk a TE/CL desync against a TE-honouring upstream.
                return FrameStatus::Error;
            }
            else if (detail::line_is(line, "content-encoding:"))
            {
                // Recorded, never acted on: framing is by Content-Length either way.
                // It matters upstream of here because every body-reading caller
                // assumes JSON, and a compressed body is not JSON.
                const std::string_view v = detail::ltrim(value);
                out.encoded = !v.empty() && !detail::contains_ci(v, "identity");
            }
            else if (detail::line_is(line, "connection:"))
            {
                std::string_view v = detail::ltrim(value);
                // Only "close" flips the default; "keep-alive" is the default.
                if (v.size() >= 5 && detail::line_is(v, "close")) out.keep_alive = false;
            }
            pos = eol;
        }

        // Reject a hostile / absurd body length before we ever buffer toward it.
        if (out.body_len > kMaxBodyLen) return FrameStatus::Error;

        out.total_len = out.header_len + out.body_len;
        if (buf.size() < out.total_len) return FrameStatus::NeedMore;
        return FrameStatus::Complete;
    }

    // ── Streaming response support (Phase B) ────────────────────────────────
    //
    // SSE responses (Anthropic `stream:true`) are framed by chunked
    // transfer-encoding and carry no Content-Length, so the strict `parse()`
    // above (Content-Length only, TE rejected) cannot read them. The two helpers
    // below add exactly what the gateway's *response* path needs to pump a stream
    //, and only the response path: client-request framing stays strict, so the
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
        bool encoded = false;          // Content-Encoding present and not identity
        size_t content_length = 0;
    };


    namespace detail
    {
    } // namespace detail

    // Parse a response header block. Unlike parse(), this tolerates chunked
    // transfer-encoding (needed for SSE) and reports how the body is framed so the
    // caller can choose whole-body vs streaming handling. Returns NeedMore until
    // the CRLFCRLF is buffered.
    inline FrameStatus parse_response_head(std::string_view buf, ResponseHead& out) noexcept
    {
        const size_t hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string_view::npos)
            return buf.size() > kMaxHeaderLen ? FrameStatus::Error : FrameStatus::NeedMore;
        out.header_len = hdr_end + 4;
        if (out.header_len > kMaxHeaderLen) return FrameStatus::Error;

        std::string_view headers = buf.substr(0, hdr_end);

        // The status line must be "HTTP/1.x SP ...", and checking that is what stops
        // arbitrary leading bytes being absorbed into it. The scan below takes the
        // first space anywhere in the head and reads three digits after it, so
        // without this check a blob prepended to a response frames as a normal
        // reply whenever it contains no space and no CRLF: "JUNKHTTP/1.1 200 OK"
        // yields status 200 and the junk disappears.
        //
        // Only reachable if stray bytes survive on a pooled upstream, which
        // {ep,ur}_release_upstream prevents by clearing rbuf. This is the second
        // lock. The v0.8.1 defects were all one shape, malformed input becoming
        // Invisible input, and the cure is refusing it here instead of trusting a
        // single guard elsewhere.
        if (headers.size() < 9 || headers.compare(0, 7, "HTTP/1.") != 0 ||
            (headers[7] != '0' && headers[7] != '1') || headers[8] != ' ')
            return FrameStatus::Error;

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

        // Same strictness as the request framer, and for a sharper reason: a
        // mis-framed response leaves stray bytes on a pooled upstream connection,
        // where they become the head of the next client's response. A framing
        // disagreement here hands one client another client's bytes, so a
        // malformed response is refused instead of salvaged.
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
                // Conflicting duplicate → reject, as on the request path.
                if (out.has_content_length && n != out.content_length) return FrameStatus::Error;
                out.content_length = n;
                out.has_content_length = true;
            }
            else if (detail::line_is(line, "transfer-encoding:"))
            {
                if (detail::contains_ci(value, "chunked")) out.chunked = true;
            }
            else if (detail::line_is(line, "content-encoding:"))
            {
                // Recorded, never acted on here: the framer's job is where the body
                // ends, not what it means. The gateway warns, because a compressed
                // body is one it cannot read token counts out of.
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
        // Both framings present (RFC 9112 §6.3): the two disagree about where the
        // body ends, and whichever we pick, the other is what some intermediary
        // picked. Refuse instead of preferring one.
        if (out.chunked && out.has_content_length) return FrameStatus::Error;
        if (out.has_content_length && out.content_length > kMaxBodyLen) return FrameStatus::Error;
        return FrameStatus::Complete;
    }

    // Incremental HTTP/1.1 chunked-transfer decoder. Feed body bytes as they
    // arrive (a chunk header or its data may split across reads); it appends the
    // decoded payload to `out` and tracks completion (the terminating 0-length
    // chunk). Stateful: hold one per streamed response. Bounded: an absurd chunk
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
            _consumed += i; // bytes of `in` this call actually took
            return _st != St::Error;
        }

        [[nodiscard]] bool done() const noexcept { return _st == St::Done; }

        // Total input bytes consumed across all feeds. Needed to find where a
        // chunked message ends: anything after that is the next pipelined response
        // (or trailing junk that must not be mistaken for one). The loop above
        // stops advancing `i` once Done, so this never over-counts past the
        // terminating chunk.
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

    // Frame a complete upstream response, accepting both body encodings.
    //
    // Why this exists separately from parse(). HTTP/1.1 delimits a body either by
    // Content-Length or by Transfer-Encoding: chunked. parse() implements only the
    // former and rejects the latter deliberately, because on the request path we
    // are the server, the bytes are attacker-controlled, and framing TE as CL while
    // an upstream honours TE is the classic smuggling desync (worse here, since a
    // desync on a pooled upstream lets one client's trailing bytes become the head
    // of another client's request). That rule must not be relaxed.
    //
    // On the response path the threat model is inverted: we are the client, talking
    // to a configured and TLS-verified provider, and chunked is simply normal
    // HTTP/1.1: a server uses it whenever the body length is unknown when headers
    // are sent, which is the usual case for a generated completion. Real providers
    // do exactly this: Anthropic returns non-streaming completions as chunked over
    // HTTP/1.1 (invisible over HTTP/2, which has native framing and no chunked at
    // all, so a curl probe that negotiates h2 will not show it).
    //
    // The streaming path already accepted chunked via ChunkDecoder; only the
    // whole-body path did not, which is the asymmetry this closes.
    //
    // Re-runnable: like parse(), it re-frames from the buffer start on every call
    // and returns NeedMore until the whole message is present.
    //
    // Returns everything in one value instead of through out-parameters: four of
    // them (head, body, total_len + status) was unreadable at the call site.
    //
    // ZERO-COPY where it can be. On Complete, `body` is a view of the decoded body:
    //   Content-Length -> it aliases `buf` directly. The bytes are already
    //                     contiguous, so copying them would be pure waste, and this
    //                     is the path that runs at full request rate.
    //   chunked        -> it aliases `scratch`, because the body bytes are
    //                     interleaved with chunk-size lines and cannot be viewed in
    //                     place. That copy is inherent to the encoding, not a
    //                     choice.
    // `scratch` is caller-owned so its capacity can be reused across requests
    // instead of allocating per response. It is untouched in the Content-Length
    // case. `body` is valid only while both `buf` and `scratch` outlive it, and
    // only until either is modified; in the gateway that means before rbuf is
    // erased or the upstream is released.

    struct ParsedResponse
    {
        FrameStatus status = FrameStatus::NeedMore;
        ResponseHead head{};
        std::string_view body{}; // valid only when status == Complete
        size_t total_len = 0;    // bytes of `buf` this message occupies

        [[nodiscard]] bool complete() const noexcept { return status == FrameStatus::Complete; }
        [[nodiscard]] bool failed() const noexcept { return status == FrameStatus::Error; }
    };

    // Per-connection decode state for non-streaming responses.
    //
    // This exists to keep the chunked path linear. An earlier revision built a
    // fresh ChunkDecoder on every call and re-decoded the whole buffer from byte
    // zero, which is O(n^2) as bytes trickle in, measured on a single upstream
    // connection, decoding a body arriving in 64 KiB reads:
    //
    //     1 MB ->  1.6 ms      4 MB -> 13.9 ms
    //     2 MB ->  4.3 ms      8 MB -> 79.0 ms   (65x the body re-decoded)
    //
    // Scope, so nobody over-reads the table: a typical 1 KB reply arrives in one
    // read, so this loop never runs and the cost is ~0.4 us either way. It takes a
    // body spanning several reads to matter at all (~128 KB), and ~500 KB before it
    // costs a millisecond. Normal traffic was never affected.
    //
    // What makes it worth fixing is not the latency of the request that triggers it
    // but HEAD-OF-LINE blocking: the loop is single-threaded, so a 79 ms decode
    // delays every other client on that worker by up to 79 ms. An unusually large
    // completion can trigger it; a hostile upstream can do it at will up to
    // kMaxBodyLen. Feeding only the newly-arrived bytes makes the cost linear and
    // the buffer reusable.
    struct ResponseDecoder
    {
        ChunkDecoder dec;
        std::string body; // decoded payload, accumulated across feeds
        size_t fed = 0;   // post-header bytes already handed to `dec`

        // Call between responses on a pooled connection. clear() keeps capacity,
        // so a warm connection does no per-request allocation.
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

        // Both framings present is a smuggling signal even from a trusted origin
        // (a compromised or buggy middlebox), so refuse instead of pick a winner.
        if (r.head.chunked && r.head.has_content_length) { r.status = FrameStatus::Error; return r; }

        // An interim response is not the answer. Framed as one, "100 Continue"
        // reaches the client as the reply and the real response is orphaned on a
        // connection we then pool, where it becomes the next client's bytes. A
        // client can provoke it with `Expect: 100-continue`, which is why
        // request_without also drops that header on the way out.
        if (r.head.status < 200) { r.status = FrameStatus::Error; return r; }

        // Neither framing header means RFC 9112 rule 8: the body runs until the
        // server closes, and the connection is not reusable afterwards. We pool
        // upstream connections, so framing one here can only drop the body (which
        // is what happened before this check) or desynchronise the pool. Refuse.
        //
        // 204 and 304 carry no body by definition and are the legitimate case for
        // no framing header at all. A streamed response is diverted to the pump on
        // its head, before this function, so close-delimited SSE is unaffected.
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

        // Chunked: feed only the bytes that arrived since the last call. The
        // decoder and the decoded body persist in `st` across calls, so a body
        // delivered in N reads costs O(body) instead of O(N * body). Still
        // idempotent: re-calling with an unchanged buffer feeds nothing and
        // re-reports the same result. `st` must be reset between responses on a
        // pooled connection (see ResponseDecoder::reset).
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