// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Exhaustive tests for the zero-alloc HTTP/1.1 framer (net/http.hpp).
//
// Coverage is generated programmatically (expected fields computed from the raw
// bytes, not hand-typed) across:
//   - Complete  : methods, Content-Length values/whitespace/case, Connection
//                 variants, header count/order, no-body requests.
//   - NeedMore  : header + body truncations, empty input.
//   - Error     : non-numeric / signed / empty Content-Length, oversize header,
//                 Transfer-Encoding, conflicting duplicate CL, over-cap body.
//   - Pipeline  : concatenated messages -> first is framed, total_len = first.
//   - Lenient   : documented parser quirks (identical dup CL collapses, "closed"
//                 prefix-matching "close"). NOTE: "trailing garbage after CL is
//                 accepted" used to live here as a quirk. It was a smuggling
//                 primitive, not a quirk — see the HttpDesync suite, which now
//                 asserts the rejection.
//   - HttpDesync: framing-desync regressions from the 2026-08-03 security sweep.
//   - Incremental: byte-by-byte arrival property — every proper prefix is
//                 NeedMore and never a false Complete/Error.

#include "net/http.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using llmbridge::http::Message;
using llmbridge::http::ParseStatus;
using llmbridge::http::parse;
using llmbridge::http::kMaxHeaderLen;

namespace
{
    std::string sanitize(std::string s)
    {
        for (char& c : s)
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return s;
    }

    // Build "<method> /p HTTP/1.1\r\n" + headers + "\r\n" + body.
    std::string build(const std::string& method,
                      const std::vector<std::string>& header_lines,
                      const std::string& body)
    {
        std::string r = method + " /v1/chat/completions HTTP/1.1\r\n";
        for (const auto& h : header_lines) r += h + "\r\n";
        r += "\r\n";
        r += body;
        return r;
    }

    // ── Complete cases ────────────────────────────────────────────────────
    struct CompleteCase
    {
        std::string name;
        std::string raw;
        size_t body_len;
        bool keep_alive;
    };

    std::vector<CompleteCase> make_complete_cases()
    {
        std::vector<CompleteCase> v;

        for (const char* m : {"POST", "GET", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"})
            v.push_back({std::string("method_") + m,
                         build(m, {"Host: x", "Content-Length: 5"}, "12345"), 5, true});

        struct CV { const char* tok; bool ka; };
        for (const CV& cv : std::vector<CV>{
                 {"keep-alive", true}, {"Keep-Alive", true}, {"KEEP-ALIVE", true},
                 {"close", false}, {"Close", false}, {"CLOSE", false},
                 {"keep-alive, foo", true}})
            v.push_back({std::string("conn_") + sanitize(cv.tok),
                         build("POST", {"Host: x", std::string("Connection: ") + cv.tok,
                                        "Content-Length: 3"}, "abc"),
                         3, cv.ka});
        v.push_back({"conn_default", build("POST", {"Host: x", "Content-Length: 3"}, "abc"), 3, true});

        for (const char* cl : {"Content-Length", "content-length", "CONTENT-LENGTH",
                               "CoNtEnT-LeNgTh", "content-LENGTH"})
            v.push_back({std::string("clcase_") + sanitize(cl),
                         build("POST", {std::string(cl) + ": 4"}, "wxyz"), 4, true});

        for (size_t n : {size_t{0}, size_t{1}, size_t{5}, size_t{16}, size_t{64}, size_t{256}, size_t{1024}})
            v.push_back({"clval_" + std::to_string(n),
                         build("POST", {"Content-Length: " + std::to_string(n)}, std::string(n, 'x')),
                         n, true});

        v.push_back({"clws_nospace", build("POST", {"Content-Length:5"}, "abcde"), 5, true});
        v.push_back({"clws_double", build("POST", {"Content-Length:  5"}, "abcde"), 5, true});
        v.push_back({"clws_tab", build("POST", {"Content-Length:\t5"}, "abcde"), 5, true});

        for (int extra : {0, 1, 2, 5, 10})
        {
            std::vector<std::string> hs;
            for (int i = 0; i < extra; ++i) hs.push_back("X-H" + std::to_string(i) + ": v" + std::to_string(i));
            hs.push_back("Content-Length: 2");
            v.push_back({"extrahdrs_" + std::to_string(extra), build("POST", hs, "ab"), 2, true});
        }

        for (const char* m : {"GET", "HEAD", "DELETE", "OPTIONS"})
            v.push_back({std::string("nobody_") + m, build(m, {"Host: x"}, ""), 0, true});

        v.push_back({"order_cl_conn_host",
                     build("POST", {"Content-Length: 3", "Connection: close", "Host: x"}, "abc"), 3, false});
        v.push_back({"order_host_conn_cl",
                     build("POST", {"Host: x", "Connection: close", "Content-Length: 3"}, "abc"), 3, false});
        v.push_back({"order_conn_cl_host",
                     build("POST", {"Connection: keep-alive", "Content-Length: 3", "Host: x"}, "abc"), 3, true});
        return v;
    }

    // ── NeedMore / Error / Pipeline ───────────────────────────────────────
    struct RawCase { std::string name; std::string raw; };

    std::vector<RawCase> make_needmore_cases()
    {
        std::vector<RawCase> v;
        v.push_back({"empty", ""});
        v.push_back({"method_only", "POST"});
        v.push_back({"request_line_no_crlf", "POST /v1/chat/completions HTTP/1.1"});
        v.push_back({"request_line_crlf", "POST / HTTP/1.1\r\n"});
        v.push_back({"headers_no_terminator", "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n"});
        v.push_back({"only_cr_of_terminator", "POST / HTTP/1.1\r\nHost: x\r\n\r"});
        std::string full = build("POST", {"Content-Length: 10"}, "0123456789");
        for (size_t k = 1; k <= 10; ++k)
            v.push_back({"body_short_" + std::to_string(k), full.substr(0, full.size() - k)});
        v.push_back({"body_missing", build("POST", {"Content-Length: 8"}, "")});
        return v;
    }

    std::vector<RawCase> make_error_cases()
    {
        std::vector<RawCase> v;
        v.push_back({"cl_alpha", "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n"});
        v.push_back({"cl_empty", "POST / HTTP/1.1\r\nContent-Length: \r\n\r\n"});
        v.push_back({"cl_empty_nospace", "POST / HTTP/1.1\r\nContent-Length:\r\n\r\n"});
        v.push_back({"cl_negative", "POST / HTTP/1.1\r\nContent-Length: -5\r\n\r\n"});
        v.push_back({"cl_plus", "POST / HTTP/1.1\r\nContent-Length: +5\r\n\r\n"});
        v.push_back({"cl_space_then_alpha", "POST / HTTP/1.1\r\nContent-Length:  zzz\r\n\r\n"});
        v.push_back({"oversize_no_terminator", "POST / HTTP/1.1\r\n" + std::string(kMaxHeaderLen + 50, 'a')});
        v.push_back({"oversize_with_terminator",
                     "POST / HTTP/1.1\r\nX-Big: " + std::string(kMaxHeaderLen, 'b') + "\r\n\r\n"});
        // Hardening: reject Transfer-Encoding (we frame by CL only — anti-smuggling),
        // conflicting duplicate Content-Length, and an over-cap body length.
        v.push_back({"transfer_encoding_chunked",
                     "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"});
        v.push_back({"transfer_encoding_case",
                     "POST / HTTP/1.1\r\ntRaNsFeR-eNcOdInG: chunked\r\n\r\n"});
        v.push_back({"dup_content_length_conflict",
                     "POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 5\r\n\r\n"});
        v.push_back({"content_length_over_cap",
                     "POST / HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n"});
        return v;
    }

    struct PipelineCase { std::string name; std::string raw; size_t first_total; };

    std::vector<PipelineCase> make_pipeline_cases()
    {
        std::vector<PipelineCase> v;
        std::string a = build("POST", {"Content-Length: 2"}, "aa");
        std::string b = build("POST", {"Content-Length: 4"}, "bbbb");
        std::string c = build("GET", {"Host: x"}, "");
        v.push_back({"two_post", a + b, a.size()});
        v.push_back({"three_mixed", a + b + c, a.size()});
        v.push_back({"post_then_get", a + c, a.size()});
        v.push_back({"get_then_post", c + a, c.size()});
        v.push_back({"first_plus_partial_next", a + b.substr(0, b.size() - 2), a.size()});
        v.push_back({"first_plus_one_byte", a + "P", a.size()});
        return v;
    }
} // namespace

class HttpComplete : public ::testing::TestWithParam<CompleteCase> {};
TEST_P(HttpComplete, FramesExactly)
{
    const auto& c = GetParam();
    Message m;
    ASSERT_EQ(parse(c.raw, m), ParseStatus::Complete) << c.name;
    const size_t hdr = c.raw.find("\r\n\r\n") + 4;
    EXPECT_EQ(m.header_len, hdr) << c.name;
    EXPECT_EQ(m.body_len, c.body_len) << c.name;
    EXPECT_EQ(m.total_len, hdr + c.body_len) << c.name;
    EXPECT_EQ(m.total_len, c.raw.size()) << c.name;
    EXPECT_EQ(m.keep_alive, c.keep_alive) << c.name;
}
INSTANTIATE_TEST_SUITE_P(Cases, HttpComplete, ::testing::ValuesIn(make_complete_cases()),
                         [](const testing::TestParamInfo<CompleteCase>& i) { return i.param.name; });

class HttpNeedMore : public ::testing::TestWithParam<RawCase> {};
TEST_P(HttpNeedMore, ReturnsNeedMore)
{
    Message m;
    EXPECT_EQ(parse(GetParam().raw, m), ParseStatus::NeedMore) << GetParam().name;
}
INSTANTIATE_TEST_SUITE_P(Cases, HttpNeedMore, ::testing::ValuesIn(make_needmore_cases()),
                         [](const testing::TestParamInfo<RawCase>& i) { return i.param.name; });

class HttpError : public ::testing::TestWithParam<RawCase> {};
TEST_P(HttpError, ReturnsError)
{
    Message m;
    EXPECT_EQ(parse(GetParam().raw, m), ParseStatus::Error) << GetParam().name;
}
INSTANTIATE_TEST_SUITE_P(Cases, HttpError, ::testing::ValuesIn(make_error_cases()),
                         [](const testing::TestParamInfo<RawCase>& i) { return i.param.name; });

class HttpPipeline : public ::testing::TestWithParam<PipelineCase> {};
TEST_P(HttpPipeline, FramesOnlyFirstMessage)
{
    const auto& c = GetParam();
    Message m;
    ASSERT_EQ(parse(c.raw, m), ParseStatus::Complete) << c.name;
    EXPECT_EQ(m.total_len, c.first_total) << c.name;
    EXPECT_LT(m.total_len, c.raw.size()) << c.name;
}
INSTANTIATE_TEST_SUITE_P(Cases, HttpPipeline, ::testing::ValuesIn(make_pipeline_cases()),
                         [](const testing::TestParamInfo<PipelineCase>& i) { return i.param.name; });

// ── Incremental arrival (byte-by-byte property) ─────────────────────────────
struct IncCase { std::string name; std::string raw; size_t prefix; };
static std::vector<IncCase> make_incremental_cases()
{
    std::vector<IncCase> v;
    const std::vector<std::pair<std::string, std::string>> reqs = {
        {"post_cl5", build("POST", {"Host: example.com", "Content-Length: 5"}, "hello")},
        {"get_nobody", build("GET", {"Host: x", "Accept: */*"}, "")},
        {"post_cl0", build("POST", {"Connection: close", "Content-Length: 0"}, "")},
    };
    for (const auto& [tag, raw] : reqs)
        for (size_t p = 1; p <= raw.size(); ++p)
            v.push_back({tag + "_" + std::to_string(p), raw, p});
    return v;
}
class HttpIncremental : public ::testing::TestWithParam<IncCase> {};
TEST_P(HttpIncremental, EveryProperPrefixIsNeedMore)
{
    const auto& c = GetParam();
    Message m;
    auto st = parse(std::string_view(c.raw).substr(0, c.prefix), m);
    if (c.prefix < c.raw.size())
        EXPECT_EQ(st, ParseStatus::NeedMore) << c.name;
    else
        EXPECT_EQ(st, ParseStatus::Complete) << c.name;
}
INSTANTIATE_TEST_SUITE_P(Cases, HttpIncremental, ::testing::ValuesIn(make_incremental_cases()),
                         [](const testing::TestParamInfo<IncCase>& i) { return i.param.name; });

// ── Lenient / documented quirks ─────────────────────────────────────────────
// --- Framing-desync regressions (security sweep, 2026-08-03) ------------------
//
// Every case below was MEASURED against the pre-fix framer. Six of the seven had
// the identical signature: parse() returned Complete with body_len == 0 while the
// body sat unconsumed in the buffer. In passthrough mode the gateway then forwards
// the header block verbatim — malformed header included — so an upstream that
// reads the length we could not becomes a desync, and because the upstream pool is
// SHARED, that desync crosses clients. Hence: refuse, never re-interpret.
//
// The first of these replaces a test that asserted the opposite ("trailing garbage
// after the CL number is accepted"). It was written as a documented quirk; it was
// a smuggling primitive.

namespace
{
    // Each input carries a 5-byte body the framer must NOT silently frame as empty.
    void expect_rejected(const std::string& raw, const char* why)
    {
        Message m;
        EXPECT_EQ(parse(raw, m), ParseStatus::Error) << why;
    }
} // namespace

TEST(HttpDesync, NonNumericContentLengthIsRejected)
{
    // std::from_chars stops at the first non-digit and still reports success, so
    // "5x" parsed as 5 and "0x1b" as 0 — the latter framing a 27-byte body empty.
    expect_rejected(build("POST", {"Content-Length: 5x"}, "hello"), "trailing garbage");
    expect_rejected(build("POST", {"Content-Length: 0x5"}, "hello"), "hex-looking");
    expect_rejected(build("POST", {"Content-Length: +5"}, "hello"), "leading plus");
    expect_rejected(build("POST", {"Content-Length: "}, "hello"), "empty value");
    expect_rejected(build("POST", {"Content-Length: 5 5"}, "hello"), "embedded space");
}

TEST(HttpDesync, WhitespaceBeforeColonIsRejected)
{
    // "Content-Length : 5" matches no `name:` prefix test in the framer, so the
    // length went unseen; RFC 9112 §5.1 forbids it for exactly this reason.
    expect_rejected(build("POST", {"Content-Length : 5"}, "hello"), "space before colon");
    expect_rejected(build("POST", {"Content-Length\t: 5"}, "hello"), "tab before colon");
}

TEST(HttpDesync, BareCrOrLfInHeaderBlockIsRejected)
{
    // A parser that treats a bare CR/LF as a line terminator sees a Content-Length
    // we never saw. Measured reaching an upstream.
    expect_rejected(build("POST", {"X-A: 1\rContent-Length: 5"}, "hello"), "bare CR");
    expect_rejected(build("POST", {"X-A: 1\nContent-Length: 5"}, "hello"), "bare LF");
}

TEST(HttpDesync, ObsFoldContinuationIsRejected)
{
    // A folded Content-Length is invisible to a prefix matcher, honoured by a
    // folding upstream. RFC 9112 §5.2 requires rejection on receipt.
    expect_rejected(build("POST", {"X-A: 1", " Content-Length: 5"}, "hello"), "obs-fold SP");
    expect_rejected(build("POST", {"X-A: 1", "\tContent-Length: 5"}, "hello"), "obs-fold HTAB");
}

TEST(HttpDesync, HeaderLineWithoutColonIsRejected)
{
    expect_rejected(build("POST", {"NotAHeader"}, "hello"), "no colon");
    expect_rejected(build("POST", {": novalue"}, "hello"), "empty field name");
}

TEST(HttpDesync, LegalContentLengthFormsStillAccepted)
{
    // The fix must not over-reject: trailing OWS is legal (RFC 9110 trims it), and
    // a plain length obviously is. A framer that rejects these breaks real clients.
    Message m;
    ASSERT_EQ(parse(build("POST", {"Content-Length: 5 "}, "hello"), m), ParseStatus::Complete);
    EXPECT_EQ(m.body_len, 5u);
    ASSERT_EQ(parse(build("POST", {"Content-Length:5"}, "hello"), m), ParseStatus::Complete);
    EXPECT_EQ(m.body_len, 5u);
}

TEST(HttpDesync, ResponseWithBothChunkedAndContentLengthIsRejected)
{
    // The two framings disagree about where the body ends. Whichever we pick, the
    // other is what some intermediary picked — and a mis-framed response leaves
    // stray bytes on the POOLED connection, i.e. in the next client's response.
    const std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                            "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    llmbridge::http::ResponseHead h;
    EXPECT_EQ(llmbridge::http::parse_response_head(raw, h), llmbridge::http::HeadStatus::Error);
}

TEST(HttpDesync, ResponseConflictingDuplicateContentLengthIsRejected)
{
    const std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 9\r\n\r\nhello";
    llmbridge::http::ResponseHead h;
    EXPECT_EQ(llmbridge::http::parse_response_head(raw, h), llmbridge::http::HeadStatus::Error);
}

TEST(HttpDesync, ResponseBareCrAndBadLengthAreRejected)
{
    llmbridge::http::ResponseHead h;
    EXPECT_EQ(llmbridge::http::parse_response_head("HTTP/1.1 200 OK\r\nX-A: 1\rContent-Length: 5\r\n\r\nhello", h),
              llmbridge::http::HeadStatus::Error);
    EXPECT_EQ(llmbridge::http::parse_response_head("HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\nhello", h),
              llmbridge::http::HeadStatus::Error);
}
TEST(HttpQuirk, TrailingSpaceAfterClNumberIsAccepted)
{
    Message m;
    ASSERT_EQ(parse(build("POST", {"Content-Length: 5 "}, "hello"), m), ParseStatus::Complete);
    EXPECT_EQ(m.body_len, 5u);
}
TEST(HttpQuirk, DuplicateContentLengthIdenticalIsAccepted)
{
    // Identical repeats are harmless and collapse to one (a conflicting duplicate
    // is rejected — see the HttpError `dup_content_length_conflict` case).
    Message m;
    ASSERT_EQ(parse(build("POST", {"Content-Length: 5", "Content-Length: 5"}, "hello"), m),
              ParseStatus::Complete);
    EXPECT_EQ(m.body_len, 5u);
}
TEST(HttpQuirk, BodyLengthAtCapIsNotError)
{
    // A CL exactly at the cap frames normally (NeedMore until the body arrives),
    // proving the guard is a true upper bound, not off-by-one strict.
    Message m;
    const std::string raw =
        "POST / HTTP/1.1\r\nContent-Length: " + std::to_string(llmbridge::http::kMaxBodyLen) + "\r\n\r\n";
    EXPECT_NE(parse(raw, m), ParseStatus::Error);
}
TEST(HttpQuirk, ConnectionClosedPrefixMatchesClose)
{
    Message m;
    ASSERT_EQ(parse(build("POST", {"Connection: closed", "Content-Length: 1"}, "x"), m),
              ParseStatus::Complete);
    EXPECT_FALSE(m.keep_alive);
}
TEST(HttpQuirk, IdempotentReparseGivesSameResult)
{
    std::string raw = build("POST", {"Content-Length: 4"}, "abcd");
    Message m1, m2;
    EXPECT_EQ(parse(raw, m1), parse(raw, m2));
    EXPECT_EQ(m1.total_len, m2.total_len);
    EXPECT_EQ(m1.body_len, m2.body_len);
    EXPECT_EQ(m1.keep_alive, m2.keep_alive);
}
TEST(HttpQuirk, ZeroLengthBodyFramesAtHeaderEnd)
{
    Message m;
    ASSERT_EQ(parse(build("POST", {"Content-Length: 0"}, ""), m), ParseStatus::Complete);
    EXPECT_EQ(m.body_len, 0u);
    EXPECT_EQ(m.total_len, m.header_len);
}
TEST(HttpQuirk, LargePaddingHeaderUnderCapIsNotError)
{
    Message m;
    EXPECT_NE(parse(build("GET", {"X-Pad: " + std::string(1024, 'p')}, ""), m), ParseStatus::Error);
}

// ── find_header ──────────────────────────────────────────────────────────────

TEST(FindHeader, CaseInsensitiveNameLookupTrimsValue)
{
    const std::string_view h = "Host: x\r\nX-API-Key:   sk-123\r\nContent-Length: 4\r\n";
    EXPECT_EQ(llmbridge::http::find_header(h, "x-api-key:"), "sk-123");
    EXPECT_EQ(llmbridge::http::find_header(h, "host:"), "x");
}

TEST(FindHeader, MissingHeaderIsEmpty)
{
    EXPECT_TRUE(llmbridge::http::find_header("Host: x\r\n", "authorization:").empty());
    EXPECT_TRUE(llmbridge::http::find_header("", "authorization:").empty());
}

TEST(FindHeader, FirstOccurrenceWinsOnDuplicates)
{
    // Anti-smuggling: duplicated credentials must resolve deterministically.
    const std::string_view h = "x-api-key: first\r\nx-api-key: second\r\n";
    EXPECT_EQ(llmbridge::http::find_header(h, "x-api-key:"), "first");
}

TEST(FindHeader, ColonInNameStopsPrefixConfusion)
{
    const std::string_view h = "x-api-key-2: wrong\r\nx-api-key: right\r\n";
    EXPECT_EQ(llmbridge::http::find_header(h, "x-api-key:"), "right");
}

TEST(FindHeader, CrlfEndsTheValue)
{
    const std::string_view h = "x-api-key: k\r\nX-Evil: 1\r\n";
    EXPECT_EQ(llmbridge::http::find_header(h, "x-api-key:"), "k");
}

// Documents the SHARP EDGE deliberately: a bare CR does NOT terminate a line, so
// it survives inside the value. Callers must validate before re-emitting; this
// test exists so nobody "fixes" the comment back to claiming otherwise.
TEST(FindHeader, BareCrSurvivesInsideValueSoCallersMustValidate)
{
    const std::string_view h = "x-api-key: k\rX-Smuggled: 1\r\nHost: a\r\n";
    const auto v = llmbridge::http::find_header(h, "x-api-key:");
    EXPECT_NE(v.find('\r'), std::string_view::npos)
        << "if this ever passes, find_header changed and gateway validation may be stale";
}

// --- Non-streaming chunked decode: correctness + cost (security sweep) --------
//
// The chunked response path is re-entered on every read. It used to build a fresh
// ChunkDecoder and re-decode from byte zero, making an N-read body cost O(N*body):
// 8 MB took 79 ms of straight-line CPU on the single-threaded loop, which is
// head-of-line blocking for every other client on that worker.

namespace
{
    std::string make_chunked_response(size_t total, size_t chunk)
    {
        std::string s = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
        for (size_t o = 0; o < total; o += chunk)
        {
            const size_t n = std::min(chunk, total - o);
            char hdr[32];
            std::snprintf(hdr, sizeof hdr, "%zx\r\n", n);
            s += hdr;
            s.append(n, 'x');
            s += "\r\n";
        }
        return s + "0\r\n\r\n";
    }

    // Drive parse_response the way the gateway does: append a read, re-parse.
    llmbridge::http::ParsedResponse drive(const std::string& wire, size_t read_size,
                                          llmbridge::http::ResponseDecoder& st,
                                          std::string& rbuf)
    {
        llmbridge::http::ParsedResponse r;
        for (size_t off = 0; off < wire.size(); off += read_size)
        {
            rbuf.append(wire, off, std::min(read_size, wire.size() - off));
            r = llmbridge::http::parse_response(rbuf, st);
            if (r.status == llmbridge::http::RespStatus::Error) return r;
        }
        return r;
    }
} // namespace

TEST(HttpChunkedResponse, IncrementalArrivalDecodesExactlyOnce)
{
    const size_t body = 512 * 1024;
    const std::string wire = make_chunked_response(body, 16 * 1024);
    llmbridge::http::ResponseDecoder st;
    std::string rbuf;
    const auto r = drive(wire, 64 * 1024, st, rbuf);

    ASSERT_EQ(r.status, llmbridge::http::RespStatus::Complete);
    EXPECT_EQ(r.body.size(), body);
    EXPECT_EQ(r.body.find_first_not_of('x'), std::string::npos);
    EXPECT_EQ(r.total_len, wire.size());
    // Every post-header byte handed to the decoder exactly once.
    EXPECT_EQ(st.dec.consumed(), wire.size() - r.head.header_len);
}

TEST(HttpChunkedResponse, ResetAllowsReuseOnAPooledConnection)
{
    // A pooled upstream serves many responses. Without reset() the second decode
    // would resume mid-stream and mis-frame — i.e. serve one client another's bytes.
    llmbridge::http::ResponseDecoder st;
    for (int i = 0; i < 3; ++i)
    {
        const std::string wire = make_chunked_response(1024 * (i + 1), 256);
        std::string rbuf;
        const auto r = drive(wire, 300, st, rbuf);
        ASSERT_EQ(r.status, llmbridge::http::RespStatus::Complete) << "iteration " << i;
        EXPECT_EQ(r.body.size(), size_t(1024 * (i + 1)));
        st.reset();
    }
}

TEST(HttpChunkedResponse, NoByteIsDecodedTwice)
{
    // The deterministic form of "not quadratic". A timing assertion was tried
    // first and flaked on allocator warmth, which is exactly the kind of test
    // people learn to ignore — so assert the invariant that actually matters:
    // across the whole incremental arrival, the decoder's consumed count only
    // ever moves FORWARD, and each step is bounded by the bytes that just
    // arrived. Re-decoding from byte zero violates both.
    const size_t body = 1024 * 1024, read_size = 64 * 1024;
    const std::string wire = make_chunked_response(body, 16 * 1024);

    llmbridge::http::ResponseDecoder st;
    std::string rbuf;
    size_t prev = 0, total_steps = 0;
    for (size_t off = 0; off < wire.size(); off += read_size)
    {
        rbuf.append(wire, off, std::min(read_size, wire.size() - off));
        const auto r = llmbridge::http::parse_response(rbuf, st);
        ASSERT_NE(r.status, llmbridge::http::RespStatus::Error);
        const size_t now = st.dec.consumed();
        EXPECT_GE(now, prev) << "decoder went backwards — re-decoding from the start";
        EXPECT_LE(now - prev, read_size) << "consumed more than just arrived — re-fed old bytes";
        total_steps += now - prev;
        prev = now;
    }
    // Total decode work equals the payload exactly: no byte seen twice.
    EXPECT_EQ(total_steps, wire.size() - sizeof("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n") + 1);
}
