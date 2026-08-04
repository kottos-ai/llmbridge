// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Tests for the streaming-response framer additions (net/http.hpp):
// parse_response_head() (detect text/event-stream + chunked) and the incremental
// ChunkDecoder. The decoder is replayed byte-by-byte to prove it tolerates a
// chunk header/data/CRLF splitting across arbitrary reads.

#include "net/http.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <string_view>

using llmbridge::net::http::ChunkDecoder;
using llmbridge::net::http::FrameStatus;
using llmbridge::net::http::parse_response_head;
using llmbridge::net::http::ResponseHead;

namespace
{
    // HTTP/1.1 chunked wire form of `payload`, split into `chunk`-sized chunks.
    std::string chunk_encode(std::string_view payload, size_t chunk)
    {
        std::string s;
        size_t i = 0;
        while (i < payload.size())
        {
            const size_t n = std::min(chunk, payload.size() - i);
            char hex[32];
            std::snprintf(hex, sizeof hex, "%zx", n);
            s += hex;
            s += "\r\n";
            s.append(payload.substr(i, n));
            s += "\r\n";
            i += n;
        }
        s += "0\r\n\r\n";
        return s;
    }

    std::string decode_whole(std::string_view wire, bool& ok, bool& done)
    {
        ChunkDecoder d;
        std::string out;
        ok = d.feed(wire, out);
        done = d.done();
        return out;
    }

    std::string decode_bytewise(std::string_view wire, bool& ok, bool& done)
    {
        ChunkDecoder d;
        std::string out;
        ok = true;
        for (char c : wire)
            if (!d.feed(std::string_view(&c, 1), out)) { ok = false; break; }
        done = d.done();
        return out;
    }
} // namespace

// ── parse_response_head ─────────────────────────────────────────────────────

TEST(RespHead, DetectsEventStreamAndChunked)
{
    ResponseHead h;
    auto st = parse_response_head(
        "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\ntransfer-encoding: chunked\r\n\r\n", h);
    EXPECT_EQ(st, FrameStatus::Complete);
    EXPECT_EQ(h.status, 200);
    EXPECT_TRUE(h.event_stream);
    EXPECT_TRUE(h.chunked);
    EXPECT_FALSE(h.has_content_length);
    EXPECT_TRUE(h.keep_alive);
}

TEST(RespHead, EventStreamWithCharsetParam)
{
    ResponseHead h;
    parse_response_head("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n\r\n", h);
    EXPECT_TRUE(h.event_stream);
}

TEST(RespHead, PlainJsonResponseIsNotStreaming)
{
    ResponseHead h;
    auto st = parse_response_head(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 42\r\n\r\n", h);
    EXPECT_EQ(st, FrameStatus::Complete);
    EXPECT_FALSE(h.event_stream);
    EXPECT_FALSE(h.chunked);
    EXPECT_TRUE(h.has_content_length);
    EXPECT_EQ(h.content_length, 42u);
}

TEST(RespHead, ConnectionCloseFlipsKeepAlive)
{
    ResponseHead h;
    parse_response_head("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n", h);
    EXPECT_FALSE(h.keep_alive);
}

TEST(RespHead, NeedMoreUntilHeadersComplete)
{
    ResponseHead h;
    EXPECT_EQ(parse_response_head("HTTP/1.1 200 OK\r\nContent-Type: text/event-str", h), FrameStatus::NeedMore);
    EXPECT_EQ(parse_response_head("", h), FrameStatus::NeedMore);
}

// ── ChunkDecoder ────────────────────────────────────────────────────────────

TEST(Chunked, BasicDecode)
{
    bool ok = false, done = false;
    EXPECT_EQ(decode_whole("5\r\nHello\r\n6\r\n world\r\n0\r\n\r\n", ok, done), "Hello world");
    EXPECT_TRUE(ok);
    EXPECT_TRUE(done);
}

TEST(Chunked, ByteByByteMatchesWhole)
{
    // A realistic Anthropic-ish SSE body carried in chunked encoding.
    const std::string sse =
        "event: message_start\ndata: {\"type\":\"message_start\"}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\","
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    const std::string wire = chunk_encode(sse, 7); // small chunks -> many splits

    bool ok1 = false, d1 = false, ok2 = false, d2 = false;
    const std::string whole = decode_whole(wire, ok1, d1);
    const std::string bytewise = decode_bytewise(wire, ok2, d2);
    EXPECT_TRUE(ok1 && d1);
    EXPECT_TRUE(ok2 && d2);
    EXPECT_EQ(whole, sse);      // decoding recovers the exact payload
    EXPECT_EQ(bytewise, sse);   // ...regardless of read boundaries
}

TEST(Chunked, VariousChunkSizesRoundTrip)
{
    std::string payload;
    for (int i = 0; i < 500; ++i) payload += "token" + std::to_string(i) + " ";
    for (size_t cs : {1u, 2u, 13u, 64u, 255u, 4096u})
    {
        bool ok = false, done = false;
        EXPECT_EQ(decode_whole(chunk_encode(payload, cs), ok, done), payload) << "chunk size " << cs;
        EXPECT_TRUE(ok && done);
    }
}

TEST(Chunked, ChunkExtensionsIgnored)
{
    bool ok = false, done = false;
    EXPECT_EQ(decode_whole("5;name=value\r\nHello\r\n0\r\n\r\n", ok, done), "Hello");
    EXPECT_TRUE(ok && done);
}

TEST(Chunked, TrailerHeadersConsumed)
{
    bool ok = false, done = false;
    EXPECT_EQ(decode_whole("5\r\nHello\r\n0\r\nX-Trailer: v\r\n\r\n", ok, done), "Hello");
    EXPECT_TRUE(ok && done);
}

TEST(Chunked, UppercaseHexSize)
{
    bool ok = false, done = false;
    // 0x1A = 26 bytes
    const std::string data(26, 'z');
    EXPECT_EQ(decode_whole("1A\r\n" + data + "\r\n0\r\n\r\n", ok, done), data);
    EXPECT_TRUE(ok && done);
}

TEST(Chunked, NotDoneUntilTerminator)
{
    ChunkDecoder d;
    std::string out;
    EXPECT_TRUE(d.feed("5\r\nHello\r\n", out)); // one chunk, no 0-terminator yet
    EXPECT_EQ(out, "Hello");
    EXPECT_FALSE(d.done());
    EXPECT_TRUE(d.feed("0\r\n\r\n", out));
    EXPECT_TRUE(d.done());
}

TEST(Chunked, MalformedSizeIsError)
{
    ChunkDecoder d;
    std::string out;
    EXPECT_FALSE(d.feed("zz\r\n", out)); // no hex digits
    EXPECT_TRUE(d.failed());
}

TEST(Chunked, MissingCrlfAfterDataIsError)
{
    ChunkDecoder d;
    std::string out;
    EXPECT_FALSE(d.feed("5\r\nHelloXX", out)); // data not followed by CRLF
    EXPECT_TRUE(d.failed());
}

TEST(Chunked, AbsurdSizeLineIsError)
{
    ChunkDecoder d;
    std::string out;
    EXPECT_FALSE(d.feed(std::string(100, 'a') + "\r\n", out)); // > 64-char size line
    EXPECT_TRUE(d.failed());
}
