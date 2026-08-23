// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// AWS Signature Version 4, enough of it to reach Bedrock.
///
/// A defined procedure, not a format to approximate: canonical request, string to
/// sign, a date-scoped HMAC chain, then one header. Every step has an exact
/// serialisation, and a mismatch anywhere fails closed with a 403 whose body says
/// nothing useful, so this is built against AWS's published test vectors and never
/// against a reading of the prose.
///
/// TLS builds only. It needs SHA-256 and HMAC-SHA256, which come from the OpenSSL a
/// TLS build already links; a dependency-free build does not compile this and cannot
/// reach a venue that requires signing.
namespace llmbridge::net::sigv4
{
    /// What the caller must hold to sign. `session_token` is empty for long-lived
    /// keys and set for temporary ones, which is what most real deployments use: an
    /// implementation that omits it works in a demo and fails at a customer.
    struct Credentials
    {
        std::string_view access_key_id;
        std::string_view secret_access_key;
        std::string_view session_token;
    };

    /// The request being signed. `path` is the ORIGIN-FORM target as it will appear
    /// on the wire, already percent-encoded where it needs to be; see canonical_uri()
    /// for why the canonical form is not the same string.
    struct Request
    {
        std::string_view method;
        std::string_view path;
        std::string_view query;    ///< without '?', empty when there is none
        std::string_view host;
        std::string_view content_type;
        std::string_view body;
        std::string_view region;
        std::string_view service;  ///< "bedrock" for the Messages endpoint
        std::string_view amz_date; ///< "YYYYMMDDTHHMMSSZ", UTC
    };

    /// One header to add to the outbound request.
    struct Header
    {
        std::string name;
        std::string value;
    };

    /// Split a bearer value into AWS credentials: "AKID:SECRET" or
    /// "AKID:SECRET:SESSION_TOKEN". False means refuse the request; the caller must
    /// never fall back to sending it unsigned or with a partial credential.
    ///
    /// Per-request passthrough, which is the whole point: llmbridge holds no AWS
    /// credential of its own, the customer's travels with their request and is gone
    /// when it completes. That keeps the OSS gateway stateless and keeps a long-lived
    /// secret out of any store the request path can read.
    ///
    /// Colon-separated because AWS's own alphabets exclude it: an access key id is
    /// uppercase alphanumeric and a secret and session token are base64, which uses
    /// `+/=` and never `:`. So the split is unambiguous, and a value that does not
    /// split into two or three non-empty parts is malformed and refused.
    ///
    /// `out` holds views into `bearer`, so it must outlive them.
    bool parse_credentials(std::string_view bearer, Credentials& out);

    /// The headers that make a request signed: x-amz-date, Authorization, and
    /// x-amz-security-token when the credentials carry one. Returns empty on any
    /// input this cannot sign correctly, which the caller must treat as "refuse the
    /// request", never as "send it unsigned".
    std::vector<Header> sign(const Credentials& c, const Request& r);

    /// Lowercase hex SHA-256. Exposed because the payload hash is also sent as a
    /// header by some services and is worth testing directly.
    std::string sha256_hex(std::string_view data);

    /// HMAC-SHA256, raw bytes. Exposed for the test vectors: the signing-key chain is
    /// where a wrong answer is silent, so it is checked step by step.
    std::string hmac_sha256(std::string_view key, std::string_view data);

    /// RFC 3986 unreserved-set encoding. `encode_slash` false keeps '/' literal,
    /// which is what a path needs and a query value does not.
    ///
    /// Inline, and deliberately free of OpenSSL: the gateway builds a Bedrock request
    /// path with it, and that code compiles in a build with no TLS even though such a
    /// build cannot reach Bedrock. The alternative was a second copy of an encoder,
    /// and two encoders that must agree byte for byte is how a signature silently
    /// stops matching a path.
    inline std::string uri_encode(std::string_view s, bool encode_slash)
    {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(s.size());
        for (const char ch : s)
        {
            const auto c = static_cast<unsigned char>(ch);
            const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                              c == '.' || c == '~' || (c == '/' && !encode_slash);
            if (safe)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('%');
                out.push_back(kDigits[c >> 4]);
                out.push_back(kDigits[c & 0x0f]);
            }
        }
        return out;
    }

    /// The canonical URI, from the path as it appears in the request line.
    ///
    /// Encoded one more time than the wire, and this is the single most common way a
    /// Bedrock signature fails. AWS encodes a non-S3 path segment twice counting from
    /// the raw path, and the request line already holds the first pass: a model id
    /// colon travels as `%3A` and is signed as `%253A`. Sign the wire form unchanged
    /// and every request to a versioned model id returns 403 with an empty body;
    /// encode the wire form twice and you get `%25253A`, which fails the same way.
    std::string canonical_uri(std::string_view wire_path);

    /// The canonical query string: parameters percent-encoded, then sorted by
    /// encoded name. Sorting is part of the specification, not a tidiness choice.
    std::string canonical_query(std::string_view query);

    /// Exposed so a test can assert the exact bytes AWS hashes. A signature mismatch
    /// tells you nothing; a canonical-request mismatch tells you which field is wrong.
    std::string canonical_request(const Request& r, std::string_view payload_hash,
                                  std::string_view session_token);
}
