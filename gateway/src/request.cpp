// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "request.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <ctime>
#include <string>

#include "net/sigv4.hpp"
#include "provider/json.hpp" // translate_failure re-parses a refused body

namespace llmbridge::detail
{
    namespace
    {
        // Anthropic's required versioning header. Pinned, not passthrough-only:
        // an OpenAI-SDK client has never heard of it, and without it the API
        // rejects the request outright. A client that does send its own value
        // wins (see auth_headers_for); the pin is a default, not an override.
        constexpr std::string_view kAnthropicVersionDefault = "2023-06-01";

        // Is this safe to re-emit as an HTTP header value?
        //
        // Do not trust find_header() to have made this safe. It splits on CRLF, so
        // a value may still contain a bare CR (or LF, NUL, any control byte), and
        // a lenient upstream parser that treats bare CR as a line terminator would
        // then see an injected header. That is not hypothetical: it was measured
        // reaching the upstream as `x-api-key: sk\rX-Smuggled: yes` before this
        // check existed. It matters more than a self-inflicted malformed request
        // because upstream connections are pooled and shared between clients, so
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

        // Every credential header we might care about, collected in one pass.
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

        bool header_stripped(std::string_view line,
                             const std::vector<std::string>& strip) noexcept
        {
            for (const std::string& s : strip)
                if (net::http::detail::line_is(line, s)) return true;
            return false;
        }

        /// Splice `base` in front of the request line's target, in place.
        ///
        /// False means the request line is not origin-form (absolute-form, connect's
        /// authority-form, or simply malformed), and the caller must refuse. We do
        /// not normalize it into one: rewriting a target we do not understand is how
        /// a proxy sends a request somewhere its operator never listed.
        bool prefix_target(std::string& req, std::string_view base)
        {
            if (base.empty()) return true;
            const size_t eol = req.find("\r\n");
            if (eol == std::string::npos) return false;
            const size_t sp1 = req.find(' ');
            if (sp1 == std::string::npos || sp1 > eol) return false;
            const size_t sp2 = req.find(' ', sp1 + 1);
            if (sp2 == std::string::npos || sp2 > eol) return false;
            if (sp2 == sp1 + 1 || req[sp1 + 1] != '/') return false;
            req.insert(sp1 + 1, base);
            return true;
        }

        AuthHeaders scan_auth_headers(std::string_view headers,
                                      const std::vector<std::string>& strip) noexcept
        {
            AuthHeaders out;
            size_t start = 0;
            while (start < headers.size())
            {
                size_t eol = headers.find("\r\n", start);
                if (eol == std::string_view::npos) eol = headers.size();
                const std::string_view line = headers.substr(start, eol - start);
                // A stripped header is invisible here too, or the tenant token in
                // Authorization would still be mapped onto the provider credential.
                if (header_stripped(line, strip)) { start = eol + 2; continue; }
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

        // Build the auth/extra header lines to inject into the translated
        // upstream request, from the client's request headers.
        //
        // Whitelist, not passthrough: the gateway rebuilds the upstream request,
        // and only the credential headers the target dialect understands may
        // cross the translation boundary. Echoing arbitrary client headers
        // through a rebuilt request is a smuggling surface (and our own framing
        // headers must stay authoritative). A byte-forward is untouched by all of
        // this: it already carries every client header.
        //
        // Values re-emitted here cannot contain CR/LF: find_header() bounds each
        // value by its own line's CRLF, so injection via a crafted credential is
        // structurally impossible instead of filtered.
        //
        // Returns false when the client supplied a syntactically invalid credential
        // (control characters). The caller must fail the request instead of
        // forward. Silently dropping would still send a credential-less request
        // upstream, which is a confusing 401; a 400 names the client's own bug.
        bool auth_headers_for(UpstreamDialect mode, std::string_view client_headers,
                              const std::vector<std::string>& strip, std::string& out)
        {
            out.clear();
            const AuthHeaders h = scan_auth_headers(client_headers, strip);

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
                case UpstreamDialect::Anthropic:
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
                case UpstreamDialect::Gemini:
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
                case UpstreamDialect::Cohere:
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
                case UpstreamDialect::Azure:
                {
                    // `api-key`, Azure's own header. A client sending Authorization is
                    // using the OpenAI SDK against an Azure endpoint, which is the
                    // normal case and the reason the bearer is accepted here at all.
                    const std::string_view key = h.x_api_key.empty() ? bearer() : h.x_api_key;
                    if (key.empty()) return false;
                    out.append("api-key: ");
                    out.append(key);
                    out.append("\r\n");
                    break;
                }
                case UpstreamDialect::Bedrock:
                    // Never reached: build_translated_request routes Bedrock to
                    // sign_bedrock, because a signature covers the target and the body
                    // and neither exists yet at this point. Refuse, because falling
                    // through to `return true` would emit no credential at all.
                    return false;
                case UpstreamDialect::OpenAI:
                    break; // byte-forward path; never called, but total anyway
            }
            return true;
        }

        // Translate an OpenAI request body to the upstream dialect, also yielding
        // the upstream start line. Empty return = malformed body. Shared by both
        // event-loop backends.
        std::string xlate_req(UpstreamDialect mode, std::string_view body,
                              std::string_view base_path, std::string_view query,
                              std::string& start_line_store,
                              std::string_view& start_line, std::string_view& target_out,
                              bool* wants_stream_usage = nullptr)
        {
            std::string target_store;
            std::string_view target;
            std::string out;
            switch (mode)
            {
                case UpstreamDialect::Anthropic:
                    target = "/v1/messages";
                    out = provider::openai_to_anthropic_request(body, wants_stream_usage);
                    break;
                case UpstreamDialect::Gemini:
                    target = "/v1beta/models/gemini:generateContent";
                    out = provider::openai_to_gemini_request(body);
                    break;
                case UpstreamDialect::Cohere:
                    target = "/v2/chat";
                    out = provider::openai_to_cohere_request(body);
                    break;
                case UpstreamDialect::Bedrock:
                {
                    // The model moves from the body into the path, so the target is
                    // per-request and cannot be a literal. Encoded as one segment,
                    // slashes included: an inference-profile id may contain them, and
                    // a raw slash there would silently change which resource is named.
                    std::string model;
                    out = provider::openai_to_bedrock_request(body, model);
                    if (out.empty() || model.empty()) return {};
                    target_store.assign("/model/")
                        .append(net::sigv4::uri_encode(model, true))
                        .append("/invoke");
                    target = target_store;
                    break;
                }
                case UpstreamDialect::Azure:
                    // Nothing is translated: Azure serves the OpenAI dialect. The body
                    // is forwarded as the client wrote it, so an option we do not
                    // model is not silently dropped on the way through.
                    out.assign(body);
                    if (out.empty()) return {};
                    target = "/chat/completions";
                    break;
                case UpstreamDialect::OpenAI:
                    return {};
            }
            // No base path is the common case and stays allocation-free: the view
            // points at a literal with static storage. Bedrock never takes that path,
            // because its target is built per request.
            if (base_path.empty() && query.empty() && mode != UpstreamDialect::Bedrock &&
                mode != UpstreamDialect::Azure)
            {
                switch (mode)
                {
                    case UpstreamDialect::Anthropic: start_line = "POST /v1/messages HTTP/1.1"; break;
                    case UpstreamDialect::Gemini:
                        start_line = "POST /v1beta/models/gemini:generateContent HTTP/1.1";
                        break;
                    case UpstreamDialect::Cohere: start_line = "POST /v2/chat HTTP/1.1"; break;
                    case UpstreamDialect::Bedrock:
                    case UpstreamDialect::Azure:
                    case UpstreamDialect::OpenAI: return {};
                }
                target_out = start_line.substr(5, start_line.size() - 14);
            }
            else
            {
                start_line_store.assign("POST ").append(base_path).append(target);
                // The venue's query, not the client's: a translating mode builds the
                // whole target, so there is nothing to merge. Azure's api-version
                // rides here; every other mode has an empty query.
                if (!query.empty()) start_line_store.append("?").append(query);
                start_line_store.append(" HTTP/1.1");
                start_line = start_line_store;
                // A view into the caller's store, so it outlives this frame: the
                // signature is computed over exactly these bytes.
                target_out = std::string_view(start_line_store).substr(
                    5, start_line_store.size() - 14);  // between "POST " and " HTTP/1.1"
            }
            return out;
        }

        /// Sign a rebuilt Bedrock request, or refuse it.
        ///
        /// Shared by both backends on purpose. This is the credential path, and the
        /// three calls it replaces used to be repeated in the epoll and io_uring
        /// dispatchers: a fix that lands on one of those misses the shipped one,
        /// because io_uring is the default on Ubuntu 24.04.
        ///
        /// Signing must happen here and not at the client: the target and the body are
        /// both rewritten above, so any signature a caller pre-computed is void by the
        /// time these bytes exist.
        bool sign_bedrock(const Upstream& up, std::string_view client_hdrs,
                          const std::vector<std::string>& strip, std::string_view target,
                          std::string_view body, std::string& out)
        {
    #ifdef LLMBRIDGE_HAVE_TLS
            if (up.aws_region.empty()) return false;
            const AuthHeaders h = scan_auth_headers(client_hdrs, strip);
            if (!header_value_safe(h.authorization)) return false;
            std::string_view bearer = h.authorization;
            if (bearer.size() <= 7 ||
                (bearer.compare(0, 7, "Bearer ") != 0 && bearer.compare(0, 7, "bearer ") != 0))
                return false;
            bearer = net::http::detail::ltrim(bearer.substr(7));

            net::sigv4::Credentials cred{};
            if (!net::sigv4::parse_credentials(bearer, cred)) return false;

            char stamp[17];
            const std::time_t now = std::time(nullptr);
            std::tm utc{};
            if (::gmtime_r(&now, &utc) == nullptr) return false;
            if (std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &utc) != 16) return false;

            net::sigv4::Request r{};
            r.method = "POST";
            r.path = target;
            r.host = up.host_hdr;
            r.content_type = "application/json";
            r.body = body;
            r.region = up.aws_region;
            // The signing service name is `bedrock` for the runtime endpoint too; it
            // does not follow the hostname, which is one of the ways this returns 403.
            r.service = "bedrock";
            r.amz_date = std::string_view(stamp, 16);

            const auto headers = net::sigv4::sign(cred, r);
            if (headers.empty()) return false;
            out.clear();
            for (const auto& hdr : headers)
                out.append(hdr.name).append(": ").append(hdr.value).append("\r\n");
            return true;
    #else
            (void)up; (void)client_hdrs; (void)strip; (void)target; (void)body; (void)out;
            return false;   // no OpenSSL, no signature, and never an unsigned request
    #endif
        }

    } // namespace

    /// The translation for a request, from the client's dialect (read off the request
    /// line) and the venue's. Shared by both backends; the caller handles the refusal
    /// so each can use its own error path. A free function, not a Gateway method,
    /// because TranslationPlan lives in dialect.hpp which already includes gateway.hpp.
    TranslationPlan resolve_dialect(const Connection* c, const Upstream& venue) noexcept
    {
        const std::string_view head(c->rbuf.data(), c->msg.header_len);
        const std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
        // Whether the client asked to stream is consulted for one venue only,
        // because Bedrock cannot stream from this endpoint, and answering it means walking
        // the body to its top-level `stream` key.
        const bool stream_matters = venue.dialect == UpstreamDialect::Bedrock;
        return resolve_translation(client_dialect_from_target(request_line(head)),
                                   venue.dialect,
                                   stream_matters && provider::wants_stream(body));
    }

    /// Why a request translation failed, for the log only. The translator returns
    /// an empty string on any failure and keeps no reason, so this re-parses the
    /// body to separate the two cases a caller can actually act on: bytes that
    /// are not JSON at all, against JSON whose shape we cannot translate. Costs a
    /// parse, but only on a path that has already failed and is about to answer
    /// 400, so it is never on the hot path.
    ///
    /// It exists because "request translate" told an operator nothing: a client
    /// sending Python's `True` instead of `true` and a client sending a model we
    /// do not support produced the identical line.
    const char* translate_failure(std::string_view body) noexcept
    {
        bool ok = false;
        const provider::json::Value v = provider::json::parse(body, ok);
        if (!ok) return refuse::kNotJson;
        if (!v.is_object()) return refuse::kNotObject;
        if (!v.find("model")) return refuse::kNoModel;
        const provider::json::Value* msgs = v.find("messages");
        if (!msgs) return refuse::kNoMessages;

        // Name the part we could not carry. A caller who sends an image gets
        // "vision is not implemented" instead of "unsupported request shape",
        // which is the difference between reading the README and filing a bug.
        if (msgs->is_array())
            for (const provider::json::Value& m : msgs->arr)
            {
                const provider::json::Value* c = m.find("content");
                if (!c || !c->is_array()) continue;
                for (const provider::json::Value& part : c->arr)
                {
                    const std::string_view t = part.str_or("type");
                    if (t == "text") continue;
                    if (t == "image_url" || t == "image")
                        return refuse::kImage;
                    if (t == "input_audio" || t == "audio")
                        return refuse::kAudio;
                    if (t == "file" || t == "document")
                        return refuse::kFile;
                    return refuse::kPart;
                }
            }

        // Tool arguments are a JSON string that must decode to one object, and
        // nothing else: see the splice in translate_body.cpp.
        if (msgs->is_array())
            for (const provider::json::Value& m : msgs->arr)
            {
                const provider::json::Value* tcs = m.find("tool_calls");
                if (!tcs || !tcs->is_array()) continue;
                for (const provider::json::Value& call : tcs->arr)
                {
                    const provider::json::Value* fn = call.find("function");
                    if (!fn) continue;
                    const std::string args =
                        provider::json::unescape_string(fn->str_or("arguments"));
                    if (args.empty()) continue;
                    bool aok = false;
                    const provider::json::Value parsed = provider::json::parse(args, aok);
                    if (!aok || !parsed.is_object() || parsed.sv.size() != args.size())
                        return refuse::kToolArgs;
                }
            }
        return refuse::kShape;
    }

    const char* dialect_name(UpstreamDialect m) noexcept
    {
        switch (m)
        {
            case UpstreamDialect::OpenAI: return "openai";
            case UpstreamDialect::Anthropic: return "anthropic";
            case UpstreamDialect::Gemini: return "gemini";
            case UpstreamDialect::Cohere: return "cohere";
            case UpstreamDialect::Bedrock: return "bedrock";
            case UpstreamDialect::Azure: return "azure";
        }
        return "?";
    }

    // Build a minimal HTTP/1.1 message (start line + JSON body) for a
    // translated request/response. The benchmark backend ignores path and
    // most headers; a real Anthropic target would add x-api-key /
    // anthropic-version here (same cost class).
    // `extra` is zero or more complete "Name: value\r\n" lines, inserted before
    // the terminating CRLF (used for the opt-in timing headers).
    std::string build_http(std::string_view start_line, std::string_view body,
                           std::string_view extra)
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

    // Upstream request builder: build_http plus a Host header (HTTP/1.1
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

    /// Whether a request head carries `Expect: 100-continue`. RFC 9110 section
    /// 10.1.1: a server that receives it must send an immediate 100 or a final
    /// status before reading the body.
    [[nodiscard]] bool expects_continue(std::string_view head) noexcept
    {
        size_t pos = head.find("\r\n");
        while (pos != std::string_view::npos && pos + 2 < head.size())
        {
            const size_t eol = head.find("\r\n", pos + 2);
            const std::string_view line =
                head.substr(pos + 2, eol == std::string_view::npos ? std::string_view::npos
                                                                    : eol - pos - 2);
            if (net::http::detail::line_is(line, "expect:"))
            {
                std::string_view v = net::http::detail::ltrim(line.substr(7));
                return v.size() >= 12 && net::http::detail::line_is(v, "100-continue");
            }
            pos = eol;
        }
        return false;
    }

    /// Copy `msg` (one framed request) without the stripped header lines, and
    /// with `Host` replaced by the venue's own. The request line survives because
    /// every strip name ends in ':', which a request line never matches;
    /// StrippingIsExactNotAPrefix locks that in.
    ///
    /// Rewriting Host is what makes byte-forward a correct reverse proxy: this
    /// path sends the request to a different origin than the client addressed, and
    /// HTTP/1.1 wants Host to name the origin being addressed. The translating
    /// path has always emitted the venue's Host (build_http_request); byte-forward
    /// passed the client's through, so a provider behind a CDN or serving several
    /// vhosts saw a name that was never its own.
    /// Empty return = refuse this request, never forward it. Today that is a
    /// request line prefix_target could not read.
    /// Rebuild a byte-forwarded request: strip what the venue must not see, name
    /// the venue in Host, prefix the base path, and state the body length
    /// Ourselves.
    ///
    /// The client's `Content-Length` is dropped and re-emitted from the bytes we
    /// actually forward. Today those are the same number, so this changes nothing
    /// observable; it is a precondition, not a fix. The moment anything edits a
    /// forwarded body, a copied length becomes a lie.
    ///
    /// Unambiguous because parse_request refuses Transfer-Encoding outright, so
    /// every request here is either Content-Length framed or has no body at all.
    std::string request_without(std::string_view msg, size_t header_len,
                                const std::vector<std::string>& strip,
                                std::string_view host_hdr, std::string_view base_path,
                                std::string_view body_override)
    {
        // Copy in runs, not per line: a flush happens only where a stripped line
        // interrupts the kept ones, so one memcpy when nothing matches and two
        // when one header does. Measured against a plain copy, min of 7
        // interleaved rounds: +86/89/99 ns at 1/4/16 KB bodies, so the cost is
        // the header scan and is flat in body size. Per-line appends cost ~45 ns
        // more again.
        // The HEAD is built on its own, and the body is appended once at the end.
        // Host and Content-Length are inserted just after the request line.
        std::string head;
        head.reserve(header_len + 128);
        std::string& out = head; // the loop below builds the head, unchanged
        bool saw_cl = false;
        size_t start = 0, run = 0;
        while (start < header_len)
        {
            const size_t eol = msg.find("\r\n", start);
            if (eol == std::string_view::npos || eol >= header_len) break;
            const std::string_view line = msg.substr(start, eol - start);
            if (line.empty()) break; // the blank line closes the block
            // Host is dropped here and re-emitted below, so exactly one survives
            // however many the client sent.
            const bool is_host =
                !host_hdr.empty() && line.size() >= 5 &&
                (line[0] == 'H' || line[0] == 'h') && (line[1] == 'o' || line[1] == 'O') &&
                (line[2] == 's' || line[2] == 'S') && (line[3] == 't' || line[3] == 'T') &&
                line[4] == ':';
            const bool is_cl = net::http::detail::line_is(line, "content-length:");
            if (is_cl) saw_cl = true;
            // Expect: 100-continue asks the upstream for an interim response we
            // have no way to relay: parse_response refuses a 1xx, so forwarding
            // the header turns a legal client request into a 502. Drop it and
            // send the body, which is what we do anyway.
            const bool is_expect = net::http::detail::line_is(line, "expect:");
            // Accept-Encoding is dropped so the provider answers in plain text,
            // because a compressed body is a body this gateway cannot measure.
            // Every real client asks for gzip; curl does not, which is how this
            // survived a live test. With it, an Anthropic stream recorded
            // tokens_in=-1 while the identical request without it recorded 8.
            //
            // The cost is bandwidth on the provider leg, and it is the right
            // trade for a gateway whose product is the measurement. Clients
            // always accept identity, so nothing downstream breaks.
            const bool is_accept_encoding =
                net::http::detail::line_is(line, "accept-encoding:");
            if (is_host || is_cl || is_expect || is_accept_encoding ||
                header_stripped(line, strip))
            {
                out.append(msg.substr(run, start - run));
                run = eol + 2;
            }
            start = eol + 2;
        }
        // Headers and blank line, then the body: separately, so a replacement body
        // can take the original's place and the length below describes what is
        // actually sent.
        out.append(msg.substr(run, header_len - run));
        const std::string_view body =
            body_override.empty() ? msg.substr(header_len) : body_override;
        // Ours, from the body we are actually sending. Emitted when the client
        // framed a body or there is one to frame; a bodyless request that carried
        // no length keeps carrying none.
        const size_t body_len = body.size();
        if (saw_cl || body_len)
        {
            const size_t after_start_line = out.find("\r\n");
            if (after_start_line == std::string::npos) return {};
            out.insert(after_start_line + 2,
                       "Content-Length: " + std::to_string(body_len) + "\r\n");
        }
        if (!host_hdr.empty())
        {
            // After the request line, which is where a Host belongs and where a
            // reader looks for it. The request line always ends at the first CRLF.
            const size_t after_start_line = out.find("\r\n");
            if (after_start_line != std::string::npos)
                out.insert(after_start_line + 2,
                           "Host: " + std::string(host_hdr) + "\r\n");
        }
        if (!prefix_target(out, base_path)) return {};
        // The head is final: the body joins it once, and nothing shifts it after.
        std::string full;
        full.reserve(head.size() + body.size());
        full.append(head);
        full.append(body);
        return full;
    }

    /// Everything between a client request and the bytes for a translated venue.
    ///
    /// One function because both event loops need identical behaviour on the
    /// credential path, and because the ordering matters: the body and target are
    /// built first, then signed, because a Bedrock signature covers both.
    bool build_translated_request(const Upstream& up, UpstreamDialect mode,
                                  std::string_view body, std::string_view client_hdrs,
                                  const std::vector<std::string>& strip,
                                  std::string& out, const char*& why,
                                  std::string_view model_override,
                                  bool* wants_stream_usage)
    {
        // `mode` is the resolved per-request translation, not up.dialect: they are
        // equal whenever this is called (a byte-forward skips this function), but the
        // caller passes it so the request path never re-derives it from the venue.
        // Before translating, so one rewrite serves every dialect: the Anthropic
        // and Bedrock translators both read the model out of the body, and Bedrock
        // then puts it in the request path.
        std::string rewritten;
        if (!model_override.empty())
        {
            rewritten = provider::rewrite_model(body, model_override);
            if (rewritten.empty()) { why = "translate"; return false; }
            body = rewritten;
        }
        std::string start_line_store;
        std::string_view start_line, target;
        const std::string tbody =
            xlate_req(mode, body, up.base_path, up.query, start_line_store,
                      start_line, target, wants_stream_usage);
        if (tbody.empty()) { why = "translate"; return false; }

        std::string auth_hdrs;
        const bool ok = mode == UpstreamDialect::Bedrock
                            ? sign_bedrock(up, client_hdrs, strip, target, tbody, auth_hdrs)
                            : auth_headers_for(mode, client_hdrs, strip, auth_hdrs);
        if (!ok) { why = "credential"; return false; }

        out = build_http_request(start_line, tbody, up.host_hdr, auth_hdrs);
        return true;
    }

    // Translate an upstream response body back to the OpenAI shape. Empty = bad.
    std::string xlate_resp(UpstreamDialect mode, std::string_view body)
    {
        switch (mode)
        {
            // Bedrock answers with Anthropic's Messages envelope, so the response
            // leg is the same translator; only the request leg differs.
            case UpstreamDialect::Bedrock:
            case UpstreamDialect::Anthropic: return provider::anthropic_to_openai_response(body);
            // Azure answers in the OpenAI shape it was asked in.
            case UpstreamDialect::Azure: return std::string(body);
            case UpstreamDialect::Gemini: return provider::gemini_to_openai_response(body);
            case UpstreamDialect::Cohere: return provider::cohere_to_openai_response(body);
            case UpstreamDialect::OpenAI: return {};
        }
        return {};
    }

    /// `Host:` for a rebuilt request. The parsed hostname when there is one, since
    /// providers route and verify on it; ip:port otherwise. Default ports omitted.
    std::string host_header_for(const Upstream& u)
    {
        if (u.sni_host.empty()) return u.ip + ":" + std::to_string(u.port);
        const bool default_port = (u.tls && u.port == 443) || (!u.tls && u.port == 80);
        return default_port ? u.sni_host : u.sni_host + ":" + std::to_string(u.port);
    }

    /// The AWS region inside an endpoint name, or empty.
    ///
    /// `bedrock-runtime.us-east-1.amazonaws.com` -> `us-east-1`. Derived and not
    /// configured because the endpoint already states it and two sources of one
    /// fact drift; empty is a refusal, never a default, since signing with the
    /// wrong region returns a 403 whose body explains nothing.
    std::string aws_region_for(const Upstream& u)
    {
        const std::string& h = u.sni_host;
        const size_t first = h.find('.');
        if (first == std::string::npos) return {};
        const size_t second = h.find('.', first + 1);
        if (second == std::string::npos) return {};
        const std::string_view region(h.data() + first + 1, second - first - 1);
        // Shape check, not a list: regions are added faster than any list is
        // updated, and "letters, digits and hyphens with a digit in it" separates
        // `us-east-1` from `amazonaws` without pinning us to a snapshot of AWS.
        if (region.size() < 5 || region.find_first_of("0123456789") == std::string_view::npos)
            return {};
        for (const char c : region)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                return {};
        return std::string(region);
    }

    /// The client's address, for a refusal we are about to log. Empty when the
    /// socket cannot answer, which is normal on one already torn down.
    std::string peer_of(int fd) noexcept
    {
        sockaddr_in a{};
        socklen_t len = sizeof a;
        if (fd < 0 || ::getpeername(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return {};
        if (a.sin_family != AF_INET) return {};
        char buf[INET_ADDRSTRLEN];
        if (!::inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf)) return {};
        return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port));
    }

} // namespace llmbridge::detail
