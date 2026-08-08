// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "net/upstream.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <algorithm>
#include <cstring>

namespace llmbridge::net
{
    namespace
    {
        /// Charset a host may use before we will put it in a Host header or SNI.
        /// Deliberately stricter than the RFCs: no percent-encoding, no underscores,
        /// no uppercase-normalisation games, provider hostnames are plain LDH names.
        bool valid_host_chars(std::string_view h) noexcept
        {
            if (h.empty() || h.size() > 253) return false;
            for (const char c : h)
            {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '.' || c == '-';
                if (!ok) return false;
            }
            // No empty labels ("a..b") and no leading/trailing dot or hyphen weirdness
            // that some resolvers "helpfully" normalise.
            if (h.front() == '.' || h.back() == '.' || h.front() == '-') return false;
            return h.find("..") == std::string_view::npos;
        }

        /// Strict port parse: digits only, 1-65535. atoi's "8080garbage" -> 8080 is
        /// exactly the silent acceptance we do not want in an address.
        bool parse_port(std::string_view s, uint16_t& out) noexcept
        {
            if (s.empty() || s.size() > 5) return false;
            uint32_t v = 0;
            for (const char c : s)
            {
                if (c < '0' || c > '9') return false;
                v = v * 10 + static_cast<uint32_t>(c - '0');
            }
            if (v == 0 || v > 65535) return false;
            out = static_cast<uint16_t>(v);
            return true;
        }
    } // namespace

    UpstreamSpec parse_upstream(std::string_view arg)
    {
        UpstreamSpec spec;
        std::string_view rest = arg;
        bool have_scheme = false;

        if (const size_t ss = rest.find("://"); ss != std::string_view::npos)
        {
            const std::string_view scheme = rest.substr(0, ss);
            if (scheme == "https") spec.tls = true;
            else if (scheme != "http")
            {
                spec.error = "unsupported scheme '" + std::string(scheme) + "' (use http:// or https://)";
                return spec;
            }
            have_scheme = true;
            rest = rest.substr(ss + 3);
        }

        // Path handling: a bare trailing "/" is tolerated (people paste it); anything
        // more is a base path we would silently drop, so refuse loudly instead.
        if (const size_t slash = rest.find('/'); slash != std::string_view::npos)
        {
            if (rest.substr(slash) != "/")
            {
                spec.error = "path '" + std::string(rest.substr(slash)) +
                             "' not supported; the gateway forwards the client's own path";
                return spec;
            }
            rest = rest.substr(0, slash);
        }

        if (rest.find('@') != std::string_view::npos)
        {
            spec.error = "userinfo ('@') not allowed in upstream";
            return spec;
        }
        if (rest.find('?') != std::string_view::npos || rest.find('#') != std::string_view::npos)
        {
            spec.error = "query/fragment not allowed in upstream";
            return spec;
        }
        if (rest.find('[') != std::string_view::npos)
        {
            spec.error = "IPv6 literals not supported (transport is IPv4-only for now)";
            return spec;
        }

        std::string_view host = rest;
        std::string_view port_sv{};
        if (const size_t colon = rest.rfind(':'); colon != std::string_view::npos)
        {
            host = rest.substr(0, colon);
            port_sv = rest.substr(colon + 1);
        }

        if (!valid_host_chars(host))
        {
            spec.error = "invalid host '" + std::string(host) + "'";
            return spec;
        }
        spec.host.assign(host);

        if (!port_sv.empty())
        {
            if (!parse_port(port_sv, spec.port))
            {
                spec.error = "invalid port '" + std::string(port_sv) + "'";
                return spec;
            }
        }
        else if (have_scheme)
        {
            spec.port = spec.tls ? 443 : 80;
        }
        else
        {
            // Bare "host" with no scheme and no port: refuse instead of guess.
            // The legacy form was always IP:PORT; keep that contract explicit.
            spec.error = "port required (HOST:PORT, or use http:// / https:// for defaults)";
            return spec;
        }
        return spec;
    }

    std::vector<std::string> resolve_host_ipv4(const std::string& host, std::string* err)
    {
        std::vector<std::string> out;

        // IPv4 literal: pass through untouched, no resolver in the loop. This also
        // means a literal keeps working when /etc/resolv.conf is broken.
        in_addr probe{};
        if (::inet_pton(AF_INET, host.c_str(), &probe) == 1)
        {
            out.push_back(host);
            return out;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;        // sockaddr_in end to end; no AAAA
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* res = nullptr;
        if (const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res); rc != 0)
        {
            if (err) *err = ::gai_strerror(rc);
            return out;
        }

        // Keep resolver order (it is often weighted/rotated deliberately), drop dups.
        for (const addrinfo* ai = res; ai != nullptr; ai = ai->ai_next)
        {
            if (ai->ai_family != AF_INET || !ai->ai_addr) continue;
            char buf[INET_ADDRSTRLEN]{};
            const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
            if (!::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf)) continue;
            if (std::find(out.begin(), out.end(), buf) == out.end()) out.emplace_back(buf);
        }
        ::freeaddrinfo(res);

        if (out.empty() && err) *err = "no A records";
        return out;
    }
} // namespace llmbridge::net
