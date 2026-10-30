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

#include <algorithm>
#include <cstring>
#include <ctime>
#include <charconv>
#include <string>

#include "net/http_write.hpp"
#include "net/sigv4.hpp"
#include "provider/dialect.hpp"
#include "provider/json.hpp"

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

        /// Whether the operator's strip list names this header. Entries are lower-case,
        /// with or without their colon.
        bool stripped(std::string_view name, const std::vector<std::string>& strip) noexcept
        {
            for (std::string_view s : strip)
            {
                if (!s.empty() && s.back() == ':') s.remove_suffix(1);
                if (net::http::detail::same_ci(name, s)) return true;
            }
            return false;
        }

        /// "Bearer K" -> K, the scheme in any case (RFC 9110 11.1); empty for any other.
        std::string_view bearer_token(std::string_view v) noexcept
        {
            if (v.size() <= 7 || !net::http::detail::same_ci(v.substr(0, 6), "bearer") ||
                (v[6] != ' ' && v[6] != '\t'))
                return {};
            return net::http::detail::ltrim(v.substr(7));
        }

        /// Whether a client's origin-form path is safe to forward under a venue's base
        /// path: no dot segment and no backslash, in plain or percent-encoded form, since
        /// the upstream resolves them and `/../` would leave the configured prefix.
        bool target_path_ok(std::string_view target)
        {
            const std::string_view path = target.substr(0, target.find('?'));
            for (size_t i = 0; i < path.size(); ++i)
            {
                const char c = path[i];
                if (c == '\\') return false;
                if (c == '%' && i + 2 < path.size())
                {
                    const char a = path[i + 1], b = static_cast<char>(path[i + 2] | 0x20);
                    if ((a == '2' && (b == 'e' || b == 'f')) || (a == '5' && b == 'c')) return false;
                }
                if (c == '/' && i + 1 < path.size() && path[i + 1] == '.')
                {
                    const size_t end = path.find('/', i + 1);
                    const std::string_view seg = path.substr(i + 1, end == std::string_view::npos
                                                                         ? std::string_view::npos
                                                                         : end - i - 1);
                    if (seg == "." || seg == "..") return false;
                }
            }
            return true;
        }

        /// The line a venue takes a named key on, up to the key; none for one that signs.
        /// A Google venue set to `bearer` takes an OAuth token instead of an API key.
        std::string_view key_line_for(UpstreamDialect venue, bool bearer) noexcept
        {
            using provider::Auth;
            if (bearer && provider::spec(venue).auth == Auth::GoogApiKey) return "Authorization: Bearer ";
            switch (provider::spec(venue).auth)
            {
                case Auth::Forward:
                case Auth::Bearer: return "Authorization: Bearer ";
                case Auth::XApiKey: return "x-api-key: ";
                case Auth::GoogApiKey: return "x-goog-api-key: ";
                case Auth::ApiKey: return "api-key: ";
                case Auth::SigV4: break;
            }
            return {};
        }

        /// The client's credential lines in the venue's terms, appended to `out`. Every
        /// credential the client sent is checked first, whether or not this venue reads
        /// it, so a malformed value cannot route around the check by picking another.
        const char* append_auth(UpstreamDialect mode, std::string_view head, const HeadFacts& f,
                                const NamedKey& key, std::string& out, std::string_view beta = {},
                                bool bearer = false)
        {
            const std::string_view auth = f.authorization.in(head), x_api_key = f.x_api_key.in(head),
                                   api_key = f.api_key.in(head), goog = f.x_goog_api_key.in(head),
                                   version = f.anthropic_version.in(head);
            for (const std::string_view v : {auth, x_api_key, api_key, goog, version})
                if (!v.empty() && !header_value_safe(v)) return "credential";
            const auto line = [&out](std::string_view name, std::string_view v) {
                if (v.empty()) return;
                out.append(name).append(": ").append(v).append("\r\n");
            };
            using provider::Auth;
            if (!key.empty())
            {
                out.append(key.line).append(key.value).append("\r\n");
                if (provider::spec(mode).auth == Auth::XApiKey)
                {
                    line("anthropic-version", version.empty() ? kAnthropicVersionDefault : version);
                    line("anthropic-beta", beta);
                }
                return nullptr;
            }
            switch (provider::spec(mode).auth)
            {
                case Auth::XApiKey:
                    line("x-api-key", x_api_key.empty() ? bearer_token(auth) : x_api_key);
                    line("anthropic-version", version.empty() ? kAnthropicVersionDefault : version);
                    line("anthropic-beta", beta);
                    return nullptr;
                case Auth::GoogApiKey:
                    // Exactly one credential: Google refuses a request carrying two.
                    if (bearer)
                    {
                        if (bearer_token(auth).empty()) return "no credential";
                        line("Authorization", auth);
                    }
                    else line("x-goog-api-key", goog.empty() ? bearer_token(auth) : goog);
                    return nullptr;
                case Auth::Bearer:
                    line("Authorization", auth); // Cohere speaks Bearer natively
                    return nullptr;
                case Auth::ApiKey:
                {
                    // Azure's own `api-key` first; the OpenAI SDK against an Azure
                    // endpoint sends Authorization, the normal case.
                    std::string_view key = !api_key.empty() ? api_key : x_api_key;
                    if (key.empty()) key = bearer_token(auth);
                    if (key.empty()) return "no credential";
                    line("api-key", key);
                    return nullptr;
                }
                case Auth::SigV4:   // signed instead: sign_bedrock
                case Auth::Forward: // byte-forwarded, never rebuilt here
                    break;
            }
            return "credential";
        }

        /// Sign a rebuilt Bedrock request into `out`, or refuse it. Here and not at the
        /// client: the target and body are both rewritten, so any earlier signature is void.
        bool sign_bedrock(const Upstream& up, std::string_view authorization,
                          std::string_view target, std::string_view body, std::string& out,
                          const char*& why)
        {
            why = "credential";
    #ifdef LLMBRIDGE_HAVE_TLS
            if (up.aws_region.empty()) return false;
            if (!header_value_safe(authorization)) return false;
            why = "bedrock credential";
            const std::string_view bearer = bearer_token(authorization);
            if (bearer.empty()) return false;

            net::sigv4::Credentials cred{};
            if (!net::sigv4::parse_credentials(bearer, cred)) return false;
            why = "credential";

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
            for (const auto& hdr : headers)
                out.append(hdr.name).append(": ").append(hdr.value).append("\r\n");
            return true;
    #else
            (void)up; (void)authorization; (void)target; (void)body; (void)out; (void)why;
            return false;   // no OpenSSL, no signature, and never an unsigned request
    #endif
        }

    } // namespace

    // The media type alone: parameters and case do not change what it is.
    bool json_media_type(std::string_view content_type) noexcept
    {
        const std::string_view t = net::http::detail::trim_ows(content_type.substr(0, content_type.find(';')));
        return net::http::detail::same_ci(t, "application/json") ||
               (t.size() > 5 && net::http::detail::same_ci(t.substr(t.size() - 5), "+json"));
    }

    bool HeadReader::operator()(const net::http::HeaderLine& h) noexcept
    {
        using net::http::Field;
        const auto span = [this](std::string_view v) {
            return HeadFacts::Span{static_cast<uint32_t>(v.data() - head.data()),
                                   static_cast<uint32_t>(v.size())};
        };
        // First occurrence wins (anti-smuggling: a repeated credential must resolve the
        // same way for us and the upstream). A stripped one is invisible, or a tenant's
        // Authorization would still be mapped onto the provider credential.
        const auto cred = [&](HeadFacts::Span& s) {
            if (s.empty() && !stripped(h.name, strip))
                s = span(rtrim_ows(net::http::detail::ltrim(h.value)));
        };
        switch (h.field)
        {
            case Field::Authorization: cred(f.authorization); break;
            case Field::XApiKey: cred(f.x_api_key); break;
            case Field::ApiKey: cred(f.api_key); break;
            case Field::XGoogApiKey: cred(f.x_goog_api_key); break;
            case Field::AnthropicVersion: cred(f.anthropic_version); break;
            case Field::AnthropicBeta:
                if (stripped(h.name, strip) || f.anthropic_betas == 2) break;
                if (f.anthropic_betas++ == 0) f.anthropic_beta = span(rtrim_ows(net::http::detail::ltrim(h.value)));
                break;
            case Field::ContentLength: f.content_length = true; break;
            case Field::ContentType:
                if (f.content_type.empty()) f.content_type = span(h.value);
                break;
            case Field::Expect:
                // RFC 9110 10.1.1: answer a 100 or a final status before reading the body.
                if (f.expect == 0)
                {
                    const std::string_view v = net::http::detail::ltrim(h.value);
                    f.expect = v.size() >= 12 && net::http::detail::line_is(v, "100-continue") ? 2 : 1;
                }
                break;
            case Field::Connection:
                if (f.connections < HeadFacts::kConnections) f.connection[f.connections] = span(h.value);
                if (f.connections <= HeadFacts::kConnections) ++f.connections;
                break;
            case Field::Other:
                // Every line a policy could name, stripped or not: no name is known yet.
                if (!key_header_ok(h.name)) break;
                if (f.key_headers < HeadFacts::kKeyHeaders)
                {
                    f.key_name[f.key_headers] = span(h.name);
                    f.key_value[f.key_headers] = span(rtrim_ows(net::http::detail::ltrim(h.value)));
                }
                if (f.key_headers <= HeadFacts::kKeyHeaders) ++f.key_headers;
                break;
            default: break;
        }
        return true;
    }

    Refusal named_key(std::string_view head, const HeadFacts& f, std::string_view name,
                      UpstreamDialect venue, NamedKey& out, bool bearer) noexcept
    {
        out = {};
        // A signature covers a key pair; a bearer key has no header there to go in.
        const std::string_view line = key_line_for(venue, bearer);
        if (line.empty()) return {kWhyKeyUnusable, refuse::kKeyHeaderUnusable};
        // Every line that could match is in the table, so a repeat is never missed.
        if (f.key_headers > HeadFacts::kKeyHeaders) return {kWhyKeyTooMany, refuse::kKeyHeaderTooMany};
        std::string_view value;
        int seen = 0;
        for (uint8_t i = 0; i < f.key_headers; ++i)
            if (net::http::detail::same_ci(f.key_name[i].in(head), name) && seen++ == 0)
                value = f.key_value[i].in(head);
        if (seen > 1) return {kWhyKeyRepeated, refuse::kKeyHeaderRepeated};
        if (value.empty()) return {kWhyKeyMissing, refuse::kKeyHeaderMissing};
        if (!header_value_safe(value)) return {"malformed credential", refuse::kCredential};
        out = {line, value};
        return {};
    }
    // Spliced into a JSON string unescaped, as a policy's message is.
    static_assert(deny_message_ok(refuse::kKeyHeaderMissing) && deny_message_ok(refuse::kKeyHeaderRepeated) &&
                  deny_message_ok(refuse::kKeyHeaderUnusable) && deny_message_ok(refuse::kKeyHeaderTooMany));
    static_assert(HeadFacts::kKeyHeaders == 8, "refuse::kKeyHeaderTooMany states the bound");

    // A test seam over one header block: the facts a framing walk would read.
    bool auth_headers_for(UpstreamDialect mode, std::string_view client_headers,
                          const std::vector<std::string>& strip, std::string& out,
                          std::string_view key_header, Refusal* refused)
    {
        const auto fail = [&](Refusal r) {
            if (refused) *refused = r;
            out.clear();
            return false;
        };
        out.clear();
        while (client_headers.size() >= 2 && client_headers.substr(client_headers.size() - 2) == "\r\n")
            client_headers.remove_suffix(2);
        const std::string head = "POST / HTTP/1.1\r\n" + std::string(client_headers);
        HeadFacts f;
        if (!net::http::walk_headers(head, HeadReader{head, strip, f})) return fail({"request framing", nullptr});
        NamedKey key;
        if (!key_header.empty())
            if (const Refusal r = named_key(head, f, key_header, mode, key)) return fail(r);
        if (const char* bad = append_auth(mode, head, f, key, out)) return fail({bad, nullptr});
        return true;
    }

    Dialect client_dialect_of(std::string_view head) noexcept
    {
        const std::string_view rl = head.substr(0, head.find("\r\n"));
        const size_t sp1 = rl.find(' '), sp2 = rl.rfind(' ');
        if (sp1 == std::string_view::npos || sp2 <= sp1) return Dialect::OpenAI;
        return client_wire(rl.substr(0, sp1), rl.substr(sp1 + 1, sp2 - sp1 - 1));
    }

    bool responses_api(std::string_view head) noexcept
    {
        const std::string_view rl = head.substr(0, head.find("\r\n"));
        const size_t sp1 = rl.find(' '), sp2 = rl.rfind(' ');
        if (sp1 == std::string_view::npos || sp2 <= sp1 || rl.substr(0, sp1) != "POST") return false;
        const std::string_view target = rl.substr(sp1 + 1, sp2 - sp1 - 1);
        return target.substr(0, target.find('?')).ends_with("/v1/responses");
    }

    /// The translation for a request, from the client's dialect (read off the request
    /// line) and the venue's. Shared by both backends; the caller handles the refusal
    /// so each can use its own error path. A free function, not a Gateway method,
    /// because TranslationPlan lives in dialect.hpp which already includes gateway.hpp.
    provider::TopLevelFacts body_facts(std::string_view head, std::string_view body) noexcept
    {
        const Dialect client = client_dialect_of(head);
        if (client == Dialect::OpenAI) return provider::top_level_facts(body);
        provider::TopLevelFacts f;
        // Gemini names the model, and whether it streams, in the path: the body has neither.
        if (client == Dialect::Gemini)
        {
            const std::string_view target = target_of(head);
            const std::string_view model = provider::gemini_path_model(target).model;
            if (provider::path_model_ok(model)) f.model = model;
            f.stream = target.substr(0, target.find('?')).ends_with(":streamGenerateContent");
            return f;
        }
        f.model = provider::model_of(body);
        return f;
    }

    std::string_view target_of(std::string_view head) noexcept
    {
        const std::string_view rl = head.substr(0, head.find("\r\n"));
        const size_t sp1 = rl.find(' '), sp2 = rl.rfind(' ');
        if (sp1 == std::string_view::npos || sp2 <= sp1) return {};
        return rl.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    namespace
    {
        // Whether a query names parameter `name`, with or without a value.
        bool query_has(std::string_view query, std::string_view name) noexcept
        {
            while (!query.empty())
            {
                const size_t amp = query.find('&');
                const std::string_view pair = query.substr(0, amp);
                if (pair.substr(0, pair.find('=')) == name) return true;
                if (amp == std::string_view::npos) break;
                query.remove_prefix(amp + 1);
            }
            return false;
        }
        bool query_is(std::string_view query, std::string_view pair) noexcept
        {
            while (!query.empty())
            {
                const size_t amp = query.find('&');
                if (query.substr(0, amp) == pair) return true;
                if (amp == std::string_view::npos) break;
                query.remove_prefix(amp + 1);
            }
            return false;
        }
    } // namespace

    Refusal target_refusal(std::string_view head) noexcept
    {
        const std::string_view target = target_of(head);
        const size_t q = target.find('?');
        const std::string_view path = target.substr(0, q);
        const std::string_view query = q == std::string_view::npos ? std::string_view{} : target.substr(q + 1);
        // A key in the URL rides every log, proxy and history that keeps one: never sent on.
        if (query_has(query, "key")) return {"key in the URL", refuse::kKeyInUrl};
        for (const std::string_view v : {"/v1beta/interactions", "/v1alpha/interactions", "/v1/interactions"})
            if (path.starts_with(v) && (path.size() == v.size() || path[v.size()] == '/' || path[v.size()] == ':'))
                return {"Gemini Interactions API", refuse::kGeminiInteractions};
        if (client_dialect_of(head) != Dialect::Gemini) return {};
        // Without alt=sse Google streams one JSON array, which would be held to its end.
        if (path.ends_with(":streamGenerateContent") && !query_is(query, "alt=sse"))
            return {"Gemini stream without alt=sse", refuse::kGeminiNoSse};
        const provider::PathModel pm = provider::gemini_path_model(target);
        if (pm.at != std::string_view::npos && !provider::path_model_ok(pm.model))
            return {"model in the path unusable", refuse::kPathModel};
        return {};
    }


    TranslationPlan resolve_dialect(const ClientConn* c, const Upstream& venue) noexcept
    {
        const std::string_view head(c->rbuf.data(), c->msg.header_len);
        const std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
        // Whether the client asked to stream matters only to an OpenAI client of a venue
        // that cannot stream, and answering it walks the body to its top-level `stream`.
        const Dialect client = client_dialect_of(head);
        const bool stream_matters = client == Dialect::OpenAI && !provider::spec(venue.dialect).streams;
        return resolve_translation(client, venue.dialect, stream_matters && provider::wants_stream(body));
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
        if (!ok)
        {
            (void)provider::json::parse(body, ok, provider::json::Keys::Any);
            return ok ? refuse::kRepeatedKey : refuse::kNotJson;
        }
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
                    if (!aok || !parsed.is_object()) return refuse::kToolArgs;
                }
            }
        return refuse::kShape;
    }

    const char* dialect_name(UpstreamDialect m) noexcept
    {
        return provider::spec(m).name.data(); // a literal, so terminated
    }

    // A minimal HTTP/1.1 message: start line, JSON type, keep-alive, length, `extra`
    // (zero or more complete "Name: value\r\n" lines), then the body.
    void append_http(std::string& out, std::string_view start_line, std::string_view body,
                     std::string_view extra)
    {
        const size_t need = out.size() + start_line.size() + body.size() + extra.size() + 96;
        if (out.capacity() < need) out.reserve(need);
        out.append(start_line);
        out.append("\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n");
        net::http::append_content_length(out, body.size());
        out.append(extra);
        out.append("\r\n");
        out.append(body);
    }

    // Kept so the framing has one test; build_translated_request writes the same shape.
    void build_http_request(std::string_view start_line, std::string_view body,
                            std::string_view host, std::string_view extra, std::string& into)
    {
        into.clear();
        const size_t need = start_line.size() + body.size() + host.size() + extra.size() + 128;
        if (into.capacity() < need) into.reserve(need + need / 8);
        into.append(start_line).append("\r\nHost: ").append(host).append("\r\n").append(extra);
        into.append("Content-Type: application/json\r\nConnection: keep-alive\r\n");
        net::http::append_content_length(into, body.size());
        into.append("\r\n").append(body);
    }

    /// A byte-forwarded request, rebuilt in one pass into `into`: Host names the venue,
    /// because the request goes to a different origin than the client addressed; the
    /// Content-Length is ours, from the bytes actually sent; Expect asks the upstream
    /// for an interim reply we cannot relay; Accept-Encoding goes so the provider
    /// answers in a body we can measure; the hop-by-hop headers and any the client's
    /// Connection names are this hop's, so the venue alone decides its own keep-alive;
    /// and an x-kottos-key- header holds a provider key no venue may see.
    /// The request line is copied, never tested as a header, and an HTTP/1.0 one goes
    /// as 1.1 so the upstream connection can be pooled.
    const char* build_forward(std::string_view msg, size_t header_len, const HeadFacts& f,
                              const std::vector<std::string>& strip, std::string_view host_hdr,
                              std::string_view base_path, std::string_view body,
                              std::string& into, size_t declared, const NamedKey& key,
                              std::string_view beta, std::string_view path_model, UpstreamDialect venue)
    {
        using net::http::Field;
        into.clear();
        if (f.connections > HeadFacts::kConnections) return "too many Connection headers";
        const auto listed = [&](std::string_view name) {
            for (uint8_t i = 0; i < f.connections; ++i)
                if (net::http::has_token(f.connection[i].in(msg), name)) return true;
            return false;
        };
        // The client's own beta list, which the policy's token joins in one line. Read only
        // with a token to add: without one the client's lines go as they came.
        std::string_view client_beta;
        if (!beta.empty() && !listed("anthropic-beta"))
        {
            if (f.anthropic_betas > 1) return "anthropic-beta repeated";
            client_beta = f.anthropic_beta.in(msg);
            if (!client_beta.empty() && !header_value_safe(client_beta)) return "anthropic-beta unusable";
            if (net::http::has_token(client_beta, beta)) beta = {};
        }
        const std::string_view head = msg.substr(0, header_len < 4 ? 0 : header_len - 4);
        const size_t eol = std::min(head.find("\r\n"), head.size());
        const std::string_view rl = head.substr(0, eol);
        // Origin-form only: absolute-form would pick the upstream vhost, authority-form is
        // CONNECT's, and a dot segment could resolve outside the base. Never normalized.
        const size_t sp1 = rl.find(' ');
        const size_t sp2 = sp1 == std::string_view::npos ? sp1 : rl.find(' ', sp1 + 1);
        if (sp2 == std::string_view::npos || sp2 == sp1 + 1 || rl[sp1 + 1] != '/')
            return "request target not origin-form";
        const std::string_view target = rl.substr(sp1 + 1, sp2 - sp1 - 1);
        if (!target_path_ok(target)) return "request target not origin-form";
        const std::string_view version = rl.substr(sp2 + 1);

        const size_t need = header_len + host_hdr.size() + base_path.size() + body.size() +
                            key.line.size() + key.value.size() + beta.size() + path_model.size() + 80;
        if (into.capacity() < need) into.reserve(need + need / 8);
        if (!path_model.empty() && !provider::path_model_ok(path_model)) return "model override unusable in a path";
        into.append(rl.substr(0, sp1 + 1)).append(base_path);
        // A Gemini venue takes the model, and its host's layout, in the path.
        if (provider::spec(venue).wire != provider::Wire::Gemini) into.append(target);
        else if (!provider::gemini_forward_target(target, path_model, venue, into))
        {
            into.clear();
            return "model override for a target that names no model";
        }
        into.append(" ");
        into.append(version == "HTTP/1.0" ? std::string_view("HTTP/1.1") : version).append("\r\n");
        if (!host_hdr.empty()) into.append("Host: ").append(host_hdr).append("\r\n");
        // A bodyless request that carried no length keeps carrying none. A body still
        // arriving declares all of it.
        if (f.content_length || declared) net::http::append_content_length(into, declared);
        // A named key is the one credential: every one the client sent goes.
        if (!key.empty()) into.append(key.line).append(key.value).append("\r\n");
        const bool merge = !beta.empty();
        if (merge)
        {
            into.append("anthropic-beta: ").append(client_beta);
            into.append(client_beta.empty() ? "" : ",").append(beta).append("\r\n");
        }
        // Copied in runs: a flush happens only where a dropped line interrupts kept ones.
        size_t run = eol + 2;
        const bool ok = net::http::walk_headers(head, [&](const net::http::HeaderLine& h) noexcept {
            const bool drop = (h.field == Field::Host && !host_hdr.empty()) ||
                              h.field == Field::ContentLength || h.field == Field::Expect ||
                              h.field == Field::AcceptEncoding || net::http::hop_by_hop(h.field) ||
                              stripped(h.name, strip) ||
                              (!key.empty() && (h.field == Field::Authorization || h.field == Field::XApiKey ||
                                                h.field == Field::ApiKey || h.field == Field::XGoogApiKey)) ||
                              (h.field == Field::AnthropicBeta && (merge || listed(h.name))) ||
                              (h.field == Field::Other &&
                               (net::http::detail::line_is(h.name, kKeyHeaderPrefix) || listed(h.name)));
            if (!drop) return true;
            const size_t at = static_cast<size_t>(h.name.data() - head.data());
            if (at > run) into.append(head.substr(run, at - run));
            run = static_cast<size_t>(h.value.data() + h.value.size() - head.data()) + 2;
            return true;
        });
        if (!ok) { into.clear(); return "request head"; }
        if (run < head.size()) into.append(head.substr(run)).append("\r\n");
        into.append("\r\n").append(body);
        return nullptr;
    }

    bool ModelSplice::matches(std::string_view body, size_t at, size_t end) const noexcept
    {
        return end != SIZE_MAX && end - at == sentinel.size() + 2 && end <= body.size() && body[at] == '"' &&
               body.compare(at + 1, sentinel.size(), sentinel) == 0 && body[end - 1] == '"';
    }

    void ModelSplice::append(std::string& out, std::string_view body, size_t from, size_t to, size_t body_len,
                             size_t at, size_t end) const
    {
        const bool hit = matches(body, at, end);
        if (hit && from <= at && at < to)
        {
            out.append(body.substr(from, at - from)).append(1, '"').append(replacement).append(1, '"');
            out.append(shrink(), ' '); // whitespace after a value is JSON's
            from = end;
        }
        if (from < to) out.append(body.substr(from, to - from));
        if (to == body_len && !hit) out.append(grow(), ' '); // and after the root
    }

    bool request_without(std::string_view msg, size_t header_len,
                         const std::vector<std::string>& strip,
                         std::string_view host_hdr, std::string_view base_path,
                         std::string_view body_override, std::string& into,
                         std::string_view key_header, UpstreamDialect venue, Refusal* refused,
                         std::string_view beta)
    {
        const auto fail = [&](Refusal r) {
            if (refused) *refused = r;
            into.clear();
            return false;
        };
        HeadFacts f;
        const std::string_view head = msg.substr(0, header_len < 4 ? 0 : header_len - 4);
        if (!net::http::walk_headers(head, HeadReader{msg, strip, f})) return fail({"request framing", nullptr});
        NamedKey key;
        if (!key_header.empty())
            if (const Refusal r = named_key(msg, f, key_header, venue, key)) return fail(r);
        const std::string_view body = body_override.empty() ? msg.substr(header_len) : body_override;
        if (const char* bad = build_forward(msg, header_len, f, strip, host_hdr, base_path, body, into,
                                            body.size(), key, beta, {}, venue))
            return fail({bad, nullptr});
        return true;
    }

    /// Everything between a client request and the bytes for a translated venue, written
    /// straight into `out`. The body is parsed once, here; a model override reaches the
    /// venue's translator and its path through RequestOut, never a splice and a reparse.
    /// The body and target come before the credential, because a Bedrock signature
    /// covers both.
    bool build_translated_request(const Upstream& up, UpstreamDialect mode,
                                  std::string_view body, std::string_view head,
                                  const HeadFacts& f, std::string& out,
                                  std::string& body_scratch, const char*& why,
                                  std::string_view model_override, bool* wants_stream_usage,
                                  const NamedKey& key, std::string_view beta)
    {
        why = "translate";
        out.clear();
        const provider::VenueSpec& s = provider::spec(mode);
        bool ok = false;
        const provider::json::Value v = provider::parse_request_body(body, ok);
        if (!ok || !v.is_object() || !s.request) return false;
        // An override replaces a model the client named, never a missing one.
        const provider::json::Value* m = v.find("model");
        if (!model_override.empty() && (!m || !m->is_string() || !provider::plain_json_text(model_override)))
            return false;
        provider::RequestOut o{body_scratch, body, model_override};
        if (!s.request(v, o)) return false;
        if (wants_stream_usage) *wants_stream_usage = o.include_usage;
        const std::string_view tbody = body_scratch;

        const size_t need = tbody.size() + up.base_path.size() + up.query.size() + up.host_hdr.size() +
                            key.value.size() + beta.size() + 640;
        if (out.capacity() < need) out.reserve(need + need / 8);
        // The venue's path from the model it is asked for; its query, not the client's:
        // Azure's api-version rides there.
        out.append("POST ").append(up.base_path);
        if (!s.target(model_override.empty() ? v.str_or("model") : model_override, out))
        { out.clear(); return false; }
        if (!up.query.empty()) out.append("?").append(up.query);
        out.append(" HTTP/1.1\r\nHost: ").append(up.host_hdr).append("\r\n");
        if (s.auth == provider::Auth::SigV4)
        {
            // Signed over exactly the target written above, between "POST " and " HTTP/1.1".
            const std::string path(out.data() + 5, out.find(" HTTP/1.1") - 5);
            // Never signed with whatever Authorization holds beside a named key.
            if (!key.empty()) { why = "credential"; out.clear(); return false; }
            if (!sign_bedrock(up, f.authorization.in(head), path, tbody, out, why))
            { out.clear(); return false; }
        }
        else if (const char* bad = append_auth(mode, head, f, key, out, beta, up.auth_bearer))
        { why = bad; out.clear(); return false; }
        out.append("Content-Type: application/json\r\nConnection: keep-alive\r\n");
        net::http::append_content_length(out, tbody.size());
        out.append("\r\n").append(tbody);
        return true;
    }

    // Translate an upstream response body back to the OpenAI shape into `out`, with
    // the counts it states in `u`. False: the body could not be translated.
    bool xlate_resp(UpstreamDialect mode, std::string_view body, std::string& out,
                    provider::openai::Usage& u)
    {
        if (const auto response = provider::spec(mode).response) return response(body, out, u);
        out.clear();
        u = {};
        return false;
    }

    /// `Host:` for a rebuilt request. The parsed hostname when there is one, since
    /// providers route and verify on it; ip:port otherwise. Default ports omitted.
    std::string host_header_for(const Upstream& u)
    {
        // A name with no address yet stands in until one resolves; nothing is sent meanwhile.
        if (u.sni_host.empty()) return (u.ip.empty() ? u.host : u.ip) + ":" + std::to_string(u.port);
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
