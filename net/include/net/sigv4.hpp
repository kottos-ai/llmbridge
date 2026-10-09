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

/// AWS Signature Version 4, enough of it to reach Bedrock; TLS builds only (OpenSSL's
/// SHA-256 and HMAC). Built against AWS's test vectors: DESIGN.md "Signing for Bedrock".
namespace llmbridge::net::sigv4
{
    /// `session_token` is empty for long-lived keys and set for temporary ones (most deployments).
    struct Credentials
    {
        std::string_view access_key_id;
        std::string_view secret_access_key;
        std::string_view session_token;
    };

    /// `path` is the origin-form target exactly as on the wire, already percent-encoded.
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

    /// Split "AKID:SECRET" or "AKID:SECRET:SESSION_TOKEN" from the client's bearer value;
    /// false means refuse, never send unsigned. `out` views into `bearer`.
    bool parse_credentials(std::string_view bearer, Credentials& out);

    /// x-amz-date, Authorization and (with a session token) x-amz-security-token. Empty
    /// when this cannot sign correctly: refuse the request, never send it unsigned.
    std::vector<Header> sign(const Credentials& c, const Request& r);

    /// Lowercase hex SHA-256, public so the payload hash can be tested directly.
    std::string sha256_hex(std::string_view data);

    /// HMAC-SHA256, raw bytes; public so the signing-key chain is tested step by step.
    std::string hmac_sha256(std::string_view key, std::string_view data);

    /// RFC 3986 unreserved-set encoding; `encode_slash` false keeps '/' for a path. Inline
    /// and free of OpenSSL, so the path builder and the signer share one encoder.
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

    /// The canonical URI: the wire path encoded once more (a colon travels as %3A and is
    /// signed as %253A), the most common way a Bedrock signature fails.
    std::string canonical_uri(std::string_view wire_path);

    /// Parameters percent-encoded, then sorted by encoded name, as the specification requires.
    std::string canonical_query(std::string_view query);

    /// The exact bytes AWS hashes, public because a mismatch here names the wrong field.
    std::string canonical_request(const Request& r, std::string_view payload_hash,
                                  std::string_view session_token);
}
