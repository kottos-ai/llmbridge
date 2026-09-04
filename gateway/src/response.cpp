// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "response.hpp"

#include <string>

#include "gateway/metrics.hpp" // wall_ns
#include "scan.hpp"

namespace llmbridge::detail
{
    namespace
    {
        /// Reason phrase, error type and client-facing message for a status.
        ///
        /// Was three ternaries over 400 and 504 with everything else falling through
        /// to 502. Fine while those were the only codes in the tree, wrong once a
        /// policy could pick one: a 401 went out as "bad gateway: upstream failure",
        /// blaming the provider for our own refusal.
        ///
        /// The message is generic per status and never names the rule that refused.
        /// The client gets the status; the reason goes to the log.
        struct ErrorShape
        {
            const char* line;
            const char* type;
            const char* msg;
        };

        ErrorShape error_shape(int code)
        {
            switch (code)
            {
                case 400: return {"HTTP/1.1 400 Bad Request", "invalid_request_error", "malformed request"};
                case 401: return {"HTTP/1.1 401 Unauthorized", "authentication_error", "unauthorized"};
                case 403: return {"HTTP/1.1 403 Forbidden", "permission_error", "forbidden"};
                case 404: return {"HTTP/1.1 404 Not Found", "invalid_request_error", "not found"};
                case 413: return {"HTTP/1.1 413 Content Too Large", "invalid_request_error", "request too large"};
                case 415: return {"HTTP/1.1 415 Unsupported Media Type", "invalid_request_error", "compressed request body is not supported"};
                case 429: return {"HTTP/1.1 429 Too Many Requests", "rate_limit_error", "rate limit exceeded"};
                case 503: return {"HTTP/1.1 503 Service Unavailable", "service_error", "service unavailable"};
                case 504: return {"HTTP/1.1 504 Gateway Timeout", "timeout_error", "upstream timed out"};
                // Anything unlisted keeps the historical fallback. It is reachable
                // only from our own call sites, all of which pass a code above; the
                // policy seam validates its status before it gets here.
                default: return {"HTTP/1.1 502 Bad Gateway", "upstream_error", "bad gateway: upstream failure"};
            }
        }

    } // namespace

    // ── Timing headers (opt-in: --timing-headers) ───────────────────────
    //
    // Per-request timing headers. LATENCY.md §3 is normative; keep the two in
    // step. Stamps are monotonic and use the t0..t6 scheme defined in
    // gateway.hpp and LATENCY.md §2; only t0 is also emitted as wall time, and
    // that one is anchored so it can never step backward.
    //
    //   t0 ──► t1 ──► t2 ──► t3 ───────────► t4 ──► t5 ──► t6
    //   client  req    wire   handed to       provider  resp   fully
    //   framed  built  ready  the kernel      received  built  flushed
    //
    //   x-llmbridge-t0            wall-clock epoch nanoseconds at t0. The one
    //                             absolute value: what orders two requests
    //                             against each other, which is why it is here
    //                             at all. Strictly increasing within a process.
    //   x-llmbridge-gateway-us    (t1-t0) + (t5-t4); Our compute, and nothing
    //                             else: framing, translation, auth mapping,
    //                             re-serialisation. It ends at t5 because the
    //                             number travels inside the response and so
    //                             cannot include the cost of sending itself.
    //   x-llmbridge-connect-us    (t2-t1): the handshake alone, ~50-80 ms cold,
    //                             exactly 0 on a pooled connection. Same span as
    //                             the connect(TLS) histogram, same function.
    //   x-llmbridge-upwrite-us    (t3-t2): the write() into the socket buffer.
    //                             Split out of connect-us because folding them
    //                             gave one name two meanings across the two
    //                             reporting surfaces. t2 was already stamped for
    //                             the histogram, so this cost a header line and
    //                             no extra clock reads.
    //   x-llmbridge-upstream-us   (t4-t3): the provider, network + inference.
    //
    // Why connect is split out instead of folded into gateway-us: measured
    // against the live API, a cold connection put 56 ms of TCP+TLS setup inside
    // the "gateway" figure while the same gateway needed 47-63 us once pooled.
    // A customer reading 56 ms as our overhead would be right to walk away, and
    // wrong about the software. One number cannot honestly carry both.
    //
    // Streaming cannot report t4 (headers precede the body), so it emits t0 and
    // x-llmbridge-upstream-ttfb-us instead: time to the provider's first byte.
    //
    // Metadata only, by construction: durations and one timestamp. No prompt,
    // no completion, no token text. "Prompt content is never logged" is a
    // public commitment and this path must never be the thing that breaks it.
    void append_timing_headers(std::string& out, int64_t t0, int64_t gateway_us,
                               int64_t connect_us, int64_t upwrite_us,
                               int64_t upstream_us, const char* upstream_key, uint64_t seq,
                               int64_t client_upload_us)
    {
        const auto add = [&out](const char* k, int64_t v) {
            if (v < 0) return; // a stamp we never took; omit instead of lie
            out.append(k);
            out.append(": ");
            out.append(std::to_string(v));
            out.append("\r\n");
        };
        add("x-llmbridge-t0", wall_ns(t0));
        add("x-llmbridge-seq", static_cast<int64_t>(seq));
        add("x-llmbridge-gateway-us", gateway_us);
        // connect-us is now exactly the connect(TLS) histogram's span (t2-t1):
        // handshake only, and therefore exactly 0 on a pooled connection. The
        // upstream write it used to absorb has its own header below. One name,
        // one meaning, on both surfaces -- see timing_split().
        add("x-llmbridge-connect-us", connect_us);
        add("x-llmbridge-upwrite-us", upwrite_us);
        // Outside `added` on purpose: it is the client's own upload, measured from
        // the first byte this gateway saw to the last. Not the time since the
        // client pressed send, which needs two clocks that agree.
        add("x-llmbridge-client-upload-us", client_upload_us);
        add(upstream_key, upstream_us);
    }

    // Token counts, non-streaming only. A stream cannot carry these: headers
    // precede the body, and both the token totals and the chunk count are
    // end-of-stream facts. They are not invented for streams. A streaming client
    // that wants them sets `stream_options.include_usage` and reads the provider's
    // own counts from the final chunk.
    void append_usage_headers(std::string& out, std::string_view translated_body)
    {
        const BodyUsage u = scan_usage(translated_body);
        if (u.in >= 0)
        {
            out.append("x-llmbridge-tokens-in: ");
            out.append(std::to_string(u.in));
            out.append("\r\n");
        }
        if (u.out >= 0)
        {
            out.append("x-llmbridge-tokens-out: ");
            out.append(std::to_string(u.out));
            out.append("\r\n");
        }
    }

    // `detail` is shown to the client only for a 4xx, where the cause is the
    // caller's own request and naming it is the difference between reading the
    // README and filing a bug. A 5xx is ours or the provider's, and its reason
    // stays in the log: a client has no use for our internals and an attacker
    // has several.
    //
    // Every detail passed here is a string literal from this file, so nothing
    // client-supplied is echoed back. Keep it that way: this string lands inside
    // a JSON body with no escaping.
    std::string build_error(int code, const char* detail)
    {
        const auto [line, type, msg] = error_shape(code);
        const char* shown = (detail && code < 500) ? detail : msg;
        std::string body = std::string("{\"error\":{\"message\":\"") + shown + "\",\"type\":\"" + type + "\"}}";
        std::string out;
        out.reserve(body.size() + 128);
        out.append(line);
        out.append("\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ");
        out.append(std::to_string(body.size()));
        out.append("\r\n\r\n");
        out.append(body);
        return out;
    }

    // Same head with a timing block spliced in before the terminating CRLF.
    // A stream cannot report total gateway time in a header (headers precede
    // the body), so it reports what is known at this point: t0, the request-path
    // cost, and time to the provider's first byte.
    ///
    /// Takes the base head, never assuming one.
    std::string sse_head_with_timing(std::string_view extra, std::string_view base)
    {
        std::string out(base.substr(0, base.size() - 2)); // drop final CRLF
        out.append(extra);
        out.append("\r\n");
        return out;
    }

    // Build a response that preserves the upstream status code. Used to relay a
    // provider's own failure (429 rate limit, 529 overloaded, 400 context
    // length, 401 auth) to the client instead of flattening it to a gateway
    // 502. The client needs the real code to decide whether to back off/retry.
    std::string build_http_status(int status, std::string_view reason, std::string_view body)
    {
        std::string out = "HTTP/1.1 " + std::to_string(status) + " ";
        out.append(reason);
        out.append("\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: ");
        out.append(std::to_string(body.size()));
        out.append("\r\n\r\n");
        out.append(body);
        return out;
    }

    // A short reason phrase for the codes providers actually return.
    //
    // The default classifies, and must never call a success an error: this table
    // held only failures, so a 200 took the `< 500` branch and every successful
    // passthrough went out as "200 Client Error" from v0.27.0, when keeping the
    // provider's status first brought a 2xx here.
    const char* reason_for(int status)
    {
        switch (status)
        {
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
            case 204: return "No Content";
            case 206: return "Partial Content";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 408: return "Request Timeout";
            case 413: return "Payload Too Large";
            case 415: return "Unsupported Media Type";
            case 429: return "Too Many Requests";
            case 500: return "Internal Server Error";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            case 504: return "Gateway Timeout";
            case 529: return "Overloaded";
            default:
                if (status < 200) return "Informational";
                if (status < 300) return "OK";
                if (status < 400) return "Redirection";
                return status < 500 ? "Client Error" : "Server Error";
        }
    }

} // namespace llmbridge::detail
