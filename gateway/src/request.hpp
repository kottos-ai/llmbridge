// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Everything between a framed client request and the bytes for a venue: dialect
// resolution, the rebuild of a byte-forwarded request, the translated request with
// its credential headers, and the response translated back. The credential path
// is in here, in one file.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/dialect.hpp"
#include "gateway/metrics.hpp"
#include "core/conn.hpp"
#include "engine.hpp"
#include "provider/body_scan.hpp"
#include "provider/translate.hpp"

namespace llmbridge::detail
{
    /// The request line for a log line, up to its query, which may hold a key (`?key=`)
    /// as no header after it may; cut at 128 bytes so a hostile URI cannot dominate the
    /// log, and at the first CR.
    inline std::string_view request_line(std::string_view buf) noexcept
    {
        constexpr size_t kMax = 128;
        const size_t eol = std::min(buf.find('\r'), buf.find('?'));
        size_t n = eol == std::string_view::npos ? buf.size() : eol;
        if (n > kMax) n = kMax;
        return buf.substr(0, n);
    }

    /// Status code off the front of a response we built; 0 if it is not one.
    inline int status_of(std::string_view resp) noexcept
    {
        if (resp.size() < 12 || resp.compare(0, 5, "HTTP/") != 0) return 0;
        const char a = resp[9], b = resp[10], c = resp[11];
        if (a < '0' || a > '9' || b < '0' || b > '9' || c < '0' || c > '9') return 0;
        return (a - '0') * 100 + (b - '0') * 10 + (c - '0');
    }

    /// The venue was sent HEAD, so its response has no body. Only a byte-forward keeps
    /// the client's method; a translated request is always a POST.
    [[nodiscard]] inline bool sent_head(const ClientConn& c) noexcept
    {
        return c.msg.head && !c.req.f.translate_body;
    }

    [[nodiscard]] inline int64_t span_since(int64_t start, int64_t end) noexcept
    {
        return (start > 0 && end >= start) ? end - start : 0;
    }

    /// A request's intervals (LATENCY.md); a stream passes t4 for the t5 it has none of.
    [[nodiscard]] inline TimingSplit split_of(const ReqState& f, int64_t t5) noexcept
    {
        return timing_split(f.ts_req_recvd, f.ts_req_built, f.ts_wire_ready, f.ts_up_sent,
                            f.ts_up_recvd, t5, f.ts_body_sent, f.body_work_ns);
    }

    constexpr std::string_view kContinue = "HTTP/1.1 100 Continue\r\n\r\n";

    /// Fills a HeadFacts from each header line it is shown, offsets from `head`'s start;
    /// handed to net::http::parse_request so framing and the facts are one walk.
    struct HeadReader
    {
        std::string_view head;
        const std::vector<std::string>& strip;
        HeadFacts& f;
        bool operator()(const net::http::HeaderLine& h) noexcept;
    };

    /// A key a policy named, as its venue takes it: the line up to the key, and the key.
    struct NamedKey
    {
        std::string_view line, value;
        [[nodiscard]] bool empty() const noexcept { return line.empty(); }
    };
    /// A refused request: the reason logged, and the client's text (null for the generic).
    struct Refusal
    {
        const char* why = nullptr;
        const char* detail = nullptr;
        explicit operator bool() const noexcept { return why != nullptr; }
    };
    inline constexpr const char* kWhyKeyMissing = "credential header missing";
    inline constexpr const char* kWhyKeyRepeated = "credential header repeated";
    inline constexpr const char* kWhyKeyUnusable = "credential header unusable";
    inline constexpr const char* kWhyKeyTooMany = "too many credential headers";
    /// The key in the family header `name` (key_header_ok) as `venue` takes it, from the
    /// framing walk's facts alone. Refused for a venue that signs, past the walk's table,
    /// repeated in any case, absent or empty, or unsafe; nothing else stands in.
    [[nodiscard]] Refusal named_key(std::string_view head, const HeadFacts& f, std::string_view name,
                                    UpstreamDialect venue, NamedKey& out, bool bearer = false) noexcept;

    /// The policy's anthropic-beta token for `up`: only a venue taking an x-api-key gets one.
    [[nodiscard]] inline std::string_view beta_for(const Upstream& up, const ReqState& f) noexcept
    {
        return provider::spec(up.dialect).auth == provider::Auth::XApiKey ? f.beta_override.view()
                                                                          : std::string_view{};
    }

    /// The client's dialect from the request line of `head`: client_wire on its method
    /// and full target.
    [[nodiscard]] Dialect client_dialect_of(std::string_view head) noexcept;
    /// The request target of `head`'s request line, query included; empty if it has none.
    [[nodiscard]] std::string_view target_of(std::string_view head) noexcept;
    /// Why a target is refused before any venue hears of it: a `key` query parameter, the
    /// Gemini Interactions API, a Gemini stream without alt=sse, a path model that cannot
    /// stand as a segment. The target itself is never logged: it may hold the key.
    [[nodiscard]] Refusal target_refusal(std::string_view head) noexcept;

    /// The request line of `head` creates an OpenAI Responses API response, whose
    /// search calls are billed per call and stated only as output items.
    [[nodiscard]] bool responses_api(std::string_view head) noexcept;
    /// application/json or a +json type, whatever its parameters and case.
    [[nodiscard]] bool json_media_type(std::string_view content_type) noexcept;
    /// Everything a policy or the sink reads from the body; the gateway calls this
    /// once per request.
    [[nodiscard]] provider::TopLevelFacts body_facts(std::string_view head,
                                                     std::string_view body) noexcept;
    TranslationPlan resolve_dialect(const ClientConn* c, const Upstream& venue) noexcept;
    const char* translate_failure(std::string_view body) noexcept;
    const char* dialect_name(UpstreamDialect m) noexcept;
    void append_http(std::string& out, std::string_view start_line, std::string_view body,
                     std::string_view extra); // appended: `out` is a stage, capacity kept
    void build_http_request(std::string_view start_line, std::string_view body,
                            std::string_view host, std::string_view extra, std::string& into);
    /// The byte-forwarded request into `into`, capacity kept: `msg`'s head, then `body`,
    /// all or the first part of one `declared` bytes long. Null, or why to refuse it,
    /// with `into` empty. A named `key` replaces every credential header the client sent.
    /// A `beta` token goes as one anthropic-beta line, after the client's own value. A
    /// `path_model` (path_model_ok) replaces the model a Gemini target names, and a
    /// Gemini or Vertex `venue` gets its own path layout (provider::gemini_forward_target).
    const char* build_forward(std::string_view msg, size_t header_len, const HeadFacts& f,
                              const std::vector<std::string>& strip, std::string_view host_hdr,
                              std::string_view base_path, std::string_view body,
                              std::string& into, size_t declared, const NamedKey& key = {},
                              std::string_view beta = {}, std::string_view path_model = {},
                              UpstreamDialect venue = UpstreamDialect::OpenAI);
    /// A conditional model substitution spliced into a body forwarded as it arrives
    /// (GATEWAY-INTERNALS.md 12d). The head declares the client's length plus grow(): a
    /// value that is the sentinel goes as the replacement, then shrink() spaces; any other
    /// value goes as sent, and grow() spaces follow the body's last byte. Either way the
    /// venue gets exactly the declared length, and valid JSON when the client's was.
    struct ModelSplice
    {
        std::string_view sentinel, replacement; ///< both substitute_ok: no escaping needed

        [[nodiscard]] size_t grow() const noexcept
        {
            return replacement.size() > sentinel.size() ? replacement.size() - sentinel.size() : 0;
        }
        [[nodiscard]] size_t shrink() const noexcept
        {
            return sentinel.size() > replacement.size() ? sentinel.size() - replacement.size() : 0;
        }
        /// The value at [at, end) of `body` is the sentinel, raw: `"` sentinel `"`.
        [[nodiscard]] bool matches(std::string_view body, size_t at, size_t end) const noexcept;
        /// Appends the venue's bytes for body[from, to), of a body `body_len` long whose
        /// model value is [at, end) (BodyScan::model_at and model_end; end SIZE_MAX while
        /// open or absent). Neither `from` nor `to` may fall inside the value.
        void append(std::string& out, std::string_view body, size_t from, size_t to, size_t body_len, size_t at,
                    size_t end) const;
    };
    /// Decision::service_tier, speed and inference_geo spliced into a body forwarded as it
    /// arrives (GATEWAY-INTERNALS.md 12e). The members go in after the root's `{`, as
    /// apply_overrides inserts absent ones, then the separator: `,` before a member the
    /// client keeps, else a space. A member of the client's that one replaces goes as
    /// spaces, with the separator before it. The head declares the client's length plus
    /// grow(), whatever the client sent.
    struct MemberSplice
    {
        std::string_view service_tier, speed, inference_geo; ///< empty: not set

        /// BodyScan's bits for the members set.
        [[nodiscard]] uint8_t mask() const noexcept;
        /// The members and their separator; 0 when none is set.
        [[nodiscard]] size_t grow() const noexcept;
        /// `"service_tier":"flex","speed":"fast"`: apply_overrides' text, in its order.
        void append_members(std::string& out) const;
        /// Every value goes between quotes as it is: no control byte, DEL, `"` or `\`,
        /// which apply_overrides refuses.
        [[nodiscard]] bool plain() const noexcept;
    };
    /// What a body forwarded as it arrives carries spliced in: a model substitution
    /// (`model.sentinel` empty for none) and members, over BodyScan's spans.
    struct Splice
    {
        ModelSplice model;
        MemberSplice members;

        [[nodiscard]] size_t grow() const noexcept { return model.grow() + members.grow(); }
        /// The body's model was the sentinel and goes replaced.
        [[nodiscard]] bool substitutes(std::string_view body, const provider::BodyScan& s) const noexcept;
        /// Appends the venue's bytes for body[from, to) of a body `body_len` long. Neither
        /// `from` nor `to` may fall inside a span BodyScan holds back.
        void append(std::string& out, std::string_view body, size_t from, size_t to, size_t body_len,
                    const provider::BodyScan& s) const;
    };
    /// A Decision's or a Retry's substitution: none, or both halves substitute_ok.
    [[nodiscard]] inline bool substitution_ok(std::string_view model, std::string_view for_model) noexcept
    {
        return (model.empty() && for_model.empty()) ||
               (substitute_ok(model, kMaxSubstituteModel) && substitute_ok(for_model, kMaxSubstituteFor));
    }

    /// build_forward over a message whose facts are read here, with the key in family
    /// header `key_header` sent as `venue` takes it; false refuses it, why in `refused`.
    bool request_without(std::string_view msg, size_t header_len,
                         const std::vector<std::string>& strip,
                         std::string_view host_hdr, std::string_view base_path,
                         std::string_view body_override, std::string& into,
                         std::string_view key_header = {},
                         UpstreamDialect venue = UpstreamDialect::OpenAI, Refusal* refused = nullptr,
                         std::string_view beta = {});
    /// `out` receives the framed request and `body_scratch` the translated body; both
    /// keep their capacity. `head` is the client's, `f` its facts. On false, `why` is
    /// "translate", "credential", "bedrock credential" or "no credential". A named `key`
    /// is the only credential sent. A `beta` token is the only anthropic-beta line, sent
    /// to a venue taking an x-api-key: the client's own does not cross a translation.
    bool build_translated_request(const Upstream& up, UpstreamDialect mode,
                                  std::string_view body, std::string_view head,
                                  const HeadFacts& f, std::string& out,
                                  std::string& body_scratch, const char*& why,
                                  std::string_view model_override = {},
                                  bool* wants_stream_usage = nullptr, const NamedKey& key = {},
                                  std::string_view beta = {});
    /// The credential lines a translated request carries, from one header block, the key
    /// in family header `key_header` when one is named. A test seam over the facts the
    /// framing walk reads.
    bool auth_headers_for(UpstreamDialect mode, std::string_view client_headers,
                          const std::vector<std::string>& strip, std::string& out,
                          std::string_view key_header = {}, Refusal* refused = nullptr);
    bool xlate_resp(UpstreamDialect mode, std::string_view body, std::string& out,
                    provider::openai::Usage& u);
    std::string host_header_for(const Upstream& u);
    std::string aws_region_for(const Upstream& u);
    std::string peer_of(int fd) noexcept;
} // namespace llmbridge::detail
