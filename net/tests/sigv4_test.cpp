// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "net/sigv4.hpp"

#include <gtest/gtest.h>

// AWS's own published values, not numbers this implementation produced. A signing
// routine that agrees with itself proves nothing: the failure mode is a 403 with an
// empty body, so the only useful check is against the authority.
//
// Both vectors below were reproduced independently before being written down here,
// so a disagreement means this code is wrong, not that a constant was mistyped.
namespace
{
    using namespace llmbridge::net;

    constexpr std::string_view kSecret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    constexpr std::string_view kAccess = "AKIDEXAMPLE";

    std::string hex(std::string_view raw)
    {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string out;
        for (const unsigned char c : raw)
        {
            out.push_back(kDigits[c >> 4]);
            out.push_back(kDigits[c & 0x0f]);
        }
        return out;
    }

    std::string signing_key(std::string_view day, std::string_view region,
                            std::string_view service)
    {
        std::string k = "AWS4";
        k.append(kSecret);
        return sigv4::hmac_sha256(
            sigv4::hmac_sha256(
                sigv4::hmac_sha256(sigv4::hmac_sha256(k, day), region), service),
            "aws4_request");
    }
}

TEST(SigV4, Sha256MatchesTheEmptyStringConstant)
{
    // The payload hash of an empty body appears in nearly every AWS example, so a
    // wrong SHA-256 would surface here, and not deep inside a signature.
    EXPECT_EQ(sigv4::sha256_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(SigV4, SigningKeyChainMatchesTheAwsDocumentedExample)
{
    // From AWS's "Task 3: Calculate the signature" walkthrough: secret above,
    // 20150830 / us-east-1 / iam.
    EXPECT_EQ(hex(signing_key("20150830", "us-east-1", "iam")),
              "c4afb1cc5771d871763a393e44b703571b55cc28424d1a5e86da6ed3c154a4b9");
}

TEST(SigV4, GetVanillaFromTheAwsTestSuite)
{
    // aws-sig-v4-test-suite / get-vanilla. No body, no query, two signed headers.
    sigv4::Request r{};
    r.method = "GET";
    r.path = "/";
    r.host = "example.amazonaws.com";
    r.region = "us-east-1";
    r.service = "service";
    r.amz_date = "20150830T123600Z";

    const std::string creq =
        sigv4::canonical_request(r, sigv4::sha256_hex(""), {});
    EXPECT_EQ(creq,
              "GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\n"
              "host;x-amz-date\n"
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const std::string sts =
        "AWS4-HMAC-SHA256\n20150830T123600Z\n"
        "20150830/us-east-1/service/aws4_request\n" +
        sigv4::sha256_hex(creq);
    EXPECT_EQ(hex(sigv4::hmac_sha256(signing_key("20150830", "us-east-1", "service"), sts)),
              "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
}

TEST(SigV4, SignProducesThatSameSignatureEndToEnd)
{
    // The whole path, so the assembled header cannot drift from the pieces above.
    sigv4::Request r{};
    r.method = "GET";
    r.path = "/";
    r.host = "example.amazonaws.com";
    r.region = "us-east-1";
    r.service = "service";
    r.amz_date = "20150830T123600Z";

    const auto hs = sigv4::sign({kAccess, kSecret, {}}, r);
    ASSERT_EQ(hs.size(), 2u);
    EXPECT_EQ(hs[0].name, "x-amz-date");
    EXPECT_EQ(hs[1].name, "Authorization");
    EXPECT_EQ(hs[1].value,
              "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/"
              "aws4_request, SignedHeaders=host;x-amz-date, "
              "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
}

TEST(SigV4, ModelIdColonIsEncodedOnceMoreThanTheWire)
{
    // THE Bedrock trap, and it caught a wrong reading of the spec while this was
    // written. AWS encodes a non-S3 path segment twice counting from the RAW path,
    // and the request line already holds the first pass. Both spellings a client
    // might send must land on the same signed form for that request line.
    EXPECT_EQ(sigv4::canonical_uri("/model/anthropic.claude-3-5-sonnet-20240620-v1%3A0/invoke"),
              "/model/anthropic.claude-3-5-sonnet-20240620-v1%253A0/invoke");
    EXPECT_EQ(sigv4::canonical_uri("/model/anthropic.claude-3-5-sonnet-20240620-v1:0/invoke"),
              "/model/anthropic.claude-3-5-sonnet-20240620-v1%3A0/invoke");
    // A path with nothing to escape survives both passes unchanged.
    EXPECT_EQ(sigv4::canonical_uri("/model/x/invoke"), "/model/x/invoke");
    EXPECT_EQ(sigv4::canonical_uri(""), "/");
}

TEST(SigV4, UriEncodingFollowsTheUnreservedSet)
{
    EXPECT_EQ(sigv4::uri_encode("aZ09-_.~", true), "aZ09-_.~");
    EXPECT_EQ(sigv4::uri_encode("a/b", false), "a/b");
    EXPECT_EQ(sigv4::uri_encode("a/b", true), "a%2Fb");
    EXPECT_EQ(sigv4::uri_encode(" ", true), "%20");
    EXPECT_EQ(sigv4::uri_encode("+", true), "%2B");
}

TEST(SigV4, QueryParametersAreSortedByEncodedName)
{
    // Sorting is specified, not cosmetic: two orderings of the same parameters must
    // sign identically or a retry with a reordered query fails.
    EXPECT_EQ(sigv4::canonical_query("b=2&a=1"), "a=1&b=2");
    EXPECT_EQ(sigv4::canonical_query("a=1&b=2"), "a=1&b=2");
    EXPECT_EQ(sigv4::canonical_query("k"), "k=");
    EXPECT_EQ(sigv4::canonical_query(""), "");
}

TEST(SigV4, SessionTokenJoinsTheSignedHeaders)
{
    // Temporary credentials are what most real deployments use, and the token is part
    // of what is signed: omitting it from SignedHeaders while sending the header is a
    // mismatch, and omitting the header entirely is a rejected credential.
    sigv4::Request r{};
    r.method = "POST";
    r.path = "/model/m/invoke";
    r.host = "bedrock-runtime.us-east-1.amazonaws.com";
    r.content_type = "application/json";
    r.body = "{}";
    r.region = "us-east-1";
    r.service = "bedrock";
    r.amz_date = "20150830T123600Z";

    const auto hs = sigv4::sign({kAccess, kSecret, "TOKEN"}, r);
    ASSERT_EQ(hs.size(), 3u);
    EXPECT_EQ(hs[1].name, "x-amz-security-token");
    EXPECT_EQ(hs[1].value, "TOKEN");
    EXPECT_NE(hs[2].value.find("SignedHeaders=content-type;host;x-amz-date;"
                               "x-amz-security-token"),
              std::string::npos);
    EXPECT_NE(sigv4::canonical_request(r, sigv4::sha256_hex(r.body), "TOKEN")
                  .find("x-amz-security-token:TOKEN"),
              std::string::npos);
}

TEST(SigV4, TheBodyIsHashed)
{
    // Two bodies must not sign the same, or a replayed request with edited content
    // would be accepted.
    sigv4::Request a{};
    a.method = "POST";
    a.path = "/x";
    a.host = "h";
    a.region = "r";
    a.service = "s";
    a.amz_date = "20150830T123600Z";
    a.body = "one";
    sigv4::Request b = a;
    b.body = "two";
    EXPECT_NE(sigv4::sign({kAccess, kSecret, {}}, a)[1].value,
              sigv4::sign({kAccess, kSecret, {}}, b)[1].value);
}

TEST(SigV4, RefusesRatherThanSigningSomethingWrong)
{
    // Empty means REFUSE. The caller must not send the request unsigned, and every
    // one of these would otherwise produce a signature AWS rejects with no clue why.
    sigv4::Request r{};
    r.method = "GET";
    r.path = "/";
    r.host = "h";
    r.region = "us-east-1";
    r.service = "bedrock";
    r.amz_date = "20150830T123600Z";

    EXPECT_TRUE(sigv4::sign({{}, kSecret, {}}, r).empty()) << "no access key id";
    EXPECT_TRUE(sigv4::sign({kAccess, {}, {}}, r).empty()) << "no secret";

    sigv4::Request bad = r;
    bad.region = {};
    EXPECT_TRUE(sigv4::sign({kAccess, kSecret, {}}, bad).empty()) << "no region";
    bad = r;
    bad.service = {};
    EXPECT_TRUE(sigv4::sign({kAccess, kSecret, {}}, bad).empty()) << "no service";

    for (const std::string_view d : {"", "20150830", "20150830T123600",
                                     "20150830X123600Z", "2015083AT123600Z"})
    {
        sigv4::Request t = r;
        t.amz_date = d;
        EXPECT_TRUE(sigv4::sign({kAccess, kSecret, {}}, t).empty())
            << "accepted malformed date: " << d;
    }
}

TEST(SigV4, BearerSplitsIntoAwsCredentials)
{
    // Per-request passthrough: the customer's credential travels with the request and
    // llmbridge stores none of its own.
    sigv4::Credentials c{};
    ASSERT_TRUE(sigv4::parse_credentials("AKIDEXAMPLE:secret", c));
    EXPECT_EQ(c.access_key_id, "AKIDEXAMPLE");
    EXPECT_EQ(c.secret_access_key, "secret");
    EXPECT_TRUE(c.session_token.empty());

    ASSERT_TRUE(sigv4::parse_credentials("AKIDEXAMPLE:secret:tok", c));
    EXPECT_EQ(c.secret_access_key, "secret");
    EXPECT_EQ(c.session_token, "tok");

    // A real base64 secret and session token: `+/=` are in the alphabet and `:` is
    // not, which is why splitting on it is unambiguous.
    ASSERT_TRUE(sigv4::parse_credentials(
        "AKIDEXAMPLE:wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY:FwoGZXIvYXdzE=", c));
    EXPECT_EQ(c.secret_access_key, "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY");
    EXPECT_EQ(c.session_token, "FwoGZXIvYXdzE=");
}

TEST(SigV4, MalformedBearerIsRefusedRatherThanPartlyAccepted)
{
    // Every one of these would otherwise sign with a credential that is missing a
    // part, and AWS answers that with the same opaque 403 as a wrong signature.
    sigv4::Credentials c{};
    for (const std::string_view v : {"", "AKID", "AKID:", ":secret", ":",
                                     "AKID::tok", "AKID:secret:", "a:b:c:d"})
        EXPECT_FALSE(sigv4::parse_credentials(v, c)) << "accepted: " << v;
}

TEST(SigV4, NothingSignedLeaksTheSecret)
{
    // The Authorization header carries the key ID and the signature, and must never
    // carry the secret or a signing-key intermediate. A grep of what goes on the wire
    // is the only check that survives a refactor of the internals.
    sigv4::Request r{};
    r.method = "POST";
    r.path = "/model/m/invoke";
    r.host = "bedrock-runtime.us-east-1.amazonaws.com";
    r.content_type = "application/json";
    r.body = R"({"messages":[]})";
    r.region = "us-east-1";
    r.service = "bedrock";
    r.amz_date = "20150830T123600Z";

    for (const auto& h : sigv4::sign({kAccess, kSecret, "SESSIONTOKEN"}, r))
    {
        EXPECT_EQ(h.value.find(kSecret), std::string::npos)
            << "secret appears in header " << h.name;
        // The session token is legitimately sent, in its own header and nowhere else.
        if (h.name != "x-amz-security-token")
            EXPECT_EQ(h.value.find("SESSIONTOKEN"), std::string::npos)
                << "session token leaked into " << h.name;
    }
}
