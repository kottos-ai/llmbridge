// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "net/sigv4.hpp"

#include "net/secure.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace llmbridge::net::sigv4
{
    namespace
    {
        constexpr std::string_view kAlgorithm = "AWS4-HMAC-SHA256";
        constexpr std::string_view kTerminator = "aws4_request";

        std::string to_hex(const unsigned char* p, size_t n)
        {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string out;
            out.resize(n * 2);
            for (size_t i = 0; i < n; ++i)
            {
                out[i * 2] = kDigits[p[i] >> 4];
                out[i * 2 + 1] = kDigits[p[i] & 0x0f];
            }
            return out;
        }

        /// "YYYYMMDDTHHMMSSZ" -> "YYYYMMDD", validated before it is sliced: a
        /// malformed date produces a scope that mismatches and a 403 that names
        /// nothing, so it is cheaper to refuse here.
        bool date_only(std::string_view amz_date, std::string_view& out)
        {
            if (amz_date.size() != 16 || amz_date[8] != 'T' || amz_date[15] != 'Z')
                return false;
            for (size_t i = 0; i < 16; ++i)
            {
                if (i == 8 || i == 15) continue;
                if (amz_date[i] < '0' || amz_date[i] > '9') return false;
            }
            out = amz_date.substr(0, 8);
            return true;
        }

        bool unreserved(unsigned char c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        }

        void append_trimmed_lower(std::string& out, std::string_view v)
        {
            size_t b = v.find_first_not_of(" \t");
            if (b == std::string_view::npos) return;
            size_t e = v.find_last_not_of(" \t");
            for (size_t i = b; i <= e; ++i)
                out.push_back(static_cast<char>(
                    v[i] >= 'A' && v[i] <= 'Z' ? v[i] - 'A' + 'a' : v[i]));
        }
    }

    std::string uri_encode(std::string_view s, bool encode_slash)
    {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(s.size());
        for (const char ch : s)
        {
            const auto c = static_cast<unsigned char>(ch);
            if (unreserved(c) || (c == '/' && !encode_slash))
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

    std::string canonical_uri(std::string_view wire_path)
    {
        if (wire_path.empty()) return "/";
        // ONE more encoding of what the request line already carries. AWS says a
        // non-S3 path segment is encoded twice, counting from the RAW path: the wire
        // form is the first pass, so this is the second. Encoding the wire form twice
        // yields %25253A for a model id colon, which is as wrong as not encoding it.
        // Handles both callers: a literal ':' becomes %3A, an already-escaped %3A
        // becomes %253A, and both are what AWS signs for that request line.
        return uri_encode(wire_path, false);
    }

    std::string canonical_query(std::string_view query)
    {
        if (query.empty()) return {};
        std::vector<std::pair<std::string, std::string>> params;
        size_t pos = 0;
        while (pos <= query.size())
        {
            const size_t amp = query.find('&', pos);
            const std::string_view pair =
                query.substr(pos, amp == std::string_view::npos ? std::string_view::npos
                                                                : amp - pos);
            if (!pair.empty())
            {
                const size_t eq = pair.find('=');
                if (eq == std::string_view::npos)
                    params.emplace_back(uri_encode(pair, true), std::string{});
                else
                    params.emplace_back(uri_encode(pair.substr(0, eq), true),
                                        uri_encode(pair.substr(eq + 1), true));
            }
            if (amp == std::string_view::npos) break;
            pos = amp + 1;
        }
        // Sorted by encoded name, then encoded value. Part of the specification: two
        // requests differing only in parameter order must produce one signature.
        std::sort(params.begin(), params.end());
        std::string out;
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (i) out.push_back('&');
            out.append(params[i].first);
            out.push_back('=');
            out.append(params[i].second);
        }
        return out;
    }

    std::string sha256_hex(std::string_view data)
    {
        std::array<unsigned char, SHA256_DIGEST_LENGTH> md{};
        SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md.data());
        return to_hex(md.data(), md.size());
    }

    std::string hmac_sha256(std::string_view key, std::string_view data)
    {
        std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
        unsigned int len = 0;
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(),
             md.data(), &len);
        std::string out(reinterpret_cast<char*>(md.data()), len);
        secure_clear(md.data(), md.size());
        return out;
    }

    std::string canonical_request(const Request& r, std::string_view payload_hash,
                                  std::string_view session_token)
    {
        // Headers must be sorted by lowercase name and this set is fixed, so it is
        // written already in order, with no runtime sort: content-type, host,
        // x-amz-date, x-amz-security-token.
        std::string out;
        out.append(r.method);
        out.push_back('\n');
        out.append(canonical_uri(r.path));
        out.push_back('\n');
        out.append(canonical_query(r.query));
        out.push_back('\n');
        if (!r.content_type.empty())
        {
            out.append("content-type:");
            append_trimmed_lower(out, r.content_type);
            out.push_back('\n');
        }
        out.append("host:");
        append_trimmed_lower(out, r.host);
        out.push_back('\n');
        out.append("x-amz-date:");
        out.append(r.amz_date);
        out.push_back('\n');
        if (!session_token.empty())
        {
            out.append("x-amz-security-token:");
            out.append(session_token);
            out.push_back('\n');
        }
        out.push_back('\n');
        if (!r.content_type.empty()) out.append("content-type;");
        out.append("host;x-amz-date");
        if (!session_token.empty()) out.append(";x-amz-security-token");
        out.push_back('\n');
        out.append(payload_hash);
        return out;
    }

    bool parse_credentials(std::string_view bearer, Credentials& out)
    {
        const size_t a = bearer.find(':');
        if (a == std::string_view::npos || a == 0) return false;
        const size_t b = bearer.find(':', a + 1);
        if (b == a + 1) return false;                       // empty secret
        if (b != std::string_view::npos && bearer.find(':', b + 1) != std::string_view::npos)
            return false;                                   // a fourth part is not a shape we know
        const std::string_view secret =
            bearer.substr(a + 1, b == std::string_view::npos ? std::string_view::npos
                                                             : b - a - 1);
        if (secret.empty()) return false;
        const std::string_view token =
            b == std::string_view::npos ? std::string_view{} : bearer.substr(b + 1);
        if (b != std::string_view::npos && token.empty()) return false;
        out.access_key_id = bearer.substr(0, a);
        out.secret_access_key = secret;
        out.session_token = token;
        return true;
    }

    std::vector<Header> sign(const Credentials& c, const Request& r)
    {
        std::string_view day;
        if (c.access_key_id.empty() || c.secret_access_key.empty() ||
            r.region.empty() || r.service.empty() || !date_only(r.amz_date, day))
            return {};

        const std::string payload_hash = sha256_hex(r.body);
        const bool tok = !c.session_token.empty();

        std::string scope;
        scope.reserve(day.size() + r.region.size() + r.service.size() + 16);
        scope.append(day).push_back('/');
        scope.append(r.region).push_back('/');
        scope.append(r.service).push_back('/');
        scope.append(kTerminator);

        const std::string creq = canonical_request(r, payload_hash, c.session_token);

        std::string sts;
        sts.append(kAlgorithm).push_back('\n');
        sts.append(r.amz_date).push_back('\n');
        sts.append(scope).push_back('\n');
        sts.append(sha256_hex(creq));

        // The chain. Each link is scrubbed as soon as the next is derived: the
        // intermediates are as good as the secret to anyone who reads this memory,
        // and they outlive the call otherwise.
        std::string k = "AWS4";
        k.append(c.secret_access_key);
        std::string k_date = hmac_sha256(k, day);
        secure_clear(k);
        std::string k_region = hmac_sha256(k_date, r.region);
        secure_clear(k_date);
        std::string k_service = hmac_sha256(k_region, r.service);
        secure_clear(k_region);
        std::string k_signing = hmac_sha256(k_service, kTerminator);
        secure_clear(k_service);
        const std::string sig_raw = hmac_sha256(k_signing, sts);
        secure_clear(k_signing);
        const std::string signature =
            to_hex(reinterpret_cast<const unsigned char*>(sig_raw.data()), sig_raw.size());

        std::string auth;
        auth.append(kAlgorithm);
        auth.append(" Credential=");
        auth.append(c.access_key_id).push_back('/');
        auth.append(scope);
        auth.append(", SignedHeaders=");
        if (!r.content_type.empty()) auth.append("content-type;");
        auth.append("host;x-amz-date");
        if (tok) auth.append(";x-amz-security-token");
        auth.append(", Signature=");
        auth.append(signature);

        std::vector<Header> out;
        out.push_back({"x-amz-date", std::string{r.amz_date}});
        if (tok) out.push_back({"x-amz-security-token", std::string{c.session_token}});
        out.push_back({"Authorization", std::move(auth)});
        return out;
    }
}
