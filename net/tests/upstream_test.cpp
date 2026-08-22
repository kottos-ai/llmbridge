// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "net/upstream.hpp"

#include <gtest/gtest.h>

using llmbridge::net::parse_upstream;
using llmbridge::net::resolve_host_ipv4;

// ── accepted forms ───────────────────────────────────────────────────────────

TEST(UpstreamParse, LegacyIpPortStillWorks)
{
    const auto s = parse_upstream("127.0.0.1:9001");
    ASSERT_TRUE(s.ok()) << s.error;
    EXPECT_EQ(s.host, "127.0.0.1");
    EXPECT_EQ(s.port, 9001);
    EXPECT_FALSE(s.tls);
}

TEST(UpstreamParse, HostPortIsPlainHttp)
{
    const auto s = parse_upstream("mock.internal:9001");
    ASSERT_TRUE(s.ok()) << s.error;
    EXPECT_EQ(s.host, "mock.internal");
    EXPECT_EQ(s.port, 9001);
    EXPECT_FALSE(s.tls);
}

TEST(UpstreamParse, HttpsDefaultsTo443)
{
    const auto s = parse_upstream("https://api.anthropic.com");
    ASSERT_TRUE(s.ok()) << s.error;
    EXPECT_EQ(s.host, "api.anthropic.com");
    EXPECT_EQ(s.port, 443);
    EXPECT_TRUE(s.tls);
}

TEST(UpstreamParse, HttpDefaultsTo80)
{
    const auto s = parse_upstream("http://mock.internal");
    ASSERT_TRUE(s.ok()) << s.error;
    EXPECT_EQ(s.port, 80);
    EXPECT_FALSE(s.tls);
}

TEST(UpstreamParse, ExplicitPortBeatsSchemeDefault)
{
    const auto s = parse_upstream("https://api.openai.com:8443");
    ASSERT_TRUE(s.ok()) << s.error;
    EXPECT_EQ(s.port, 8443);
    EXPECT_TRUE(s.tls);
}

TEST(UpstreamParse, BareTrailingSlashTolerated)
{
    const auto s = parse_upstream("https://api.anthropic.com/");
    ASSERT_TRUE(s.ok()) << s.error;
    EXPECT_EQ(s.host, "api.anthropic.com");
}

// ── rejected forms; each with a specific reason, not a generic failure ──────

// Was RejectsBasePath until 0.19.0. A base path is now KEPT and prefixed to the
// request target, so the case that used to be refused is the Groq/OpenRouter shape.
TEST(UpstreamParse, KeepsABasePathAndTheAuthorityWithIt)
{
    const auto s = parse_upstream("https://api.openai.com/v1");
    ASSERT_TRUE(s.ok()) << s.error;
    EXPECT_EQ(s.host, "api.openai.com");
    EXPECT_EQ(s.port, 443);
    EXPECT_EQ(s.path, "/v1");
}

// https://good.example@evil.example: the eyeball reads "good", the connect goes
// to "evil". Refusing '@' outright kills the whole class.
TEST(UpstreamParse, RejectsUserinfo)
{
    const auto s = parse_upstream("https://api.openai.com@evil.example");
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.error.find("userinfo"), std::string::npos);
}

TEST(UpstreamParse, RejectsIpv6Literal)
{
    EXPECT_FALSE(parse_upstream("[::1]:8080").ok());
    EXPECT_FALSE(parse_upstream("https://[2001:db8::1]:443").ok());
}

TEST(UpstreamParse, RejectsUnsupportedScheme)
{
    EXPECT_FALSE(parse_upstream("ftp://host:21").ok());
    EXPECT_FALSE(parse_upstream("ws://host:80").ok());
}

TEST(UpstreamParse, RejectsBarePortlessHost)
{
    // The legacy contract was IP:PORT; a bare host with no scheme has no port to
    // default to, and guessing 80 would surprise the IP:PORT muscle memory.
    EXPECT_FALSE(parse_upstream("api.openai.com").ok());
}

TEST(UpstreamParse, RejectsBadPorts)
{
    EXPECT_FALSE(parse_upstream("host:0").ok());
    EXPECT_FALSE(parse_upstream("host:65536").ok());
    EXPECT_FALSE(parse_upstream("host:80x").ok());   // atoi would have taken this
    EXPECT_FALSE(parse_upstream("host:").ok());
    EXPECT_FALSE(parse_upstream("host:-1").ok());
}

// The host string is later written into an HTTP Host header and the TLS SNI
// field. A CR/LF (or space, or slash smuggled via percent-encoding) that survived
// parsing would be a header-injection primitive, so the charset gate IS a
// security boundary and gets tested as one.
TEST(UpstreamParse, RejectsHeaderInjectionChars)
{
    EXPECT_FALSE(parse_upstream("host\r\nX-Evil: 1:80").ok());
    EXPECT_FALSE(parse_upstream("ho st:80").ok());
    EXPECT_FALSE(parse_upstream("host%2f:80").ok());
    EXPECT_FALSE(parse_upstream("host_name:80").ok());  // '_' outside LDH on purpose
}

TEST(UpstreamParse, RejectsMalformedHosts)
{
    EXPECT_FALSE(parse_upstream(":9001").ok());          // empty host
    EXPECT_FALSE(parse_upstream("a..b:80").ok());        // empty label
    EXPECT_FALSE(parse_upstream(".host:80").ok());
    EXPECT_FALSE(parse_upstream("host.:80").ok());
    EXPECT_FALSE(parse_upstream("-host:80").ok());
    EXPECT_FALSE(parse_upstream("").ok());
    EXPECT_FALSE(parse_upstream(std::string(300, 'a') + ":80").ok());  // > 253
    EXPECT_FALSE(parse_upstream("https://?q=1").ok());
}

// ── resolution: hermetic cases only ─────────────────────────────────────────
// localhost and numeric literals resolve without a network round-trip; an
// NXDOMAIN test would hit real DNS and is deliberately absent (tests must be
// hermetic per project policy).

TEST(UpstreamResolve, NumericLiteralPassesThroughWithoutResolver)
{
    std::string err;
    const auto ips = resolve_host_ipv4("192.0.2.7", &err);
    ASSERT_EQ(ips.size(), 1u) << err;
    EXPECT_EQ(ips[0], "192.0.2.7");
}

TEST(UpstreamResolve, LocalhostResolvesToLoopback)
{
    std::string err;
    const auto ips = resolve_host_ipv4("localhost", &err);
    ASSERT_FALSE(ips.empty()) << err;
    EXPECT_EQ(ips[0].rfind("127.", 0), 0u) << ips[0];
}

// ── base path ───────────────────────────────────────────────────────────────
//
// A prefix for providers serving an OpenAI-compatible API below the root. It is
// spliced into a request line, so the rejection cases below matter more than the
// acceptance ones: the pooled upstream means a target one client can retarget is
// a target ANOTHER client's request can be sent to.

TEST(ParseUpstream, KeepsABasePath)
{
    const auto s = parse_upstream("https://api.groq.com/openai");
    ASSERT_TRUE(s.ok()) << s.error;
    EXPECT_EQ(s.host, "api.groq.com");
    EXPECT_EQ(s.port, 443);
    EXPECT_TRUE(s.tls);
    EXPECT_EQ(s.path, "/openai");
}

TEST(ParseUpstream, ABasePathMayHaveSeveralSegmentsAndAPort)
{
    const auto s = parse_upstream("http://host.internal:9001/a/b-c/d_e");
    ASSERT_TRUE(s.ok()) << s.error;
    EXPECT_EQ(s.host, "host.internal");
    EXPECT_EQ(s.port, 9001);
    EXPECT_EQ(s.path, "/a/b-c/d_e");
}

TEST(ParseUpstream, DropsATrailingSlashSoTheJoinCannotDoubleIt)
{
    EXPECT_EQ(parse_upstream("https://openrouter.ai/api/").path, "/api");
    EXPECT_EQ(parse_upstream("https://api.anthropic.com/").path, "");
}

TEST(ParseUpstream, RejectsABasePathWithoutAScheme)
{
    // "host:9001/x" reads as a path here and as something else in half the world's
    // URL parsers, and the legacy form never had one.
    const auto s = parse_upstream("host.internal:9001/openai");
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.error.find("http"), std::string::npos) << s.error;
}

// The injection cases. Each of these would end or retarget the request line.
TEST(ParseUpstream, RejectsControlBytesInABasePath)
{
    for (const std::string bad : {"https://h.io/a\rb", "https://h.io/a\nb",
                                  "https://h.io/a b", "https://h.io/a\tb",
                                  "https://h.io/a\r\nX-Evil: 1"})
    {
        const auto s = parse_upstream(bad);
        EXPECT_FALSE(s.ok()) << "accepted " << bad << " as path '" << s.path << "'";
    }
}

TEST(ParseUpstream, RejectsQueryAndFragmentInABasePath)
{
    // Azure OpenAI needs "?api-version=". It is now PARSED and kept: a translating
    // mode builds its own request target and has no client query to merge with. The
    // pairing that would need merging, a query on a byte-forwarding venue, is refused
    // by the Gateway at startup, where the mode is known.
    const auto q = parse_upstream("https://h.io/openai/deployments/gpt-4o?api-version=2024-02-01");
    ASSERT_TRUE(q.ok()) << q.error;
    EXPECT_EQ(q.path, "/openai/deployments/gpt-4o");
    EXPECT_EQ(q.query, "api-version=2024-02-01");
    EXPECT_TRUE(q.tls);

    // The query is split off BEFORE the path is normalised, so no '?' can reach a
    // request-line path.
    EXPECT_EQ(parse_upstream("https://h.io/x?a=1&b=2").query, "a=1&b=2");
    EXPECT_EQ(parse_upstream("https://h.io/x?a=1&b=2").path, "/x");
    EXPECT_TRUE(parse_upstream("https://h.io/x").query.empty());

    // A fragment never travels on the wire, so it is a paste error worth naming.
    EXPECT_FALSE(parse_upstream("https://h.io/openai#frag").ok());
    EXPECT_FALSE(parse_upstream("https://h.io/x?a=1#frag").ok());
    // The query lands in a request line, so the same charset reasoning as the path
    // applies: anything that could split or retarget the line is refused.
    EXPECT_FALSE(parse_upstream("https://h.io/x?a=1 HTTP/1.1").ok());
    EXPECT_FALSE(parse_upstream("https://h.io/x?a=\r\nX: y").ok());
}

TEST(ParseUpstream, RejectsDotAndEmptySegments)
{
    for (const std::string bad : {"https://h.io/a/../b", "https://h.io/a/./b",
                                  "https://h.io/a//b", "https://h.io/..", "https://h.io/a/.."})
        EXPECT_FALSE(parse_upstream(bad).ok()) << "accepted " << bad;
}

TEST(ParseUpstream, RejectsPercentEncodingInABasePath)
{
    // %2e%2e and %2f are how a dot segment or a separator gets past a check that
    // only looks at literal characters.
    EXPECT_FALSE(parse_upstream("https://h.io/a/%2e%2e/b").ok());
    EXPECT_FALSE(parse_upstream("https://h.io/a%2fb").ok());
}
