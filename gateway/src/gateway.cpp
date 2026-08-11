// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "gateway/gateway.hpp"

#include "net/secure.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include "net/socket_util.hpp"
#include "net/uring.hpp" // self-guarded by LLMBRIDGE_HAVE_URING

namespace llmbridge
{
    namespace
    {
        constexpr size_t kInitialBuf = 4096;
        constexpr int kMaxEvents = 1024;
        constexpr int kPollTickMs = 200; // so request_stop() is observed promptly

        // Build a minimal HTTP/1.1 message (start line + JSON body) for a
        // translated request/response. The benchmark backend ignores path and
        // most headers; a real Anthropic target would add x-api-key /
        // anthropic-version here (same cost class).
        // `extra` is zero or more complete "Name: value\r\n" lines, inserted before
        // the terminating CRLF (used for the opt-in timing headers).
        std::string build_http(std::string_view start_line, std::string_view body,
                               std::string_view extra = {})
        {
            std::string out;
            out.reserve(start_line.size() + body.size() + extra.size() + 96);
            out.append(start_line);
            out.append("\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: ");
            out.append(std::to_string(body.size()));
            out.append("\r\n");
            out.append(extra);
            out.append("\r\n");
            out.append(body);
            return out;
        }

        // Upstream REQUEST builder: build_http plus a Host header (HTTP/1.1
        // requires one; the benchmark mocks never cared, real providers reject
        // without it) and the per-dialect auth/extra header lines.
        std::string build_http_request(std::string_view start_line, std::string_view body,
                                       std::string_view host, std::string_view extra)
        {
            std::string out;
            out.reserve(start_line.size() + body.size() + host.size() + extra.size() + 128);
            out.append(start_line);
            out.append("\r\nHost: ");
            out.append(host);
            out.append("\r\n");
            out.append(extra); // zero or more complete "Name: value\r\n" lines
            out.append("Content-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: ");
            out.append(std::to_string(body.size()));
            out.append("\r\n\r\n");
            out.append(body);
            return out;
        }

        // Anthropic's REQUIRED versioning header. Pinned, not passthrough-only:
        // an OpenAI-SDK client has never heard of it, and without it the API
        // rejects the request outright. A client that DOES send its own value
        // wins (see auth_headers_for); the pin is a default, not an override.
        constexpr std::string_view kAnthropicVersionDefault = "2023-06-01";

        // Build the auth/extra header lines to inject into the translated
        // upstream request, from the CLIENT's request headers.
        //
        // WHITELIST, not passthrough: the gateway rebuilds the upstream request,
        // and only the credential headers the target dialect understands may
        // cross the translation boundary. Echoing arbitrary client headers
        // through a rebuilt request is a smuggling surface (and our own framing
        // headers must stay authoritative). TranslateMode::None is untouched by
        // all of this: byte-forwarding already carries every client header.
        //
        // Values re-emitted here cannot contain CR/LF: find_header() bounds each
        // value by its own line's CRLF, so injection via a crafted credential is
        // structurally impossible instead of filtered.
        //
        // The credential is handled as a transient string_view over the client's
        // request buffer and written straight into the upstream bytes; it is
        // never copied anywhere that outlives the request, and never logged.
        // Erase a credential-bearing buffer before it is released or pooled.
        // See net/secure.hpp for why this is not just memset, and why it is a
        // detected platform primitive instead of a compiler trick.
        //
        // Scope is deliberately narrow: the only place a credential OUTLIVES its
        // request is a pooled upstream, which idles up to kIdleUpstreamNs (30 s)
        // holding the request that carried the key. Transient buffers are
        // overwritten microseconds later and are not worth a hot-path memset.
        // Measured: 2.4 ns for a typical ~96 B request buffer, once per request
        // (~0.02% of one core at 84k RPS).
        using llmbridge::net::secure_clear;

        // Is this safe to re-emit as an HTTP header VALUE?
        //
        // Do NOT trust find_header() to have made this safe. It splits on CRLF, so
        // a value may still contain a BARE CR (or LF, NUL, any control byte), and
        // a lenient upstream parser that treats bare CR as a line terminator would
        // then see an injected header. That is not hypothetical: it was measured
        // reaching the upstream as `x-api-key: sk\rX-Smuggled: yes` before this
        // check existed. It matters more than a self-inflicted malformed request
        // because upstream connections are POOLED AND SHARED between clients, so
        // smuggling on one is a cross-client request-splitting vector.
        //
        // RFC 7230 field-value permits obs-text (0x80-0xFF); we are deliberately
        // stricter. A provider API key is printable ASCII, and narrowing the
        // charset costs us nothing real.
        bool header_value_safe(std::string_view v) noexcept
        {
            if (v.empty() || v.size() > 8192) return false;
            for (const unsigned char ch : v)
                if (ch < 0x20 || ch > 0x7E) return false; // CTLs (incl. CR/LF/NUL) and 8-bit
            return true;
        }

        // Trailing OWS is legal in a header line but is not part of the credential;
        // forwarding it verbatim has broken real provider auth before.
        std::string_view rtrim_ows(std::string_view v) noexcept
        {
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
            return v;
        }

        // Every credential header we might care about, collected in ONE pass.
        //
        // The first version called find_header() six times: four to validate, then
        // up to two to fetch; i.e. six full walks of the client's header block per
        // request. Measured on a thermally-gated A/B at 20k RPS that cost ~40 us at
        // p99 (~8%), with p50 unchanged: exactly the shape of "a little extra work
        // on every request". One walk removes it.
        struct AuthHeaders
        {
            std::string_view authorization;
            std::string_view x_api_key;
            std::string_view x_goog_api_key;
            std::string_view anthropic_version;
        };

        AuthHeaders scan_auth_headers(std::string_view headers) noexcept
        {
            AuthHeaders out;
            size_t start = 0;
            while (start < headers.size())
            {
                size_t eol = headers.find("\r\n", start);
                if (eol == std::string_view::npos) eol = headers.size();
                const std::string_view line = headers.substr(start, eol - start);
                // First occurrence wins (anti-smuggling: a duplicated credential must
                // resolve the same way for us and for the upstream), hence the
                // empty() guards.
                if (out.authorization.empty() && net::http::detail::line_is(line, "authorization:"))
                    out.authorization = rtrim_ows(net::http::detail::ltrim(line.substr(14)));
                else if (out.x_api_key.empty() && net::http::detail::line_is(line, "x-api-key:"))
                    out.x_api_key = rtrim_ows(net::http::detail::ltrim(line.substr(10)));
                else if (out.x_goog_api_key.empty() && net::http::detail::line_is(line, "x-goog-api-key:"))
                    out.x_goog_api_key = rtrim_ows(net::http::detail::ltrim(line.substr(15)));
                else if (out.anthropic_version.empty() &&
                         net::http::detail::line_is(line, "anthropic-version:"))
                    out.anthropic_version = rtrim_ows(net::http::detail::ltrim(line.substr(18)));
                start = eol + 2;
            }
            return out;
        }

        // Returns false when the client supplied a syntactically invalid credential
        // (control characters). The caller MUST fail the request instead of
        // forward. Silently dropping would still send a credential-less request
        // upstream, which is a confusing 401; a 400 names the client's own bug.
        bool auth_headers_for(TranslateMode mode, std::string_view client_headers, std::string& out)
        {
            out.clear();
            const AuthHeaders h = scan_auth_headers(client_headers);

            // Validate every credential-bearing header the client sent, whether or
            // not this dialect uses it. Otherwise a malformed value routes around
            // the check by picking the other header.
            for (const std::string_view v :
                 {h.authorization, h.x_api_key, h.x_goog_api_key, h.anthropic_version})
                if (!v.empty() && !header_value_safe(v)) return false;

            // "Bearer K" -> K (bearer scheme only; anything else is not a provider
            // API key and is dropped instead of guessed at).
            const auto bearer = [&]() -> std::string_view {
                const std::string_view v = h.authorization;
                if (v.size() > 7 &&
                    (v.compare(0, 7, "Bearer ") == 0 || v.compare(0, 7, "bearer ") == 0))
                    return net::http::detail::ltrim(v.substr(7));
                return {};
            };

            switch (mode)
            {
                case TranslateMode::Anthropic:
                {
                    std::string_view key = h.x_api_key.empty() ? bearer() : h.x_api_key;
                    if (!key.empty())
                    {
                        out.append("x-api-key: ");
                        out.append(key);
                        out.append("\r\n");
                    }
                    out.append("anthropic-version: ");
                    out.append(h.anthropic_version.empty() ? kAnthropicVersionDefault
                                                           : h.anthropic_version);
                    out.append("\r\n");
                    break;
                }
                case TranslateMode::Gemini:
                {
                    std::string_view key = h.x_goog_api_key.empty() ? bearer() : h.x_goog_api_key;
                    if (!key.empty())
                    {
                        out.append("x-goog-api-key: ");
                        out.append(key);
                        out.append("\r\n");
                    }
                    break;
                }
                case TranslateMode::Cohere:
                {
                    // Cohere speaks Bearer natively -> forward the whole value.
                    if (!h.authorization.empty())
                    {
                        out.append("Authorization: ");
                        out.append(h.authorization);
                        out.append("\r\n");
                    }
                    break;
                }
                case TranslateMode::None:
                    break; // byte-forward path; never called, but total anyway
            }
            return true;
        }

        // Translate an OpenAI request body to the upstream dialect, also yielding
        // the upstream start line. Empty return = malformed body. Shared by both
        // event-loop backends.
        std::string xlate_req(TranslateMode mode, std::string_view body, std::string_view& start_line)
        {
            switch (mode)
            {
                case TranslateMode::Anthropic:
                    start_line = "POST /v1/messages HTTP/1.1";
                    return provider::openai_to_anthropic_request(body);
                case TranslateMode::Gemini:
                    start_line = "POST /v1beta/models/gemini:generateContent HTTP/1.1";
                    return provider::openai_to_gemini_request(body);
                case TranslateMode::Cohere:
                    start_line = "POST /v2/chat HTTP/1.1";
                    return provider::openai_to_cohere_request(body);
                case TranslateMode::None:
                    return {};
            }
            return {};
        }

        // ── Timing headers (opt-in: --timing-headers) ───────────────────────
        //
        // Per-request timing headers. LATENCY.md §3 is normative; keep the two in
        // step. Stamps are monotonic and use the t0..t6 scheme defined in
        // gateway.hpp and LATENCY.md §2; only t0 is also emitted as wall time, and
        // that one is anchored so it can never step backward.
        //
        //   t0 ──► t1 ──► t2 ──► t3 ───────────► t4 ──► t5 ──► t6
        //   client  req    wire   handed to       provider  resp   fully
        //   framed  BUILT  ready  the kernel      received  built  flushed
        //
        //   x-llmbridge-t0            wall-clock epoch NANOSECONDS at t0. The one
        //                             absolute value: what orders two requests
        //                             against each other, which is why it is here
        //                             at all. Strictly increasing within a process.
        //   x-llmbridge-gateway-us    (t1-t0) + (t5-t4); OUR compute, and nothing
        //                             else: framing, translation, auth mapping,
        //                             re-serialisation. It ends at t5 because the
        //                             number travels INSIDE the response and so
        //                             cannot include the cost of sending itself.
        //   x-llmbridge-connect-us    (t2-t1): the handshake ALONE, ~50-80 ms cold,
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
        // against the live API, a COLD connection put 56 ms of TCP+TLS setup inside
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
        // Total order across ALL workers, independent of any clock.
        //
        // std::atomic, NOT volatile: volatile provides neither atomicity nor
        // inter-thread ordering in C++ (it is for memory-mapped I/O), and workers are
        // std::threads sharing this process, so a plain or volatile counter would be a
        // data race that hands two requests the same number.
        //
        // relaxed is sufficient and is the cheapest correct choice: every atomic has a
        // single total modification order, so fetch_add yields unique, increasing
        // values in the order the increments occurred. We need uniqueness and
        // ordering of the counter itself, not ordering of surrounding memory, so no
        // fences are warranted.
        //
        // WHY THIS EXISTS AT ALL: two requests can share a nanosecond, and clocks on
        // different hosts cannot be trusted to sub-millisecond agreement without PTP.
        // (t0, seq) is a total order that needs neither. This is the sequencer
        // pattern: an exchange defines order by arrival at a sequencing point, not by
        // comparing timestamps, and it is why the tape for inference should sequence
        // instead of timestamp.
        std::atomic<uint64_t> g_seq{0};

        // Pull `prompt_tokens` / `completion_tokens` out of a translated OpenAI body.
        //
        // BOUNDED on purpose: `usage` is the last object in the response we build, so
        // this searches only the tail instead of scanning a body that may be many KB
        // of completion text. On no match the headers are simply omitted. The
        // existing rule everywhere in this file is to omit instead of report a
        // number we did not measure.
        struct BodyUsage { long long in = -1, out = -1; };

        BodyUsage scan_usage(std::string_view body) noexcept
        {
            BodyUsage u;
            constexpr size_t kTail = 256;
            const std::string_view tail =
                body.size() > kTail ? body.substr(body.size() - kTail) : body;
            const auto num_after = [&tail](std::string_view key) -> long long {
                const size_t k = tail.find(key);
                if (k == std::string_view::npos) return -1;
                size_t i = k + key.size();
                while (i < tail.size() && (tail[i] == ':' || tail[i] == ' ')) ++i;
                long long v = 0;
                bool any = false;
                for (; i < tail.size() && tail[i] >= '0' && tail[i] <= '9'; ++i)
                {
                    v = v * 10 + (tail[i] - '0');
                    any = true;
                }
                return any ? v : -1;
            };
            u.in = num_after("\"prompt_tokens\"");
            u.out = num_after("\"completion_tokens\"");
            return u;
        }

        void append_timing_headers(std::string& out, int64_t t0, int64_t gateway_us,
                                   int64_t connect_us, int64_t upwrite_us,
                                   int64_t upstream_us, const char* upstream_key)
        {
            const auto add = [&out](const char* k, int64_t v) {
                if (v < 0) return; // a stamp we never took; omit instead of lie
                out.append(k);
                out.append(": ");
                out.append(std::to_string(v));
                out.append("\r\n");
            };
            add("x-llmbridge-t0", wall_ns(t0));
            add("x-llmbridge-seq", static_cast<int64_t>(g_seq.fetch_add(1, std::memory_order_relaxed)));
            add("x-llmbridge-gateway-us", gateway_us);
            // connect-us is now EXACTLY the connect(TLS) histogram's span (t2-t1):
            // handshake only, and therefore exactly 0 on a pooled connection. The
            // upstream write it used to absorb has its own header below. One name,
            // one meaning, on both surfaces -- see timing_split().
            add("x-llmbridge-connect-us", connect_us);
            add("x-llmbridge-upwrite-us", upwrite_us);
            add(upstream_key, upstream_us);
        }

        // Token counts, non-streaming only. A stream cannot carry these: headers
        // precede the body, and both the token totals and the chunk count are
        // end-of-stream facts. They are NOT invented for streams. A streaming client
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

        std::string build_error(int code)
        {
            const char* line = code == 400   ? "HTTP/1.1 400 Bad Request"
                               : code == 504 ? "HTTP/1.1 504 Gateway Timeout"
                                             : "HTTP/1.1 502 Bad Gateway";
            const char* type = code == 400   ? "invalid_request_error"
                               : code == 504 ? "timeout_error"
                                             : "upstream_error";
            const char* msg = code == 400   ? "malformed request"
                              : code == 504 ? "upstream timed out"
                                            : "bad gateway: upstream failure";
            std::string body = std::string("{\"error\":{\"message\":\"") + msg + "\",\"type\":\"" + type + "\"}}";
            std::string out;
            out.reserve(body.size() + 128);
            out.append(line);
            out.append("\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ");
            out.append(std::to_string(body.size()));
            out.append("\r\n\r\n");
            out.append(body);
            return out;
        }

        // Client-facing SSE response head. The stream is close-delimited: the body
        // ends when we close the socket, so no client-side chunk framing is needed.
        constexpr std::string_view kSseHead =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n";

        // Same head with a timing block spliced in before the terminating CRLF.
        // A stream cannot report total gateway time in a header (headers precede
        // the body), so it reports what IS known at this point: t0, the request-path
        // cost, and time to the provider's first byte.
        std::string sse_head_with_timing(std::string_view extra)
        {
            std::string out(kSseHead.substr(0, kSseHead.size() - 2)); // drop final CRLF
            out.append(extra);
            out.append("\r\n");
            return out;
        }

        // Build a response that PRESERVES the upstream status code. Used to relay a
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
        const char* reason_for(int status)
        {
            switch (status)
            {
                case 400: return "Bad Request";
                case 401: return "Unauthorized";
                case 403: return "Forbidden";
                case 404: return "Not Found";
                case 408: return "Request Timeout";
                case 413: return "Payload Too Large";
                case 429: return "Too Many Requests";
                case 500: return "Internal Server Error";
                case 502: return "Bad Gateway";
                case 503: return "Service Unavailable";
                case 504: return "Gateway Timeout";
                case 529: return "Overloaded";
                default: return status < 500 ? "Client Error" : "Server Error";
            }
        }

        // Outcome of one streaming translate step (shared by both backends).
        enum class StreamStep
        {
            Ok,      // bytes translated (maybe none); stream continues
            Ended,   // upstream signalled end; terminal [DONE] emitted
            Corrupt, // malformed chunked framing: drop WITHOUT a fake clean [DONE]
            Failed   // translator refused (cap tripped / protocol error): drop
        };

        // The dialect/transport transform shared by the epoll and io_uring pumps:
        // chunk-decode -> SSE-translate -> detect end. Lives in ONE place so a fix
        // (e.g. honouring the translator's cap) can't land on one backend only; the
        // backends keep their own idiomatic DELIVERY (flush+pause vs kick+cap).
        // `in` is the upstream's raw buffer (consumed); `out` receives client SSE.
        StreamStep stream_step(Connection* client, std::string& in, std::string& out, bool at_eof)
        {
            std::string sse_in;
            if (client->stream_chunked)
            {
                const bool ok = client->chunkdec.feed(in, sse_in);
                in.clear();
                if (!ok) return StreamStep::Corrupt; // truncate honestly: no fake [DONE]
            }
            else
            {
                sse_in.swap(in);
                in.clear();
            }

            // Honour the translator's own failure (its DoS caps are sticky): a
            // hostile/broken upstream must tear the stream down, not silently
            // produce nothing while we keep reading it forever.
            if (!sse_in.empty() && !client->sse->feed(sse_in, out)) return StreamStep::Failed;

            const bool ended = (client->stream_chunked && client->chunkdec.done()) || at_eof;
            if (ended && !client->stream_ended)
            {
                if (!client->sse->finish(out)) return StreamStep::Failed;
                client->stream_ended = true;
                return StreamStep::Ended;
            }
            return client->stream_ended ? StreamStep::Ended : StreamStep::Ok;
        }

        // Translate an upstream response body back to the OpenAI shape. Empty = bad.
        std::string xlate_resp(TranslateMode mode, std::string_view body)
        {
            switch (mode)
            {
                case TranslateMode::Anthropic: return provider::anthropic_to_openai_response(body);
                case TranslateMode::Gemini: return provider::gemini_to_openai_response(body);
                case TranslateMode::Cohere: return provider::cohere_to_openai_response(body);
                case TranslateMode::None: return {};
            }
            return {};
        }
    } // namespace

    Gateway::Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                     int64_t warmup_ns, TranslateMode translate, IoBackend io,
                     int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers)
        : _listen_port(listen_port), _upstream_ip(std::move(upstream_ip)),
          _upstream_port(upstream_port), _warmup_ns(warmup_ns), _translate(translate), _io(io),
          _upstream_idle_ns(upstream_idle_ns), _tls(std::move(tls)),
          _timing_headers(timing_headers)
    {
#ifdef LLMBRIDGE_HAVE_TLS
        if (_tls.upstream_tls)
        {
            // Setup path: a bad trust store must fail construction, not the first
            // request. Same throw discipline as the listener below.
            net::tls::Context::ClientOptions o;
            o.ca_file = _tls.ca_file;
            if (!_tls_upstream_ctx.init_client(o))
                throw std::runtime_error("TLS context init failed: " + _tls_upstream_ctx.last_error());
        }
        if (_tls.client_tls)
        {
            // Same discipline as above and for a sharper reason: a missing or
            // unreadable certificate must stop the process, never downgrade the
            // listener to plaintext. A client that dialled https and got a
            // plaintext answer would send its credential in the clear.
            net::tls::Context::ServerOptions so;
            so.cert_file = _tls.cert_file;
            so.key_file = _tls.key_file;
            if (!_tls_client_ctx.init_server(so))
                throw std::runtime_error("inbound TLS init failed: " + _tls_client_ctx.last_error());
        }
#else
        if (_tls.upstream_tls)
            throw std::runtime_error("TLS upstream requested but built without LLMBRIDGE_TLS");
#endif
        // Host header for rebuilt (translated) upstream requests. Prefer the
        // parsed hostname (rides in TlsConfig::sni_host whether or not TLS is
        // on, since real providers route/verify on it); fall back to ip:port for the
        // bare IP:PORT form. Default ports are omitted per convention.
        if (!_tls.sni_host.empty())
        {
            _upstream_host_hdr = _tls.sni_host;
            const bool default_port =
                (_tls.upstream_tls && _upstream_port == 443) || (!_tls.upstream_tls && _upstream_port == 80);
            if (!default_port) _upstream_host_hdr += ":" + std::to_string(_upstream_port);
        }
        else
        {
            _upstream_host_hdr = _upstream_ip + ":" + std::to_string(_upstream_port);
        }
        // Linux has no SO_NOSIGPIPE; ignore SIGPIPE process-wide so a write to a
        // peer-closed socket returns EPIPE instead of killing us. Idempotent, so
        // safe to set here (covers the daemon and the test harness alike).
        std::signal(SIGPIPE, SIG_IGN);
        _epfd = ::epoll_create1(0);
        if (_epfd < 0) throw std::runtime_error("epoll_create1() failed");
        _listen_fd = net::make_listener(_listen_port);
        if (_listen_fd < 0) throw std::runtime_error("failed to bind listen port");
        _listen_conn = new Connection();
        _listen_conn->fd = _listen_fd;
        ep_add_read(_listen_conn);
        std::fprintf(stderr, "llmbridge: listening :%u -> upstream %s:%u%s\n",
                     _listen_port, _upstream_ip.c_str(), _upstream_port,
                     _translate == TranslateMode::Anthropic ? " (translate: anthropic)" : "");

        // Resolve the event-loop backend: io_uring for Uring/Auto when the kernel
        // supports it, else epoll. Uring requested but unavailable -> epoll.
#ifdef LLMBRIDGE_HAVE_URING
        const bool uring_ok = net::uring::available();
        if (_io == IoBackend::Uring || _io == IoBackend::Auto) _uring_active = uring_ok;
        if (_io == IoBackend::Uring && !uring_ok)
            std::fprintf(stderr, "llmbridge: --io=uring requested but io_uring unavailable; using epoll\n");
#endif
        const char* want = _io == IoBackend::Uring ? "uring" : _io == IoBackend::Epoll ? "epoll" : "auto";
        std::fprintf(stderr, "llmbridge: io=%s, %s loop\n", want, _uring_active ? "io_uring" : "epoll");
    }

    Gateway::~Gateway()
    {
        for (auto& [id, c] : _clients)
        {
            // An in-flight (acquired, not pooled) upstream is reachable only via
            // peer: free it too, or it leaks when we stop mid-request. (The
            // io_uring loop already nulls these during its drain.)
            if (Connection* u = c->peer) { if (u->fd >= 0) ::close(u->fd); delete u; }
            if (c->fd >= 0) ::close(c->fd);
            delete c;
        }
        for (Connection* u : _idle_upstreams) { if (u->fd >= 0) ::close(u->fd); delete u; }
        for (Connection* d : _doomed) delete d;
        if (_listen_fd >= 0) ::close(_listen_fd);
        if (_epfd >= 0) ::close(_epfd);
        delete _listen_conn;
    }

    uint16_t Gateway::bound_port() const noexcept
    {
        if (_listen_fd < 0) return 0;
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) return 0;
        return ntohs(addr.sin_port);
    }

    // epoll is level-triggered here (no EPOLLET): every readable handler first
    // drains the socket into rbuf, so EPOLLIN won't re-fire on unread bytes, and
    // EPOLLIN stays armed for the connection's whole life. Write interest is
    // toggled via EPOLL_CTL_MOD on top of that always-on EPOLLIN.
    void Gateway::ep_add_read(Connection* c) noexcept
    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_ADD, c->fd, &ev);
    }

    void Gateway::ep_arm_write(Connection* c) noexcept
    {
        if (c->write_armed) return;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->write_armed = true;
    }

    void Gateway::ep_disarm_write(Connection* c) noexcept
    {
        if (!c->write_armed) return;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->write_armed = false;
    }

    // Backpressure: pause/resume reading a connection. Used to stop pulling
    // upstream SSE bytes while the client's write buffer is draining, so a slow
    // client can't make us buffer an unbounded stream. (EPOLLHUP/EPOLLERR still
    // fire while paused, so an upstream close is never missed.)
    void Gateway::ep_pause_read(Connection* c) noexcept
    {
        if (c->read_paused) return;
        epoll_event ev{};
        ev.events = c->write_armed ? static_cast<uint32_t>(EPOLLOUT) : 0u;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->read_paused = true;
        ++_stats.stream_pauses; // observability: proves backpressure actually engaged
    }

    void Gateway::ep_resume_read(Connection* c) noexcept
    {
        if (!c->read_paused) return;
        epoll_event ev{};
        ev.events = static_cast<uint32_t>(EPOLLIN) | (c->write_armed ? static_cast<uint32_t>(EPOLLOUT) : 0u);
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
        c->read_paused = false;
    }

    bool Gateway::ep_drain_read(Connection* c) noexcept
    {
        char tmp[16384];
        for (;;)
        {
            ssize_t n = ::read(c->fd, tmp, sizeof(tmp));
            if (n > 0)
            {
                c->rbuf.append(tmp, static_cast<size_t>(n));
                if (static_cast<size_t>(n) < sizeof(tmp)) return true;
                continue;
            }
            if (n == 0) return false;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;
        }
    }

    bool Gateway::ep_pump_write(Connection* c, bool* done) noexcept
    {
        *done = false;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_invariant_ok(c)) return false; // never plaintext on a TLS conn
        // Branching HERE and not at the call sites is deliberate: ep_respond,
        // ep_on_client_writable and the SSE stream flush all reach the socket
        // through this one function, so inbound TLS covers streaming for free
        // instead of needing a fourth copy of the logic.
        if (c->tls)
        {
            if (c->tls->handshake_done()) tls_push_wbuf(c);
            bool flushed = false;
            if (!ep_tls_flush(c, &flushed)) return false;
            *done = flushed && tls_wbuf_flushed(c);
            return true;
        }
#endif
        while (c->woff < c->wbuf.size())
        {
            ssize_t n = ::write(c->fd, c->wbuf.data() + c->woff, c->wbuf.size() - c->woff);
            if (n > 0) { c->woff += static_cast<size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        // Leave wbuf intact on completion: the request stays available for a
        // stale-connection resend until the response is read; callers clear it.
        *done = true;
        return true;
    }

    // Shared by both backends, hence no ep_/ur_ prefix: is this upstream carrying
    // TLS? Compiles to `false` in a build without TLS support.
    bool Gateway::upstream_is_tls(const Connection* u) const noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        return u->tls != nullptr;
#else
        (void)u;
        return false;
#endif
    }

#ifdef LLMBRIDGE_HAVE_TLS
    // ── TLS plumbing ────────────────────────────────────────────────────────
    // The invariant (see gateway.hpp): rbuf/wbuf are PLAINTEXT, always. TLS lives
    // strictly between the socket and those buffers. For a TLS upstream, `woff`
    // counts plaintext fed into the Session (so retry-resend still works from
    // wbuf), and tls_out/tls_out_off track the encrypted bytes towards the socket.

    bool Gateway::tls_required(const Connection* c) const noexcept
    {
        return c->is_client ? _tls.client_tls : _tls.upstream_tls;
    }

    bool Gateway::tls_invariant_ok(Connection* c) noexcept
    {
        if (c->tls || !tls_required(c)) return true;

        // A connection the configuration says must be encrypted has no Session.
        // Whatever the cause, the one thing we must not do is fall through to the
        // plaintext path: on the client leg those bytes are a response to a peer
        // that dialled TLS, and on the upstream leg the very next thing we would
        // send is the provider credential. Either way the failure mode is a
        // disclosure, so refuse and let the caller tear the connection down.
        //
        // Unreachable by construction TODAY: both accept paths close the
        // connection immediately when tls_attach_client() fails, and both upstream
        // paths do the same for tls_attach_upstream(). This exists so that a
        // future edit which adds a third way to create a connection cannot quietly
        // reintroduce a plaintext path, and so that the condition is COUNTED
        // instead of guessed at if it ever does occur.
        ++_stats.errors;
        return false;
    }

    bool Gateway::tls_attach_upstream(Connection* u) noexcept
    {
        u->tls = std::make_unique<net::tls::Session>();
        if (!u->tls->init_client(_tls_upstream_ctx, _tls.sni_host))
        {
            u->tls.reset();
            return false;
        }
        return true;
    }

    bool Gateway::tls_attach_client(Connection* c) noexcept
    {
        c->tls = std::make_unique<net::tls::Session>();
        if (!c->tls->init_server(_tls_client_ctx))
        {
            c->tls.reset();
            return false;
        }
        // Accept state is already set by init_server. The handshake advances on the
        // first readable event: the client speaks first with a ClientHello, so there
        // is nothing to emit here and nothing to wait for.
        return true;
    }

    void Gateway::tls_pump_out(Connection* u) noexcept
    {
        // io_uring CAUTION: callers there must only pump while no SEND SQE is in
        // flight on this conn; appending can reallocate tls_out under the kernel.
        // The SSL object's write BIO is the staging area in the meantime.
        uint8_t buf[16384];
        size_t n;
        while ((n = u->tls->pull_ciphertext({buf, sizeof buf})) > 0)
            u->tls_out.append(reinterpret_cast<const char*>(buf), n);
    }

    void Gateway::tls_push_wbuf(Connection* u) noexcept
    {
        // Feed as much request plaintext as the Session accepts. Does NOT pump the
        // resulting ciphertext; the two backends stage it differently.
        while (u->woff < u->wbuf.size())
        {
            const auto* p = reinterpret_cast<const uint8_t*>(u->wbuf.data()) + u->woff;
            const size_t n = u->tls->write_plaintext({p, u->wbuf.size() - u->woff});
            if (n == 0) break; // handshake not done, or session back-pressured
            u->woff += n;
        }
    }

    bool Gateway::tls_wbuf_flushed(const Connection* u) const noexcept
    {
        return u->woff >= u->wbuf.size() && u->tls_out_off >= u->tls_out.size() &&
               !u->tls->has_pending_output();
    }

    bool Gateway::tls_feed(Connection* u, const char* p, size_t n) noexcept
    {
        const bool hs_was_done = u->tls->handshake_done();
        if (n > 0 &&
            u->tls->feed_ciphertext({reinterpret_cast<const uint8_t*>(p), n}) != n)
            return false;
        if (u->tls->want() == net::tls::Want::Error) return false;

        // Drain whatever plaintext became available into rbuf. From here on the
        // existing HTTP framing / SSE pump code takes over unchanged.
        uint8_t buf[16384];
        size_t r;
        while ((r = u->tls->read_plaintext({buf, sizeof buf})) > 0)
            u->rbuf.append(reinterpret_cast<const char*>(buf), r);
        if (u->tls->want() == net::tls::Want::Error) return false;

        // Handshake completed on this feed. On an UPSTREAM conn the request has
        // been waiting in wbuf and can finally go through the Session. On an
        // INBOUND conn there is nothing pending, because the client speaks first
        // and its request arrives as plaintext out of this very call; t2 is an
        // upstream concept and stamping it here would be meaningless.
        if (!hs_was_done && u->tls->handshake_done() && !u->is_client)
        {
            // t2 belongs HERE for TLS, not at TCP connect. The wire cannot carry
            // the request until the handshake is done, so stamping t2 earlier put
            // the entire handshake inside upwrite-us (t3-t2) and left connect-us
            // reporting the TCP leg alone. See the attribution test in
            // gateway/tests/gateway_tls_test.cpp.
            if (u->peer) u->peer->ts_wire_ready = now_ns();
            if (u->woff < u->wbuf.size()) tls_push_wbuf(u);
        }
        return true;
    }

    bool Gateway::ep_tls_flush(Connection* u, bool* done) noexcept
    {
        *done = false;
        tls_pump_out(u); // epoll: no in-flight SEND to worry about; always safe
        while (u->tls_out_off < u->tls_out.size())
        {
            const ssize_t n = ::write(u->fd, u->tls_out.data() + u->tls_out_off,
                                      u->tls_out.size() - u->tls_out_off);
            if (n > 0) { u->tls_out_off += static_cast<size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                ep_arm_write(u);
                return true;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        u->tls_out.clear();
        u->tls_out_off = 0;
        *done = true;
        return true;
    }

    bool Gateway::ep_tls_drain_read(Connection* u) noexcept
    {
        char tmp[16384];
        for (;;)
        {
            const ssize_t n = ::read(u->fd, tmp, sizeof tmp);
            if (n > 0)
            {
                if (!tls_feed(u, tmp, static_cast<size_t>(n))) return false;
                if (static_cast<size_t>(n) < sizeof tmp) break;
                continue;
            }
            if (n == 0) return false; // EOF
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
        // Feeding may have produced output (handshake flights, the request itself
        // once the handshake completed). Put it on the wire before returning to
        // the parse logic, and stamp the request-sent time on the transition.
        const bool was_flushed = u->peer && tls_wbuf_flushed(u);
        bool done = false;
        if (!ep_tls_flush(u, &done)) return false;
        if (u->peer && !was_flushed && tls_wbuf_flushed(u))
            u->peer->ts_up_sent = now_ns();
        return true;
    }
#endif // LLMBRIDGE_HAVE_TLS

    namespace
    {
        // May this streaming upstream go back into the keep-alive pool?
        //
        // Same spirit as the non-streaming rule ("pool only what will stay open"),
        // plus the framing conditions that make the end of the body knowable. Every
        // clause is load-bearing:
        //
        //   stream_keep_alive  the provider didn't say Connection: close; pooling a
        //                      conn it is about to close just buys a retry later
        //   stream_chunked     a close-delimited body has NO end marker except EOF,
        //                      so "the response finished" and "the connection died"
        //                      are indistinguishable, so never reuse one
        //   chunkdec.done()    the terminal 0-length chunk was consumed, so we are
        //                      at a real message boundary instead of mid-body
        //   rbuf empty         no trailing/pipelined bytes left over; anything still
        //                      buffered would be mis-read as the NEXT response
        //   !close_after_resp  aborted, corrupt, or idle-timed-out streams are never
        //                      pooled; we don't trust framing we already distrusted
        //
        // Conservative by construction: any doubt falls through to close, which is
        // exactly the behaviour that shipped before reuse existed.
        bool stream_upstream_reusable(const Connection* client, const Connection* u) noexcept
        {
            return client != nullptr && u != nullptr && !u->doomed && u->fd >= 0
                   && client->stream_keep_alive && client->stream_chunked
                   && client->chunkdec.done() && u->rbuf.empty() && !client->close_after_resp;
        }
    } // namespace

    Connection* Gateway::ep_acquire_upstream() noexcept
    {
        if (!_idle_upstreams.empty())
        {
            Connection* u = _idle_upstreams.back();
            _idle_upstreams.pop_back();
            u->from_pool = true; // reused -> a pre-response failure is retry-eligible
            u->retried = false;  // fresh request: one retry available again
            ++_stats.upstream_reused;
            return u;
        }
        int fd = net::start_connect(_upstream_ip.c_str(), _upstream_port);
        if (fd < 0) return nullptr;
        Connection* u = new Connection();
        u->fd = fd;
        u->is_client = false;
        u->from_pool = false;
        u->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (_tls.upstream_tls && !tls_attach_upstream(u))
        {
            ::close(fd);
            delete u;
            return nullptr;
        }
#endif
        ep_add_read(u);
        ep_arm_write(u); // learn when the non-blocking connect completes
        ++_stats.upstream_conns_opened;
        return u;
    }

    bool Gateway::ep_retry_upstream(Connection* u) noexcept
    {
        // Stale pooled connection: reused from the keep-alive pool, not yet retried,
        // and failed before sending any response. The provider almost certainly
        // dropped it idle without processing. Resend the request once on a fresh
        // connection instead of failing the client. (Same rule as the io_uring path.)
        if (!u->from_pool || u->retried || !u->rbuf.empty()) return false;
        Connection* client = u->peer;
        if (!client) return false;
        int fd = net::start_connect(_upstream_ip.c_str(), _upstream_port);
        if (fd < 0) return false;

        Connection* uf = new Connection();
        uf->fd = fd;
        uf->is_client = false;
        uf->from_pool = false;
        uf->retried = true; // this request's one allowed retry is now spent
        uf->wbuf = std::move(u->wbuf); // PLAINTEXT, re-pushed through the new session
        uf->woff = 0;
        uf->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (_tls.upstream_tls && !tls_attach_upstream(uf))
        {
            ::close(fd);
            delete uf;
            return false;
        }
#endif
        ep_add_read(uf);
        ep_arm_write(uf); // learn when connect completes, then send the request
        ++_stats.upstream_conns_opened;
        ++_stats.upstream_retries;

        u->peer = nullptr;
        ep_close_upstream(u); // discard the dead connection
        client->peer = uf;
        uf->peer = client;
        return true;
    }

    void Gateway::ep_release_upstream(Connection* u) noexcept
    {
        // Bounded pool: past the cap, close instead of accumulate. A streaming
        // gateway pools roughly one upstream per concurrent stream, so an unbounded
        // pool would pin an fd per stream forever.
        if (_idle_upstreams.size() >= kMaxIdleUpstreams) { ep_close_upstream(u); return; }
        u->peer = nullptr;
        u->rbuf.clear();
        u->rdec.reset();
        // wbuf held the REBUILT REQUEST, including the client's credential, and this
        // connection may now idle for 30 s. Scrub instead of clear.
        secure_clear(u->wbuf);
        u->woff = 0;
        u->msg = net::http::Message{};
#ifdef LLMBRIDGE_HAVE_TLS
        u->tls_out.clear(); // per-request ciphertext; the Session itself is KEPT
        u->tls_out_off = 0; // pooled reuse must not pay a second handshake
#endif
        u->ts_pooled = now_ns(); // idle-eviction baseline
        ep_disarm_write(u);
        _idle_upstreams.push_back(u);
    }

    // close_* defer the free to the end of the epoll batch: an earlier event in
    // the same batch can close a conn that a later event still references via
    // data.ptr (e.g. a peer aborted while its own fd is also ready), so freeing
    // inline would dangle that pointer. Events for doomed conns are skipped in
    // run().
    void Gateway::ep_close_client(Connection* c) noexcept
    {
        if (c->doomed) return;
        if (c->id) _clients.erase(c->id);
        if (c->fd >= 0) { ::close(c->fd); c->fd = -1; }
        c->doomed = true;
        _doomed.push_back(c);
    }

    void Gateway::ep_close_upstream(Connection* u) noexcept
    {
        if (u->doomed) return;
        for (auto it = _idle_upstreams.begin(); it != _idle_upstreams.end(); ++it)
            if (*it == u) { _idle_upstreams.erase(it); break; }
        if (u->fd >= 0) { ::close(u->fd); u->fd = -1; }
        u->doomed = true;
        _doomed.push_back(u);
    }

    void Gateway::ep_abort_pair(Connection* client) noexcept
    {
        Connection* u = client->peer;
        if (u) { u->peer = nullptr; ep_close_upstream(u); }
        ep_close_client(client);
        ++_stats.errors;
    }

    void Gateway::ep_error_respond(Connection* client, int code) noexcept
    {
        // Null-tolerant to match ur_error_respond exactly. Every current epoll call
        // site already guarantees non-null, but twins with different contracts are
        // a trap: a caller copied from one side to the other inherits the wrong
        // assumption silently.
        if (!client || client->doomed) return;
        // We're replying to the client ourselves, so drop any in-flight upstream.
        if (Connection* u = client->peer) { client->peer = nullptr; u->peer = nullptr; ep_close_upstream(u); }
        client->wbuf = build_error(code);
        client->woff = 0;
        client->close_after_resp = true; // ep_finish_client closes once it flushes
        ++_stats.errors;
        ep_respond(client);
    }

    void Gateway::ep_on_accept() noexcept
    {
        for (;;)
        {
            int fd = ::accept(_listen_fd, nullptr, nullptr);
            if (fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                return;
            }
            net::set_nonblocking(fd);
            net::set_nodelay(fd);
            net::set_nosigpipe(fd);
            Connection* c = new Connection();
            c->fd = fd;
            c->is_client = true;
            c->id = _next_client_id++;
            c->ts_accepted = now_ns();
            c->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
            if (_tls.client_tls && !tls_attach_client(c))
            {
                // Fail CLOSED. Answering in plaintext because the Session would
                // not attach is how a credential ends up on the wire in the clear.
                ::close(fd);
                delete c;
                ++_stats.errors;
                continue;
            }
#endif
            _clients[c->id] = c;
            ep_add_read(c);
        }
    }

    void Gateway::ep_on_client_readable(Connection* c) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        // Inbound TLS: ciphertext off the socket, plaintext into rbuf. Everything
        // below this point, framing included, is unchanged and unaware of TLS.
        const bool ok = c->tls ? ep_tls_drain_read(c) : ep_drain_read(c);
#else
        const bool ok = ep_drain_read(c);
#endif
        if (!ok)
        {
            // EOF/error: mid-request close is a real abort; idle close is normal.
            const bool in_flight = c->peer != nullptr || !c->wbuf.empty();
            if (in_flight) ep_abort_pair(c);
            else ep_close_client(c);
            return;
        }
        // One request in flight at a time per client.
        if (c->peer != nullptr || !c->wbuf.empty()) return;
        if (c->rbuf.empty()) return;

        // Stamp just before framing so the (completing) HTTP parse is counted in
        // the request-path overhead. On a partial read parse returns NeedMore and
        // we discard t0 and return, so inter-packet network wait is never charged
        // to the gateway.
        const int64_t t0 = now_ns();
        net::http::Message m;
        auto st = net::http::parse_request(c->rbuf, m);
        if (st == net::http::FrameStatus::NeedMore) return;
        if (st == net::http::FrameStatus::Error) { ep_error_respond(c, 400); return; }

        c->msg = m;
        c->ts_req_recvd = t0;
        ep_forward(c);
    }

    void Gateway::ep_forward(Connection* c) noexcept
    {
        // Build the bytes to send upstream (translate first, before acquiring an
        // upstream, so a bad body can't leak a pooled connection).
        std::string upstream_bytes;
        if (_translate != TranslateMode::None)
        {
            std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
            std::string_view start_line;
            std::string tbody = xlate_req(_translate, body, start_line);
            if (tbody.empty()) { ep_error_respond(c, 400); return; }
            // Remember whether the client asked for a final usage chunk. The
            // request bytes are consumed below, but the stream needs it later.
            c->wants_usage = provider::openai_wants_stream_usage(body);
            const std::string_view client_hdrs(c->rbuf.data(), c->msg.header_len);
            std::string auth_hdrs;
            // Malformed credential => 400, and NOTHING goes upstream.
            if (!auth_headers_for(_translate, client_hdrs, auth_hdrs)) { ep_error_respond(c, 400); return; }
            upstream_bytes = build_http_request(start_line, tbody, _upstream_host_hdr, auth_hdrs);
        }
        else
        {
            upstream_bytes.assign(c->rbuf.data(), c->msg.total_len);
        }

        Connection* u = ep_acquire_upstream();
        if (!u) { ep_error_respond(c, 502); return; }

        u->wbuf = std::move(upstream_bytes);
        u->woff = 0;
        c->rbuf.erase(0, c->msg.total_len);
        c->peer = u;
        u->peer = c;
        c->ever_framed = true;        // past the setup deadline for good
        c->ts_req_built = now_ns();   // end of OUR request-side work
        c->ts_up_activity = c->ts_req_built; // idle-timeout baseline for this request
        if (u->connected) c->ts_wire_ready = c->ts_req_built; // pooled: no handshake

        // Optimistic send: if the pooled upstream is already connected (the common
        // case), write immediately and only arm EPOLLOUT if the socket buffer is
        // full. Arming unconditionally costs two epoll_ctl calls + an extra wakeup
        // per request; the client-response leg already writes this way.
        if (u->connected)
        {
#ifdef LLMBRIDGE_HAVE_TLS
            if (u->tls)
            {
                // Pooled TLS conns are always past the handshake (a fresh conn is
                // never `connected` here). Push the plaintext through the session
                // and flush the ciphertext; partial flushes finish on writability.
                tls_push_wbuf(u);
                bool done = false;
                if (!ep_tls_flush(u, &done))
                {
                    if (!ep_retry_upstream(u)) ep_error_respond(c, 502);
                    return;
                }
                if (done && tls_wbuf_flushed(u)) c->ts_up_sent = now_ns();
                return;
            }
#endif
            bool done = false;
            if (!ep_pump_write(u, &done)) { if (!ep_retry_upstream(u)) ep_error_respond(c, 502); return; }
            if (done) c->ts_up_sent = now_ns(); // request fully sent (end of request path)
            else ep_arm_write(u);               // socket full; finish on writability
        }
        else
        {
            ep_arm_write(u); // connect pending; ep_on_upstream_writable sends once it completes
        }
    }

    void Gateway::ep_on_upstream_writable(Connection* u) noexcept
    {
        if (!u->connected)
        {
            int err = net::connect_result(u->fd);
            if (err != 0)
            {
                Connection* client = u->peer;
                u->peer = nullptr;
                ep_close_upstream(u);
                if (client) { client->peer = nullptr; ep_error_respond(client, 502); }
                else ++_stats.errors;
                return;
            }
            u->connected = true;
            // t2 for a PLAINTEXT upstream: the socket can carry the request now.
            // A TLS upstream is not wire-ready yet; tls_feed() stamps t2 when the
            // handshake completes.
            if (!upstream_is_tls(u) && u->peer && u->peer->ts_wire_ready == 0)
                u->peer->ts_wire_ready = now_ns();
#ifdef LLMBRIDGE_HAVE_TLS
            if (u->tls && !u->tls->handshake_done())
                u->tls->start_handshake(); // ClientHello lands in the write BIO
#endif
        }
#ifdef LLMBRIDGE_HAVE_TLS
        if (u->tls)
        {
            const bool was_flushed = u->peer && tls_wbuf_flushed(u);
            bool tdone = false;
            if (!ep_tls_flush(u, &tdone))
            {
                if (u->peer) { if (!ep_retry_upstream(u)) ep_error_respond(u->peer, 502); }
                else ep_close_upstream(u);
                return;
            }
            if (!tdone) return; // EPOLLOUT re-armed by ep_tls_flush
            ep_disarm_write(u); // ciphertext drained; handshake replies arrive via read
            if (u->peer && !was_flushed && tls_wbuf_flushed(u))
                u->peer->ts_up_sent = now_ns();
            return;
        }
#endif
        bool done = false;
        if (!ep_pump_write(u, &done))
        {
            if (u->peer) { if (!ep_retry_upstream(u)) ep_error_respond(u->peer, 502); }
            else ep_close_upstream(u);
            return;
        }
        if (!done) { ep_arm_write(u); return; }
        ep_disarm_write(u);
        if (u->peer) u->peer->ts_up_sent = now_ns(); // end of request-path work
    }

    void Gateway::ep_on_upstream_readable(Connection* u) noexcept
    {
        Connection* client = u->peer;
        const bool read_ok =
#ifdef LLMBRIDGE_HAVE_TLS
            u->tls ? ep_tls_drain_read(u) :
#endif
                     ep_drain_read(u);
        if (!read_ok)
        {
#ifdef LLMBRIDGE_HAVE_TLS
            // TLS parity with the io_uring path: a FATAL session error (bad record,
            // MAC failure) mid-stream must ABORT the client, never finalize the
            // stream as clean: a corrupted stream that ends in a well-formed
            // [DONE] would hide the corruption from the client entirely. Only a
            // real transport EOF may end a close-delimited stream normally.
            if (u->tls && u->tls->want() == net::tls::Want::Error && client && client->streaming)
            {
                ep_abort_pair(client);
                return;
            }
#endif
            if (client && client->streaming) { ep_stream_on_upstream_eof(u); return; }
            if (client == nullptr) ep_close_upstream(u); // idle pooled conn dropped (eviction)
            else if (!ep_retry_upstream(u)) ep_error_respond(client, 502);
            return;
        }
        if (client == nullptr) return; // stray bytes on an idle pooled conn
        client->ts_up_activity = now_ns(); // upstream made progress

        // Mid-stream: pump the newly-arrived body bytes and return.
        if (client->streaming) { ep_stream_pump(u); return; }

        // First response bytes: for the Anthropic translate path, peek the head to
        // decide whole-body vs streaming (text/event-stream). Other modes and
        // non-streaming responses fall through to the whole-body path unchanged.
        if (_translate == TranslateMode::Anthropic)
        {
            net::http::ResponseHead h;
            const auto hs = net::http::parse_response_head(u->rbuf, h);
            if (hs == net::http::FrameStatus::NeedMore) return;
            if (hs == net::http::FrameStatus::Error) { ep_error_respond(client, 502); return; }
            // Only a 200 carries a real event stream. A provider error (429 rate
            // limit, 529 overloaded, 400 context length, 401 auth) must reach the
            // client with ITS status (relayed below once the body is framed)
            // never laundered into a 200 stream.
            if (h.event_stream && h.status == 200)
            {
                // t4 for a stream: the provider's response HEAD is now complete.
                // NOT the first token, and not the first data chunk. A provider
                // MAY send 200 + content-type as soon as it ACCEPTS the stream,
                // well before its first generated token. Anthropic does NOT: measured
                // 2026-08-06, its head trails its first token by ~1 ms, so for that
                // provider this tracks TTFT closely. That is a per-provider fact, not
                // a protocol guarantee; see LATENCY.md §3 before assuming it holds.
                // The non-streaming path stamps this after framing, which this branch
                // returns before reaching, so stamp it here or it stays 0 and the
                // TTFB timing header reports garbage.
                client->ts_up_recvd = now_ns();
                ep_begin_stream(u, h);
                return;
            }
        }

        // Stamp just before framing so the response HTTP parse is counted in the
        // response-path overhead, without charging inter-packet network wait.
        const int64_t t0 = now_ns();
        // parse_response, NOT parse: real providers return non-streaming bodies
        // chunked over HTTP/1.1, which parse_request() rejects by design (see http.hpp).
        // `r.body` aliases rbuf (Content-Length) or _resp_scratch (chunked); it is
        // dead before rbuf is erased or the upstream released, below.
        const auto r = net::http::parse_response(u->rbuf, u->rdec);
        if (r.failed()) { ep_error_respond(client, 502); return; }
        if (!r.complete()) return;
        const net::http::ResponseHead& h = r.head;
        const std::string_view body_buf = r.body;
        const size_t total_len = r.total_len;

        client->ts_up_recvd = t0; // end of upstream wait (stamped pre-framing)

        if (_translate != TranslateMode::None)
        {
            const std::string_view body = body_buf;
            // Relay a provider failure with ITS OWN status + message (rate limit,
            // overloaded GPU, context length, auth); translating a non-200 body as
            // if it were a completion would fail and mask it as a generic 502.
            if (h.status != 0 && h.status != 200)
            {
                client->wbuf = build_http_status(
                    h.status, reason_for(h.status),
                    provider::upstream_error_to_openai(body, "upstream_error"));
                client->woff = 0;
                client->peer = nullptr;
                if (h.keep_alive) ep_release_upstream(u); else ep_close_upstream(u);
                ++_stats.errors;
                ep_respond(client);
                return;
            }
            std::string tbody = xlate_resp(_translate, body);
            if (tbody.empty())
            {
                client->peer = nullptr;
                ep_release_upstream(u); // framing was valid; the upstream conn is reusable
                ep_error_respond(client, 502);
                return;
            }
            std::string timing;
            if (_timing_headers)
            {
                // t5: the response is built here and written immediately after.
                const int64_t ts_resp_built = now_ns();
                const TimingSplit sp = timing_split(
                    client->ts_req_recvd, client->ts_req_built, client->ts_wire_ready,
                    client->ts_up_sent, client->ts_up_recvd, ts_resp_built);
                append_timing_headers(timing, client->ts_req_recvd, sp.compute_ns / 1000,
                                      sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                      sp.upstream_ns / 1000, "x-llmbridge-upstream-us");
                append_usage_headers(timing, tbody);
            }
            client->wbuf = build_http("HTTP/1.1 200 OK", tbody, timing);
        }
        else
        {
            // Passthrough: forward the upstream's own bytes. A chunked response is
            // re-framed with Content-Length instead of relayed verbatim; we have
            // already decoded it, and handing the client a chunked body we did not
            // re-verify would push our framing problem downstream.
            if (h.chunked) client->wbuf = build_http("HTTP/1.1 200 OK", body_buf);
            else client->wbuf.assign(u->rbuf.data(), total_len);
        }
        client->woff = 0;

        // Response fully read -> upstream is free. Pool it only if it will stay
        // open (response keep-alive, and for passthrough the client didn't ask to
        // close); otherwise it's about to close, so drop it instead of reuse a
        // stale connection.
        const bool pool_upstream =
            h.keep_alive && (_translate != TranslateMode::None || client->msg.keep_alive);
        client->peer = nullptr;
        // Drop the framed message so a pipelined next response is not mis-read as
        // part of this one; anything left is the start of the next message.
        u->rbuf.erase(0, total_len);
        u->rdec.reset(); // next response on this conn decodes from a clean state
        if (pool_upstream) ep_release_upstream(u);
        else ep_close_upstream(u);
        ep_respond(client);
    }

    void Gateway::ep_respond(Connection* c) noexcept
    {
        bool done = false;
        if (!ep_pump_write(c, &done)) { ep_close_client(c); ++_stats.errors; return; }
        if (!done) { ep_arm_write(c); return; } // socket full; finish on writability
        ep_finish_client(c);
    }

    void Gateway::ep_on_client_writable(Connection* c) noexcept
    {
        if (c->streaming) { ep_stream_flush(c); return; } // pump path has its own drain logic
        bool done = false;
        if (!ep_pump_write(c, &done)) { ep_abort_pair(c); return; }
        if (!done) { ep_arm_write(c); return; }
        ep_finish_client(c);
    }

    void Gateway::ep_finish_client(Connection* c) noexcept
    {
        // Error replies (close_after_resp) are counted in _stats.errors, not the
        // latency histograms; their timing stamps are unset and would be garbage.
        if (!c->close_after_resp)
        {
            const int64_t ts_resp_sent = now_ns();
            if (ts_resp_sent - _t_start >= _warmup_ns)
            {
                // req_path is OUR request-side work only; the wait for connect +
                // TLS is its own histogram so it cannot inflate the added-latency
                // claim. overhead = req_path + resp_path, connect excluded.
                //
                // Derived from the SAME timing_split() the headers use, so
                // connect(TLS) here and x-llmbridge-connect-us there cannot come to
                // mean different things. t5 does not enter this grouping, so t4 is
                // passed in its place.
                const TimingSplit sp = timing_split(c->ts_req_recvd, c->ts_req_built,
                                                    c->ts_wire_ready, c->ts_up_sent,
                                                    c->ts_up_recvd, c->ts_up_recvd);
                const int64_t conn_ns = sp.connect_ns;
                const int64_t req_ns = sp.req_path_ns;
                const int64_t resp_ns = ts_resp_sent - c->ts_up_recvd;
                if (req_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(req_ns));
                if (conn_ns >= 0) _stats.connect.record(static_cast<uint64_t>(conn_ns));
                if (resp_ns >= 0) _stats.resp_path.record(static_cast<uint64_t>(resp_ns));
                if (req_ns >= 0 && resp_ns >= 0)
                    _stats.overhead.record(static_cast<uint64_t>(req_ns + resp_ns));
                ++_stats.requests;
            }
        }
        ep_disarm_write(c);
        c->wbuf.clear(); // response fully sent; ep_pump_write no longer clears it for us
        c->woff = 0;
        const bool close_now = c->close_after_resp || !c->msg.keep_alive;
        c->msg = net::http::Message{};
        if (close_now) { ep_close_client(c); return; }
        ep_on_client_readable(c); // service any pipelined next request already in rbuf
    }

    // ── Streaming pump (epoll, Anthropic->OpenAI SSE) ───────────────────────
    // Enter streaming: the upstream response is text/event-stream. Send the
    // client SSE headers (close-delimited) and translate the body as it arrives.
    void Gateway::ep_begin_stream(Connection* u, const net::http::ResponseHead& h) noexcept
    {
        Connection* client = u->peer;
        client->streaming = true;
        client->stream_chunked = h.chunked;
        client->stream_keep_alive = h.keep_alive; // decides poolability at stream end
        client->sse = std::make_unique<provider::AnthropicToOpenAiSse>(-1, client->wants_usage);
        if (_timing_headers)
        {
            // t4 = provider's first response byte, stamped by the caller.
            std::string timing;
            // t5 = t4: a stream's response is not built at one instant, so the
            // compute leg is the request side alone instead of an invented figure.
            const TimingSplit sp = timing_split(
                client->ts_req_recvd, client->ts_req_built, client->ts_wire_ready,
                client->ts_up_sent, client->ts_up_recvd, client->ts_up_recvd);
            append_timing_headers(timing, client->ts_req_recvd, sp.compute_ns / 1000,
                                  sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                  sp.upstream_ns / 1000, "x-llmbridge-upstream-ttfb-us");
            client->wbuf.assign(sse_head_with_timing(timing));
        }
        else
            client->wbuf.assign(kSseHead);
        client->woff = 0;

        u->rbuf.erase(0, h.header_len); // consume the head; the rest is body
        ep_stream_pump(u);                 // translate any initial body + flush headers
    }

    // Decode + translate the upstream body bytes now sitting in u->rbuf, appending
    // OpenAI SSE to the client's write buffer, then flush.
    void Gateway::ep_stream_pump(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { ep_close_upstream(u); return; } // lost peer mid-stream

        const StreamStep st = stream_step(client, u->rbuf, client->wbuf, /*at_eof=*/false);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            // Truncate honestly: flush what we already translated, then close
            // WITHOUT a terminal [DONE] so the client sees an aborted stream
            // instead of a fabricated clean finish.
            client->stream_ended = true;
            client->close_after_resp = true;
            ++_stats.errors;
            ep_stream_flush(client);
            return;
        }
        ep_stream_flush(client);
    }

    // Upstream closed the connection: translate whatever remains, emit the terminal
    // [DONE] if we haven't, and finalize.
    void Gateway::ep_stream_on_upstream_eof(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { ep_close_upstream(u); return; }

        const StreamStep st = stream_step(client, u->rbuf, client->wbuf, /*at_eof=*/true);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            client->stream_ended = true;
            client->close_after_resp = true;
            ++_stats.errors;
        }
        ep_stream_flush(client);
    }

    // Write buffered SSE to the client. If it doesn't all go, finish on writability
    // and pause upstream reads (backpressure). On full flush, resume upstream, or
    // finalize if the stream has ended.
    void Gateway::ep_stream_flush(Connection* client) noexcept
    {
        bool done = false;
        if (!ep_pump_write(client, &done)) { ep_abort_pair(client); return; } // client gone
        if (!done)
        {
            ep_arm_write(client);
            if (client->peer && !client->peer->doomed) ep_pause_read(client->peer);
            return;
        }
        client->wbuf.clear();
        client->woff = 0;
        ep_disarm_write(client);
        if (client->stream_ended) { ep_finalize_stream(client); return; }
        if (client->peer && !client->peer->doomed) ep_resume_read(client->peer);
    }

    // Stream complete: drop the upstream (a just-streamed conn isn't pooled),
    // count the request, and close the client (which delimits the SSE body).
    void Gateway::ep_finalize_stream(Connection* client) noexcept
    {
        if (Connection* u = client->peer)
        {
            const bool reusable = stream_upstream_reusable(client, u);
            client->peer = nullptr;
            u->peer = nullptr;
            // Reuse is the whole point: a streaming request otherwise costs a fresh
            // upstream connect EVERY time, which measured as the dominant term in
            // time-to-first-token. Pool it when the framing says that is safe.
            if (reusable) ep_release_upstream(u);
            else ep_close_upstream(u);
        }
        // Only a stream that terminated cleanly counts as a served request; an
        // aborted one (close_after_resp) was already counted in _stats.errors.
        if (!client->close_after_resp) ++_stats.requests; // latency histograms N/A
        ep_close_client(client);
    }

    // Abort requests whose upstream has gone silent. Runs on the loop's existing
    // periodic tick, so an idle gateway costs one cheap scan per tick. A client
    // that hasn't been answered yet gets a real 504; a live stream (headers already
    // sent) is closed WITHOUT a terminal [DONE], so the client sees a truncated
    // stream instead of a fabricated clean finish.
    void Gateway::sweep_idle(bool uring) noexcept
    {
        const int64_t now = now_ns();
        if (now - _last_sweep_ns < 50'000'000LL) return; // at most ~20 sweeps/sec
        _last_sweep_ns = now;

        // Reap idle pooled upstreams. Providers close idle keep-alives on their own
        // schedule, and discovering a corpse costs a request its retry, so drop them
        // first. Pooled conns have peer == nullptr, so the in-flight scan below skips
        // them and would otherwise hold them forever.
        for (size_t i = 0; i < _idle_upstreams.size();)
        {
            Connection* u = _idle_upstreams[i];
            if (u->ts_pooled != 0 && now - u->ts_pooled > kIdleUpstreamNs)
            {
                _idle_upstreams.erase(_idle_upstreams.begin() + static_cast<long>(i));
#ifdef LLMBRIDGE_HAVE_URING
                if (uring) { ur_close(u); continue; }
#endif
                ep_close_upstream(u);
                continue;
            }
            ++i;
        }

        // Drop clients that never finished setting up. This runs BEFORE the
        // upstream-idle early return below, deliberately: the two are unrelated,
        // and a deployment with the upstream timeout disabled still must not let a
        // peer hold a connection open forever by sending nothing.
        //
        // Measured before this existed: 50 half-open connections, each holding a
        // slot with a partial request, and the gateway closed none of them. On a
        // loopback sidecar that is nearly harmless. On an internet-facing listener
        // it is a resource-exhaustion vector that costs an attacker one packet.
        {
            std::vector<Connection*> unfinished;
            for (auto& [id, c] : _clients)
            {
                if (c->doomed || c->ever_framed || c->ts_accepted == 0) continue;
                if (now - c->ts_accepted > kClientSetupNs) unfinished.push_back(c);
            }
            for (Connection* c : unfinished)
            {
                ++_stats.client_setup_timeouts;
#ifdef LLMBRIDGE_HAVE_URING
                if (uring) { ur_close(c); continue; }
#endif
                ep_close_client(c);
            }
        }

        // The in-flight abort below is gated on the upstream idle timeout; pool
        // eviction above is not; they are independent settings.
        if (_upstream_idle_ns <= 0) return;

        // Collect first: the teardown below erases from _clients.
        std::vector<Connection*> stale;
        for (auto& [id, c] : _clients)
        {
            if (c->doomed) continue;
            const bool in_flight = c->peer != nullptr || c->streaming;
            if (!in_flight || c->ts_up_activity == 0) continue;
            if (now - c->ts_up_activity > _upstream_idle_ns) stale.push_back(c);
        }
        for (Connection* c : stale)
        {
            ++_stats.upstream_timeouts;
            const bool streaming = c->streaming;
            if (streaming)
            {
                // Response headers are already out; truncate honestly (no [DONE]).
                c->stream_ended = true;
                c->close_after_resp = true;
                ++_stats.errors;
            }
#ifdef LLMBRIDGE_HAVE_URING
            if (uring)
            {
                if (streaming) ur_abort_pair(c);
                else ur_error_respond(c, 504);
                continue;
            }
#else
            (void)uring;
#endif
            if (streaming) ep_abort_pair(c);
            else ep_error_respond(c, 504);
        }
    }

    int Gateway::run()
    {
#ifdef LLMBRIDGE_HAVE_URING
        if (_uring_active) return run_uring();
#endif
        return run_epoll();
    }

    int Gateway::run_epoll()
    {
        _t_start = now_ns();
        epoll_event events[kMaxEvents];

        while (!_stop)
        {
            // kPollTickMs timeout so request_stop() is observed within a tick.
            int n = ::epoll_wait(_epfd, events, kMaxEvents, kPollTickMs);
            if (n < 0) { if (errno == EINTR) continue; break; }
            for (int i = 0; i < n; ++i)
            {
                Connection* c = static_cast<Connection*>(events[i].data.ptr);
                if (c == _listen_conn) { ep_on_accept(); continue; }
                if (c->doomed) continue; // freed earlier this batch
                // Unlike kqueue, epoll coalesces a fd's readiness into one entry,
                // so a single event can carry both EPOLLIN and EPOLLOUT. Error/
                // hangup conditions fold into the readable path (ep_drain_read then
                // reports the EOF/error). Re-check doomed between the two halves.
                const uint32_t e = events[i].events;
                const bool readable = e & (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP);
                const bool writable = e & EPOLLOUT;
                if (c->is_client)
                {
                    if (readable) ep_on_client_readable(c);
                    if (writable && !c->doomed) ep_on_client_writable(c);
                }
                else
                {
                    if (writable) ep_on_upstream_writable(c);
                    if (readable && !c->doomed) ep_on_upstream_readable(c);
                }
            }
            sweep_idle(/*uring=*/false); // abort requests whose upstream went silent
            for (Connection* d : _doomed) delete d;
            _doomed.clear();
        }
        return 0;
    }

#ifdef LLMBRIDGE_HAVE_URING
    // ════════════════════════════════════════════════════════════════════════
    // io_uring backend (Phase 1): a completion-driven mirror of the epoll loop.
    // Each request advances a small per-connection state machine: every CQE says
    // "this op finished with N bytes," we act, and submit the next op. A conn is
    // freed only when its `inflight` SQEs have all completed (no use-after-free on
    // a completion that lands after we close).
    // ════════════════════════════════════════════════════════════════════════
    namespace
    {
        enum UOp : uint64_t { UAccept = 0, URecv = 1, USend = 2, UConnect = 3, UTimer = 4, UCancel = 5 };
        constexpr uint64_t kTagMask = 7;
        inline uint64_t make_ud(Connection* c, UOp op) { return reinterpret_cast<uintptr_t>(c) | op; }
        inline Connection* ud_conn(uint64_t d) { return reinterpret_cast<Connection*>(d & ~kTagMask); }
        inline UOp ud_op(uint64_t d) { return static_cast<UOp>(d & kTagMask); }

        constexpr unsigned kRingDepth = 4096;
        // Provided-buffer pool for multishot recv: plenty so a connection always has
        // a buffer to land in (we recycle each immediately after copying it out).
        constexpr unsigned kBufGroup = 1;
        constexpr unsigned kBufCount = 4096; // power of two
        constexpr unsigned kBufSize = 4096;

        // Cap on buffered SSE output for one stream. SSE is model-rate-limited, so
        // this only trips for a pathologically slow client, instead of buffer
        // without bound we drop that stream. (The epoll pump instead pauses reads;
        // for a model-rate stream the cap is equivalent in practice.)
        constexpr size_t kStreamBufCap = 8 << 20; // 8 MiB
    } // namespace

    bool Gateway::ur_next_sqe(io_uring_sqe** out) noexcept
    {
        io_uring_sqe* s = _ring.get_sqe();
        if (!s) { _ring.submit(); s = _ring.get_sqe(); } // SQ full: flush, retry once
        *out = s;
        return s != nullptr;
    }

    void Gateway::ur_submit_accept() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return;
        s->opcode = IORING_OP_ACCEPT;
        s->fd = _listen_fd;
        s->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
        s->ioprio = IORING_ACCEPT_MULTISHOT; // one SQE, a completion per accepted fd
        s->user_data = make_ud(_listen_conn, UAccept);
    }

    void Gateway::ur_submit_timer() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return;
        s->opcode = IORING_OP_TIMEOUT;
        s->addr = reinterpret_cast<uint64_t>(&_uring_ts);
        s->len = 1;
        s->user_data = UTimer; // conn = nullptr
    }

    bool Gateway::ur_arm_recv(Connection* c) noexcept
    {
        // Multishot recv drawing from the provided-buffer pool: one submission keeps
        // delivering a completion per data arrival (each naming the buffer it used),
        // so we never re-submit a recv per read. Armed once per connection; only
        // re-armed if the kernel ends the multishot (e.g. pool exhaustion).
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) { ur_close(c); return false; }
        s->opcode = IORING_OP_RECV;
        s->fd = c->fd;
        s->addr = 0;
        s->len = 0;
        s->flags |= IOSQE_BUFFER_SELECT;
        s->buf_group = kBufGroup;
        s->ioprio |= IORING_RECV_MULTISHOT;
        s->user_data = make_ud(c, URecv);
        ++c->inflight;
        ++_uring_inflight;
        return true;
    }

    bool Gateway::ur_submit_send(Connection* c) noexcept
    {
        io_uring_sqe* s = nullptr;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_invariant_ok(c)) { ur_close(c); return false; } // never plaintext
#endif
        if (!ur_next_sqe(&s)) { ur_close(c); return false; }
        s->opcode = IORING_OP_SEND;
        s->fd = c->fd;
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            // TLS on either leg: the wire sees ciphertext. wbuf (plaintext) is fed
            // to the Session elsewhere; woff tracks that, not this send.
            s->addr = reinterpret_cast<uint64_t>(c->tls_out.data() + c->tls_out_off);
            s->len = static_cast<unsigned>(c->tls_out.size() - c->tls_out_off);
        }
        else
#endif
        {
            s->addr = reinterpret_cast<uint64_t>(c->wbuf.data() + c->woff);
            s->len = static_cast<unsigned>(c->wbuf.size() - c->woff);
        }
        s->user_data = make_ud(c, USend);
        ++c->inflight;
        ++_uring_inflight;
        return true;
    }

    bool Gateway::ur_submit_connect(Connection* u) noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) { ur_abort_pair(u->peer); return false; }
        s->opcode = IORING_OP_CONNECT;
        s->fd = u->fd;
        s->addr = reinterpret_cast<uint64_t>(&_upstream_addr);
        s->off = sizeof(_upstream_addr); // connect addrlen rides in `off`
        s->user_data = make_ud(u, UConnect);
        ++u->inflight;
        ++_uring_inflight;
        return true;
    }

    void Gateway::ur_submit_cancel(int fd) noexcept
    {
        // Cancel every in-flight op on `fd` (notably the armed multishot recv, which
        // shutdown() does NOT terminate). The cancelled ops complete with -ECANCELED,
        // releasing their inflight slots so the conn can be freed / the drain finishes.
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return;
        s->opcode = IORING_OP_ASYNC_CANCEL;
        s->fd = fd;
        s->cancel_flags = IORING_ASYNC_CANCEL_FD;
        s->user_data = UCancel; // sentinel: not inflight-counted, completion ignored
    }

#ifdef LLMBRIDGE_HAVE_TLS
    void Gateway::ur_tls_flush(Connection* u) noexcept
    {
        // Serialized sends, same discipline as the client streaming pump: a SEND
        // SQE points into tls_out, so tls_out must be IMMUTABLE while one is in
        // flight; appending could reallocate it under the kernel. Ciphertext
        // produced meanwhile stages inside the Session's write BIO; we pump it
        // out here once the previous send has fully completed.
        if (u->send_inflight) return;
        if (u->tls_out_off >= u->tls_out.size())
        {
            u->tls_out.clear();
            u->tls_out_off = 0;
            tls_pump_out(u);
        }
        if (u->tls_out.empty()) return;
        u->send_inflight = true;
        ur_submit_send(u);
    }
#endif

    // Send wbuf to a CLIENT connection, whatever the transport.
    //
    // The plaintext path submits a send straight out of wbuf. A TLS conn cannot:
    // the SQE points at tls_out, so the plaintext has to go through the Session
    // first or the kernel is handed a zero-length send. That is exactly the bug
    // this helper exists to make unrepeatable. It was found by running curl
    // against both backends, and never by reading the code.
    void Gateway::ur_client_send(Connection* c) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            if (c->tls->handshake_done()) tls_push_wbuf(c);
            ur_tls_flush(c);
            return;
        }
#endif
        ur_submit_send(c);
    }

    Connection* Gateway::ur_acquire_upstream() noexcept
    {
        if (!_idle_upstreams.empty())
        {
            Connection* u = _idle_upstreams.back();
            _idle_upstreams.pop_back();
            u->from_pool = true; // reused -> a pre-response failure is retry-eligible
            u->retried = false;  // fresh request: one retry available again
            ++_stats.upstream_reused;
            return u;
        }
        const int fd = net::make_client_socket();
        if (fd < 0) return nullptr;
        Connection* u = new Connection();
        u->fd = fd;
        u->is_client = false;
        u->connected = false;
        u->from_pool = false;
        u->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (_tls.upstream_tls && !tls_attach_upstream(u))
        {
            ::close(fd);
            delete u;
            return nullptr;
        }
#endif
        ++_stats.upstream_conns_opened;
        return u;
    }

    bool Gateway::ur_retry_upstream(Connection* u) noexcept
    {
        // Only safe to resend when the upstream was a pooled reuse, hasn't been
        // retried, and gave us ZERO response bytes; i.e. the provider closed the
        // idle keep-alive connection without processing the request. (Industry
        // convention: retry an idempotent-or-idle-reused request that failed before
        // any response; don't retry once a partial response has been seen.)
        if (!u->from_pool || u->retried || !u->rbuf.empty()) return false;
        Connection* client = u->peer;
        if (!client) return false;
        const int fd = net::make_client_socket();
        if (fd < 0) return false;

        Connection* uf = new Connection();
        uf->fd = fd;
        uf->is_client = false;
        uf->connected = false;
        uf->from_pool = false;
        uf->retried = true; // this request's one allowed retry is now spent
        // PLAINTEXT invariant pays off here: wbuf was never consumed by the dead
        // session (woff only tracked what was FED, wbuf itself stayed whole), so
        // the retry re-pushes the identical request through a brand-new session.
        uf->wbuf = std::move(u->wbuf);
        uf->woff = 0;
        uf->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (_tls.upstream_tls && !tls_attach_upstream(uf))
        {
            ::close(fd);
            delete uf;
            return false;
        }
#endif
        ++_stats.upstream_conns_opened;

        u->peer = nullptr;
        ur_close(u); // discard the dead pooled connection
        client->peer = uf;
        uf->peer = client;
        ++_stats.upstream_retries;
        ur_submit_connect(uf); // connect fresh, then send on completion
        return true;
    }

    void Gateway::ur_release_upstream(Connection* u) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        u->tls_out.clear(); // per-request ciphertext; Session kept for reuse
        u->tls_out_off = 0;
        u->send_inflight = false;
#endif
        // See ep_release_upstream: bounded pool, closed past the cap.
        if (_idle_upstreams.size() >= kMaxIdleUpstreams) { ur_close(u); return; }
        u->peer = nullptr;
        u->rbuf.clear();
        u->rdec.reset();
        secure_clear(u->wbuf); // see ep_release_upstream: credential must not idle in the pool
        u->woff = 0;
        u->msg = net::http::Message{};
        u->ts_pooled = now_ns(); // idle-eviction baseline
        _idle_upstreams.push_back(u);
    }

    void Gateway::ur_close(Connection* c) noexcept
    {
        if (c->doomed) return;
        if (c->is_client && c->id) _clients.erase(c->id);
        for (auto it = _idle_upstreams.begin(); it != _idle_upstreams.end(); ++it)
            if (*it == c) { _idle_upstreams.erase(it); break; }
        // Force any in-flight op on this fd to complete so its inflight count drains
        // (the fd is closed at free time). shutdown alone does NOT end an armed
        // multishot recv, so also cancel everything on the fd.
        if (c->fd >= 0) { ::shutdown(c->fd, SHUT_RDWR); ur_submit_cancel(c->fd); }
        c->doomed = true;
        _doomed.push_back(c);
        ur_maybe_free(c);
    }

    void Gateway::ur_abort_pair(Connection* client) noexcept
    {
        if (!client) return;
        if (Connection* u = client->peer) { u->peer = nullptr; ur_close(u); }
        ur_close(client);
        ++_stats.errors;
    }

    void Gateway::ur_error_respond(Connection* client, int code) noexcept
    {
        if (!client || client->doomed) return;
        if (Connection* u = client->peer) { client->peer = nullptr; u->peer = nullptr; ur_close(u); }
        client->wbuf = build_error(code);
        client->woff = 0;
        client->close_after_resp = true; // ur_finish_client closes once the reply flushes
        ++_stats.errors;
        ur_client_send(client);
    }

    void Gateway::ur_maybe_free(Connection* c) noexcept
    {
        if (!c->doomed || c->inflight > 0) return; // a completion still references it
        if (c->fd >= 0) { ::close(c->fd); c->fd = -1; }
        for (auto it = _doomed.begin(); it != _doomed.end(); ++it)
            if (*it == c) { _doomed.erase(it); break; }
        delete c;
    }

    void Gateway::ur_on_cqe(uint64_t user_data, int res, uint32_t flags) noexcept
    {
        const UOp op = ud_op(user_data);
        if (op == UTimer)
        {
            if (!_draining && !_stop) { sweep_idle(/*uring=*/true); ur_submit_timer(); }
            return;
        }
        if (op == UCancel) return; // control op (cancel-by-fd); not inflight-counted
        if (op == UAccept) { ur_on_accept(res, flags); return; } // not inflight-counted

        Connection* c = ud_conn(user_data);
        // A multishot recv stays armed only while it keeps delivering data (F_MORE
        // set AND res > 0). Its terminal completion (EOF/error, res <= 0) can still
        // carry F_MORE, so it must release the inflight slot, or the drain
        // never reaches zero. Only the consuming (terminal) completion decrements.
        const bool armed = (op == URecv) && (flags & IORING_CQE_F_MORE) && res > 0;
        if (!armed) { --c->inflight; --_uring_inflight; }

        if (_draining || c->doomed)
        {
            if (op == URecv && (flags & IORING_CQE_F_BUFFER))
                _bufring.recycle(flags >> IORING_CQE_BUFFER_SHIFT); // return the provided buffer
            if (!armed) ur_maybe_free(c);
            return;
        }
        switch (op)
        {
            case URecv: ur_on_recv(c, res, flags); break;
            case USend: ur_on_send(c, res); break;
            case UConnect: ur_on_connect(c, res); break;
            default: break;
        }
    }

    void Gateway::ur_on_accept(int res, uint32_t flags) noexcept
    {
        if (_stop || _draining)
        {
            if (res >= 0) ::close(res); // shutting down: don't take new work
            return;
        }
        if (!(flags & IORING_CQE_F_MORE)) ur_submit_accept(); // multishot ended -> re-arm
        if (res < 0) return;                                 // transient accept error
        const int fd = res;
        net::set_nodelay(fd);
        Connection* c = new Connection();
        c->fd = fd;
        c->is_client = true;
        c->id = _next_client_id++;
        c->ts_accepted = now_ns();
        c->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (_tls.client_tls && !tls_attach_client(c))
        {
            // Fail CLOSED, exactly as the epoll twin does. No recv is armed yet, so
            // there is nothing in flight and the Connection can be freed directly
            // instead of going on the doomed list.
            ::close(fd);
            delete c;
            ++_stats.errors;
            return;
        }
#endif
        _clients[c->id] = c;
        ur_arm_recv(c); // multishot recv stays armed for the connection's life
    }

    void Gateway::ur_on_recv(Connection* c, int res, uint32_t flags) noexcept
    {
        const bool armed = flags & IORING_CQE_F_MORE;

        // -ENOBUFS is NOT a connection error: the provided-buffer pool was momentarily
        // empty, so the kernel declined to pick a buffer and ended the multishot without
        // consuming one. The cure is to re-arm, not to tear down. Without this branch the
        // fallthrough below would kill the client pair or, mid-stream, report a premature
        // stream end.
        //
        // Measured scope, so nobody mistakes this for a fix to a live bug: on this kernel
        // (7.0) with kBufCount=4096, `uring_enobufs` stayed at **0** even at 8192
        // concurrent streams (16384 armed recvs, 4x the pool). The kernel ends the
        // multishot with res>0 and F_MORE clear under pool pressure, which the re-arm at
        // the bottom of this function already handles. So this is DEFENSIVE hardening for
        // a path that is reachable by contract but was never observed in practice; it is
        // NOT the explanation for io_uring's high-concurrency tail latency (that was
        // hypothesised from the kBufCount correlation and disproved by this counter).
        // The regression test forces the condition by shrinking the pool.
        if (res == -ENOBUFS)
        {
            ++_stats.uring_enobufs;
            ur_arm_recv(c);
            return;
        }

        if (res <= 0) // multishot ended: EOF (0) or error (<0)
        {
            if (flags & IORING_CQE_F_BUFFER) _bufring.recycle(flags >> IORING_CQE_BUFFER_SHIFT);
            if (c->is_client) { if (c->peer) ur_abort_pair(c); else ur_close(c); }
            else if (c->peer && c->peer->streaming) ur_stream_on_upstream_eof(c); // stream end, not a failure
            else if (!ur_retry_upstream(c)) { if (c->peer) ur_error_respond(c->peer, 502); else ur_close(c); }
            return;
        }

        // Copy the bytes out of the kernel-selected buffer, then return it to the
        // pool. For a TLS upstream the bytes are ciphertext: they go through the
        // Session (tls_feed), whose plaintext lands in rbuf, so the parse logic
        // below sees only plaintext either way.
        bool tls_ok = true;
        if (flags & IORING_CQE_F_BUFFER)
        {
            const unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
#ifdef LLMBRIDGE_HAVE_TLS
            if (c->tls)
                tls_ok = tls_feed(c, _bufring.data(bid), static_cast<size_t>(res));
            else
#endif
                c->rbuf.append(_bufring.data(bid), static_cast<size_t>(res));
            _bufring.recycle(bid);
        }
        if (!armed) ur_arm_recv(c); // kernel ended the multishot (pool pressure) -> re-arm
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_ok)
        {
            // Fatal TLS failure (bad record, MAC failure, refused certificate).
            // Deliberately NOT ur_stream_on_upstream_eof for a mid-stream failure: a
            // corrupted stream must not be finalized as if it ended cleanly.
            if (c->peer && c->peer->streaming) { ur_abort_pair(c->peer); return; }
            // An inbound TLS failure is the CLIENT's connection dying, not a
            // retryable upstream fault. Retrying it would resend the request to the
            // provider on behalf of a peer that can no longer receive the answer.
            if (c->is_client) { if (c->peer) ur_abort_pair(c); else ur_close(c); return; }
            if (!ur_retry_upstream(c))
            {
                if (c->peer) ur_error_respond(c->peer, 502);
                else ur_close(c);
            }
            return;
        }
        if (c->tls)
            ur_tls_flush(c); // handshake replies / newly-pushed request bytes
#else
        (void)tls_ok;
#endif

        if (c->is_client)
        {
            ur_try_forward_buffered(c); // forward a framed request iff the client is idle
        }
        else
        {
            // Upstream bytes are the response to the in-flight request. Stray data on
            // an idle pooled upstream (no peer) means it's unusable, so drop it.
            if (!c->peer) { ur_close(c); return; }
            c->peer->ts_up_activity = now_ns(); // upstream made progress

            // Mid-stream: pump the newly-arrived body bytes and return.
            if (c->peer->streaming) { ur_stream_pump(c); return; }

            // First response bytes: peek the head (parse_response_head tolerates
            // chunked, unlike parse_request()); a text/event-stream response enters the
            // streaming pump, everything else the whole-body path below.
            if (_translate == TranslateMode::Anthropic)
            {
                net::http::ResponseHead h;
                const auto hs = net::http::parse_response_head(c->rbuf, h);
                if (hs == net::http::FrameStatus::NeedMore) return; // wait for the full head
                if (hs == net::http::FrameStatus::Error) { ur_error_respond(c->peer, 502); return; }
                // Only a 200 is a real stream; a provider error is relayed with its
                // own status by ur_on_response below (never laundered into a 200).
                if (h.event_stream && h.status == 200)
                {
                    c->peer->ts_up_recvd = now_ns(); // t4: head complete, see the epoll mirror
                    ur_begin_stream(c, h);
                    return;
                }
            }

            // parse_response, NOT parse: providers return non-streaming bodies
            // chunked over HTTP/1.1 and parse_request() rejects that by design (http.hpp).
            const auto r = net::http::parse_response(c->rbuf, c->rdec);
            if (r.failed()) { ur_error_respond(c->peer, 502); return; }
            if (!r.complete()) return; // armed recv delivers the rest
            c->peer->ts_up_recvd = now_ns();
            ur_on_response(c, r.head, r.body, r.total_len);
        }
    }

    void Gateway::ur_try_forward_buffered(Connection* c) noexcept
    {
        // Forward the next framed request only when the client is idle. No request
        // in flight (peer) and no response still draining to it (wbuf).
        if (c->peer != nullptr || !c->wbuf.empty() || c->rbuf.empty()) return;
        net::http::Message m;
        const auto st = net::http::parse_request(c->rbuf, m);
        if (st == net::http::FrameStatus::NeedMore) return; // the armed recv will deliver more
        if (st == net::http::FrameStatus::Error) { ur_error_respond(c, 400); return; }
        c->msg = m;
        c->ts_req_recvd = now_ns();
        ur_forward(c);
    }

    void Gateway::ur_forward(Connection* c) noexcept
    {
        std::string upstream_bytes;
        if (_translate != TranslateMode::None)
        {
            std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
            std::string_view start_line;
            std::string tbody = xlate_req(_translate, body, start_line);
            if (tbody.empty()) { ur_error_respond(c, 400); return; }
            c->wants_usage = provider::openai_wants_stream_usage(body); // see epoll mirror
            const std::string_view client_hdrs(c->rbuf.data(), c->msg.header_len);
            std::string auth_hdrs;
            // Malformed credential => 400, and NOTHING goes upstream.
            // (This called the EPOLL responder until the ep_/ur_ split. Every
            // other error in this function used the uring one. It was harmless,
            // but only by three accidents: `peer` is still null here so the epoll
            // upstream-close branch never ran; a ~100-byte error body always
            // completes inline so the epoll write-arm was never reached; and the
            // close path was already deferred via _doomed. The old naming made the
            // crossing invisible. The prefixes turn it into a grep.)
            if (!auth_headers_for(_translate, client_hdrs, auth_hdrs)) { ur_error_respond(c, 400); return; }
            upstream_bytes = build_http_request(start_line, tbody, _upstream_host_hdr, auth_hdrs);
        }
        else
        {
            upstream_bytes.assign(c->rbuf.data(), c->msg.total_len);
        }

        Connection* u = ur_acquire_upstream();
        if (!u) { ur_error_respond(c, 502); return; }

        u->wbuf = std::move(upstream_bytes);
        u->woff = 0;
        c->rbuf.erase(0, c->msg.total_len);
        c->peer = u;
        u->peer = c;
        c->ever_framed = true;        // past the setup deadline for good
        c->ts_req_built = now_ns();   // end of OUR request-side work
        c->ts_up_activity = c->ts_req_built; // idle-timeout baseline for this request
        if (u->connected) c->ts_wire_ready = c->ts_req_built; // pooled: no handshake

#ifdef LLMBRIDGE_HAVE_TLS
        if (u->connected && u->tls)
        {
            tls_push_wbuf(u); // pooled conns are past the handshake
            ur_tls_flush(u);
            return;
        }
#endif
        if (u->connected) ur_submit_send(u);
        else ur_submit_connect(u); // connect first; on completion we send the request
    }

    void Gateway::ur_on_connect(Connection* u, int res) noexcept
    {
        if (res < 0)
        {
            Connection* cl = u->peer;
            u->peer = nullptr;
            ur_close(u);
            if (cl) { cl->peer = nullptr; ur_error_respond(cl, 502); }
            else ++_stats.errors;
            return;
        }
        u->connected = true;
        // t2 for a PLAINTEXT upstream only; see the epoll twin and tls_feed().
        if (!upstream_is_tls(u) && u->peer && u->peer->ts_wire_ready == 0)
            u->peer->ts_wire_ready = now_ns();
        ur_arm_recv(u); // arm the multishot recv for this upstream's life
#ifdef LLMBRIDGE_HAVE_TLS
        if (u->tls)
        {
            // The request waits in wbuf (plaintext) until the handshake completes;
            // what goes out now is the ClientHello.
            u->tls->start_handshake();
            ur_tls_flush(u);
            return;
        }
#endif
        ur_submit_send(u); // then send the request
    }

    void Gateway::ur_on_response(Connection* u, const net::http::ResponseHead& h,
                                std::string_view body_buf, size_t total_len) noexcept
    {
        Connection* client = u->peer;
        if (_translate != TranslateMode::None)
        {
            const std::string_view body = body_buf;
            // Relay a provider failure with ITS OWN status + message (see the epoll
            // mirror): a 429/529/400 must not be flattened into a generic 502.
            if (h.status != 0 && h.status != 200)
            {
                client->wbuf = build_http_status(
                    h.status, reason_for(h.status),
                    provider::upstream_error_to_openai(body, "upstream_error"));
                client->woff = 0;
                client->peer = nullptr;
                if (h.keep_alive) ur_release_upstream(u); else ur_close(u);
                ++_stats.errors;
                ur_client_send(client);
                return;
            }
            std::string tbody = xlate_resp(_translate, body);
            if (tbody.empty())
            {
                client->peer = nullptr;
                ur_release_upstream(u); // framing was valid; the upstream conn is reusable
                ur_error_respond(client, 502);
                return;
            }
            std::string timing;
            if (_timing_headers)
            {
                const int64_t ts_resp_built = now_ns(); // t5, see the epoll twin
                const TimingSplit sp = timing_split(
                    client->ts_req_recvd, client->ts_req_built, client->ts_wire_ready,
                    client->ts_up_sent, client->ts_up_recvd, ts_resp_built);
                append_timing_headers(timing, client->ts_req_recvd, sp.compute_ns / 1000,
                                      sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                      sp.upstream_ns / 1000, "x-llmbridge-upstream-us");
                append_usage_headers(timing, tbody);
            }
            client->wbuf = build_http("HTTP/1.1 200 OK", tbody, timing);
        }
        else
        {
            // See the epoll mirror: a decoded chunked body is re-framed with
            // Content-Length instead of relayed verbatim.
            if (h.chunked) client->wbuf = build_http("HTTP/1.1 200 OK", body_buf);
            else client->wbuf.assign(u->rbuf.data(), total_len);
        }
        // Pool the upstream only if it will stay open: the response must say
        // keep-alive AND (for passthrough, where the client's Connection header was
        // forwarded verbatim) the client must not have asked to close. Otherwise the
        // upstream is about to close on us. Drop it instead of reusing a corpse.
        const bool pool_upstream =
            h.keep_alive && (_translate != TranslateMode::None || client->msg.keep_alive);
        client->woff = 0;
        client->peer = nullptr;
        // Drop the framed message; anything left is the next pipelined response.
        u->rbuf.erase(0, total_len);
        u->rdec.reset(); // see the epoll mirror
        if (pool_upstream) ur_release_upstream(u);
        else ur_close(u);
        ur_client_send(client);
    }

    void Gateway::ur_on_send(Connection* c, int res) noexcept
    {
        if (res <= 0)
        {
            if (c->is_client) { if (c->peer) ur_abort_pair(c); else ur_close(c); }
            else if (!ur_retry_upstream(c)) { if (c->peer) ur_error_respond(c->peer, 502); else ur_close(c); }
            return;
        }
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            // TLS on either leg: this send moved CIPHERTEXT (tls_out); woff/wbuf
            // track plaintext fed to the Session and are not touched here.
            c->tls_out_off += static_cast<size_t>(res);
            if (c->tls_out_off < c->tls_out.size()) { ur_submit_send(c); return; } // partial
            c->send_inflight = false;
            // The Session may hold more: later handshake flights, or plaintext that
            // could not be pushed before the handshake finished.
            if (c->tls->handshake_done() && c->woff < c->wbuf.size()) tls_push_wbuf(c);
            ur_tls_flush(c); // refill from the write BIO; resubmit if non-empty
            if (c->send_inflight) return; // more ciphertext went out; wait for it

            if (!c->is_client)
            {
                if (c->peer && tls_wbuf_flushed(c)) c->peer->ts_up_sent = now_ns();
                return;
            }
            // Inbound leg: the response is fully encrypted AND fully on the wire,
            // so this is the same completion point the plaintext path reaches when
            // woff catches up with wbuf. Streams continue, everything else finishes.
            if (!tls_wbuf_flushed(c)) return;
            if (c->streaming)
            {
                c->wbuf.clear();
                c->woff = 0;
                ur_stream_flush(c);
            }
            else if (!c->wbuf.empty())
            {
                c->wbuf.clear();
                c->woff = 0;
                ur_finish_client(c);
            }
            return;
        }
#endif
        c->woff += static_cast<size_t>(res);
        if (c->woff < c->wbuf.size()) { ur_submit_send(c); return; } // partial -> send remainder

        if (!c->is_client)
        {
            // Request fully sent. The response arrives via the already-armed multishot
            // recv. KEEP wbuf so we can resend on a stale-connection failure.
            if (c->peer) c->peer->ts_up_sent = now_ns();
        }
        else if (c->streaming)
        {
            // This SSE buffer is fully out. Free the send slot and either send the
            // next pending bytes or finalize if the stream has ended.
            c->wbuf.clear();
            c->woff = 0;
            c->send_inflight = false;
            ur_stream_flush(c);
        }
        else
        {
            c->wbuf.clear();
            c->woff = 0;
            ur_finish_client(c);
        }
    }

    void Gateway::ur_finish_client(Connection* c) noexcept
    {
        // Error replies (close_after_resp) are counted as errors, not in the latency
        // histograms; their timing stamps are unset.
        if (!c->close_after_resp)
        {
            const int64_t ts_resp_sent = now_ns(); // t6, name matches the epoll twin
            if (ts_resp_sent - _t_start >= _warmup_ns)
            {
                // req_path is OUR request-side work only; the wait for connect +
                // TLS is its own histogram so it cannot inflate the added-latency
                // claim. overhead = req_path + resp_path, connect excluded.
                //
                // Derived from the SAME timing_split() the headers use, so
                // connect(TLS) here and x-llmbridge-connect-us there cannot come to
                // mean different things. t5 does not enter this grouping, so t4 is
                // passed in its place.
                const TimingSplit sp = timing_split(c->ts_req_recvd, c->ts_req_built,
                                                    c->ts_wire_ready, c->ts_up_sent,
                                                    c->ts_up_recvd, c->ts_up_recvd);
                const int64_t conn_ns = sp.connect_ns;
                const int64_t req_ns = sp.req_path_ns;
                const int64_t resp_ns = ts_resp_sent - c->ts_up_recvd;
                if (req_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(req_ns));
                if (conn_ns >= 0) _stats.connect.record(static_cast<uint64_t>(conn_ns));
                if (resp_ns >= 0) _stats.resp_path.record(static_cast<uint64_t>(resp_ns));
                if (req_ns >= 0 && resp_ns >= 0)
                    _stats.overhead.record(static_cast<uint64_t>(req_ns + resp_ns));
                ++_stats.requests;
            }
        }
        const bool close_now = c->close_after_resp || !c->msg.keep_alive;
        c->msg = net::http::Message{};
        if (close_now) { ur_close(c); return; }
        // The client's multishot recv is still armed; a pipelined next request may
        // already sit in rbuf. Forward it, else the armed recv delivers more.
        ur_try_forward_buffered(c);
    }

    // ── io_uring streaming pump (Anthropic->OpenAI SSE) ─────────────────────
    // Enter streaming: send the client SSE headers, then translate the body as it
    // arrives. Output accumulates in wpending; ur_stream_flush moves it into wbuf
    // (kept immutable during an in-flight SEND) one send at a time.
    void Gateway::ur_begin_stream(Connection* u, const net::http::ResponseHead& h) noexcept
    {
        Connection* client = u->peer;
        client->streaming = true;
        client->stream_chunked = h.chunked;
        client->stream_keep_alive = h.keep_alive; // decides poolability at stream end
        client->sse = std::make_unique<provider::AnthropicToOpenAiSse>(-1, client->wants_usage);
        if (_timing_headers)
        {
            // t4 = provider's first response byte, stamped by the caller.
            std::string timing;
            // t5 = t4: a stream's response is not built at one instant, so the
            // compute leg is the request side alone instead of an invented figure.
            const TimingSplit sp = timing_split(
                client->ts_req_recvd, client->ts_req_built, client->ts_wire_ready,
                client->ts_up_sent, client->ts_up_recvd, client->ts_up_recvd);
            append_timing_headers(timing, client->ts_req_recvd, sp.compute_ns / 1000,
                                  sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                  sp.upstream_ns / 1000, "x-llmbridge-upstream-ttfb-us");
            client->wpending.assign(sse_head_with_timing(timing));
        }
        else
            client->wpending.assign(kSseHead);
        u->rbuf.erase(0, h.header_len); // consume the head; the rest is body
        ur_stream_pump(u);
    }

    void Gateway::ur_stream_pump(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { ur_close(u); return; }

        const StreamStep st = stream_step(client, u->rbuf, client->wpending, /*at_eof=*/false);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            // Truncate honestly: no fabricated [DONE]. Flush what we have, then the
            // finalize path closes the client (an aborted SSE body).
            client->stream_ended = true;
            client->close_after_resp = true;
            ++_stats.errors;
            ur_stream_flush(client);
            return;
        }
        if (client->wpending.size() + client->wbuf.size() > kStreamBufCap) { ur_abort_pair(client); return; }
        ur_stream_flush(client);
    }

    void Gateway::ur_stream_on_upstream_eof(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { ur_close(u); return; }
        const StreamStep st = stream_step(client, u->rbuf, client->wpending, /*at_eof=*/true);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            client->stream_ended = true;
            client->close_after_resp = true;
            ++_stats.errors;
        }
        ur_stream_flush(client);
    }

    // Serialize sends: only one SEND SQE outstanding (concurrent sends on a fd would
    // interleave). wbuf is (re)filled from wpending ONLY when idle, so its bytes stay
    // put while the kernel reads them for an in-flight SEND: no realloc-under-kernel.
    void Gateway::ur_stream_flush(Connection* client) noexcept
    {
        if (client->send_inflight) return; // a send is already draining wbuf
        if (client->wpending.empty())
        {
            if (client->stream_ended) ur_finalize_stream(client); // nothing left + ended
            return;
        }
        client->wbuf = std::move(client->wpending);
        client->wpending.clear();
        client->woff = 0;
        client->send_inflight = true;
        ur_client_send(client); // (closes the client on SQE exhaustion; nothing more to do)
    }

    void Gateway::ur_finalize_stream(Connection* client) noexcept
    {
        if (Connection* u = client->peer)
        {
            const bool reusable = stream_upstream_reusable(client, u);
            client->peer = nullptr;
            u->peer = nullptr;
            if (reusable) ur_release_upstream(u); // see the epoll mirror
            else ur_close(u);
        }
        // Only a cleanly-terminated stream counts as served (see the epoll mirror).
        if (!client->close_after_resp) ++_stats.requests; // latency histograms N/A
        ur_close(client);
    }

    int Gateway::run_uring()
    {
        _t_start = now_ns();

        unsigned flags = 0;
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_DEFER_TASKRUN)
        flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
#endif
        if (!_ring.init(kRingDepth, flags) && !_ring.init(kRingDepth, 0))
        {
            std::fprintf(stderr, "llmbridge: io_uring init failed; using epoll\n");
            return run_epoll();
        }
        const unsigned bufcount = _uring_buf_count ? _uring_buf_count : kBufCount;
        if (!_bufring.init(_ring, kBufGroup, bufcount, kBufSize))
        {
            std::fprintf(stderr, "llmbridge: io_uring provided-buffer ring unavailable; using epoll\n");
            return run_epoll();
        }
        if (!net::resolve_ipv4(_upstream_ip.c_str(), _upstream_port, _upstream_addr))
            return 1;

        _uring_ts.tv_sec = kPollTickMs / 1000;
        _uring_ts.tv_nsec = static_cast<long long>(kPollTickMs % 1000) * 1000000LL;

        ur_submit_accept();
        ur_submit_timer();

        auto reap = [this] {
            _ring.for_each_cqe([this](const io_uring_cqe* cqe) {
                ur_on_cqe(cqe->user_data, cqe->res, cqe->flags);
            });
        };

        while (!_stop)
        {
            const int r = _ring.submit_and_wait(1);
            if (r < 0 && r != -EINTR && r != -ETIME) break;
            reap();
        }

        // Graceful drain: stop taking new work, force every live fd's in-flight ops
        // to complete, and reap until nothing is outstanding, so no kernel op
        // writes into a buffer we're about to free. Acquired upstreams are reachable
        // only via client->peer, so shut those down too.
        _draining = true;
        auto stop_conn = [this](Connection* c) {
            if (c->fd >= 0) { ::shutdown(c->fd, SHUT_RDWR); ur_submit_cancel(c->fd); }
        };
        for (auto& [id, c] : _clients)
        {
            stop_conn(c);
            if (c->peer) stop_conn(c->peer); // acquired upstreams reachable only via peer
        }
        for (Connection* u : _idle_upstreams) stop_conn(u);
        // doomed conns were already shut down + cancelled by ur_close().

        while (_uring_inflight > 0)
        {
            const int r = _ring.submit_and_wait(1);
            if (r < 0 && r != -EINTR && r != -ETIME) break;
            reap();
        }

        // Free acquired (in-flight) upstreams now drained but tracked only via peer;
        // the rest (_clients, _idle_upstreams, _doomed, listen_conn) are freed by
        // ~Gateway.
        for (auto& [id, c] : _clients)
            if (Connection* u = c->peer) { c->peer = nullptr; if (u->fd >= 0) ::close(u->fd); delete u; }

        return 0;
    }
#endif // LLMBRIDGE_HAVE_URING
} // namespace llmbridge
