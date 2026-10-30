// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "net/http.hpp"

/// May this request proceed? llmbridge authenticates and meters nobody: whoever needs it
/// supplies a Policy. A stock build installs none. Design: DESIGN.md "The policy seam".
namespace llmbridge
{
    /// Metadata only, no route to the body. `head` carries the client's keys (Authorization,
    /// x-kottos-key-*): never log a RequestFacts or keep a view; use net::http::find_header.
    struct RequestFacts
    {
        std::string_view head;  ///< request line + headers, through the CRLFCRLF
        size_t body_bytes = 0;  ///< Content-Length as framed. Bytes, not tokens
        std::string_view model; ///< top-level plain-string `model`, else empty; Gemini's path model
        uint64_t prefix_hash = 0;
        uint64_t seq = 0; ///< the gateway's request sequencer
        /// Top-level `"stream": true` and `stream_options.include_usage`; anything else is false.
        bool stream = false;
        bool include_usage = false;
        /// False when decide() runs on the head of a body still arriving (decides_on_head):
        /// `model` is a top-level `model` that has closed in the bytes so far, else empty;
        /// `stream` and `include_usage` are false (a Gemini client's: its path's); `prefix_hash`
        /// is over a held body's bytes, the gateway waiting for them. confirm() gets the whole's.
        bool body_whole = true;
        /// At confirm() only: the body is streaming to the venue with its top-level `model`,
        /// the plain string Decision::substitute_for (`model` above), already replaced by
        /// Decision::substitute_model. The decision in force has been carried out: returning
        /// it keeps the request, with no redo. False for a parked body, rewritten after.
        bool model_substituted = false;
        /// At confirm() only: the streaming body went with Decision::service_tier, speed or
        /// inference_geo spliced in, the client's own blanked (GATEWAY-INTERNALS.md 12e).
        bool service_tier_spliced = false;
        bool speed_spliced = false;
        bool inference_geo_spliced = false;
    };

    inline constexpr size_t kMaxDenyMessage = 256;

    /// The rule for Decision::message, public so a policy can static_assert its own. It lands
    /// unescaped in a JSON string: 1 to kMaxDenyMessage bytes of printable ASCII, no `"` or `\`.
    [[nodiscard]] constexpr bool deny_message_ok(const char* m) noexcept
    {
        if (!m) return false;
        size_t n = 0;
        for (; m[n] != '\0'; ++n)
        {
            const auto c = static_cast<unsigned char>(m[n]);
            if (n == kMaxDenyMessage || c < 0x20 || c > 0x7E || c == '"' || c == '\\')
                return false;
        }
        return n > 0;
    }

    inline constexpr size_t kMaxSubstituteModel = 255;
    inline constexpr size_t kMaxSubstituteFor = 64;

    /// The rule for Decision::substitute_model (at most kMaxSubstituteModel bytes) and
    /// Decision::substitute_for (kMaxSubstituteFor), public so a policy can static_assert its
    /// own: the gateway writes either between quotes in a JSON body unescaped, and declares
    /// a streamed body's length from their sizes, so each is 1 or more bytes of printable
    /// ASCII with no `"` or `\`.
    [[nodiscard]] constexpr bool substitute_ok(std::string_view m, size_t max) noexcept
    {
        if (m.empty() || m.size() > max) return false;
        for (const char ch : m)
        {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
        }
        return true;
    }

    /// Headers holding provider keys: no name with this prefix, in any case, goes upstream.
    inline constexpr std::string_view kKeyHeaderPrefix = "x-kottos-key-";
    inline constexpr size_t kMaxKeyHeaderName = 64;

    /// The rule for Decision::credential_header and Retry::credential_header, public so a
    /// policy can static_assert its own: the prefix in any case, then at least one of
    /// [A-Za-z0-9._-], kMaxKeyHeaderName bytes in all.
    [[nodiscard]] constexpr bool key_header_ok(std::string_view name) noexcept
    {
        if (name.size() <= kKeyHeaderPrefix.size() || name.size() > kMaxKeyHeaderName) return false;
        for (size_t i = 0; i < name.size(); ++i)
        {
            const char c = net::http::detail::lc(name[i]);
            const bool ok = i < kKeyHeaderPrefix.size()
                                ? c == kKeyHeaderPrefix[i]
                                : (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                                      c == '_' || c == '.';
            if (!ok) return false;
        }
        return true;
    }

    inline constexpr size_t kMaxBetaToken = 64;

    /// The rule for Decision::anthropic_beta and Retry::anthropic_beta: 1 to kMaxBetaToken
    /// bytes of [a-z0-9._-], such as `fast-mode-2026-02-01`. One token, never a list.
    [[nodiscard]] constexpr bool beta_token_ok(std::string_view t) noexcept
    {
        if (t.empty() || t.size() > kMaxBetaToken) return false;
        for (const char c : t)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
                return false;
        return true;
    }

    /// At most this many venues besides the decision's own hear a hedged request.
    inline constexpr size_t kMaxHedgeLegs = 3;

    /// One more venue a hedged request goes to (Decision::hedge), each field as Decision's
    /// own and copied like them: GATEWAY-INTERNALS.md 13.
    struct HedgeLeg
    {
        int upstream_index = -1; ///< in range, or the request is a 500: a leg never falls back to venue 0
        std::string_view model{};
        std::string_view service_tier{};
        std::string_view speed{};
        std::string_view inference_geo{};
        std::string_view anthropic_beta{};
        /// Required, key_header_ok: a client's own key sent to several venues would reach
        /// companies it was never meant for.
        std::string_view credential_header{};
    };

    struct Decision
    {
        /// `= false` so `Decision d;` refuses too; `Decision{}` zeroes it either way.
        bool allow = false;
        /// Must be 400-599; the gateway substitutes 403 for anything else and warns.
        int deny_status = 401;
        /// Index into the gateway's upstream table; out of range (the -1 default too) means the
        /// first one, but past the end with credential_header set is a 500. Ignored unless `allow`.
        int upstream_index = -1;

        /// Rewrite the request's `model` before sending (in a Gemini path: path_model_ok); empty
        /// leaves the client's. Valid until decide() returns; the gateway copies it, and a longer
        /// one than 255 bytes, or one a path cannot hold, gets the request a 500.
        std::string_view model{};

        /// Set the request's `service_tier`, at most 32 bytes; empty leaves the body alone.
        /// A streamed body gets it, `speed` and `inference_geo` spliced in: 12e, no park.
        std::string_view service_tier{};

        /// Logged, never sent; must outlive the call and carry no credential material.
        const char* reason = "policy denied";
        const char* message = nullptr;
        uint64_t tag = 0;

        /// The client header holding the key for the venue picked here, key_header_ok,
        /// such as `x-kottos-key-anthropic`; empty sends the client's own credentials. Its
        /// value goes as the venue's own key header and is the only credential sent: absent,
        /// empty, repeated or unsafe is a 400, never another header. Copied, like `model`.
        std::string_view credential_header{};

        /// A decision on the head only: allowed, but nothing goes upstream before the
        /// body's last byte. Ignored on a whole body and from confirm().
        bool hold = false;

        /// Set the request's top-level `speed` and `inference_geo` (Anthropic's fast mode
        /// and data residency), at most 32 bytes each, spliced as service_tier is: on a
        /// byte-forward only. Empty leaves the body alone.
        std::string_view speed{};
        std::string_view inference_geo{};
        /// A token, beta_token_ok, added to the request's `anthropic-beta` header when the
        /// venue takes an x-api-key; any other venue gets none. Empty adds nothing.
        std::string_view anthropic_beta{};

        /// A conditional model substitution, the one a head decision can make without the
        /// model: when the body's top-level `model` is the plain JSON string `substitute_for`
        /// (its raw bytes, no escape), it goes as `substitute_model` instead; any other model,
        /// or none, goes as the client sent it. Both or neither, each substitute_ok (else a
        /// 500); `model` set wins over it. On a body streamed as it arrives the gateway
        /// splices it into the stream, so the body need not park: GATEWAY-INTERNALS.md 12d.
        /// Copied, like `model`. kottos sets `"auto"` and the model it routes "auto" to.
        std::string_view substitute_model{};
        std::string_view substitute_for{};

        /// A hedge: the request also goes, at once, to each of `hedge_legs` more venues (at
        /// most kMaxHedgeLegs), each leg, the decision's own included, its own request with
        /// its own record. The body is held to its last byte first, as with `hold`. The first
        /// leg to show a token, or a whole 2xx reply when not streamed, serves the client and
        /// the others are reset unanswered; with no success, the last leg to fail answers it.
        /// The decision names a credential_header too, and makes no substitution, or the
        /// request is a 500. Valid until decide() returns; the gateway copies the legs.
        const HedgeLeg* hedge = nullptr;
        uint8_t hedge_legs = 0;
    };

    /// A venue did not answer (refused connect, failed write, EOF or silence before the
    /// response was whole) and the client has seen nothing. `retryable`: the policy may fail
    /// over; false (one venue, a response byte in, the third venue) ignores its answer and
    /// leaves the failure for it to count. An unparseable answer never reaches here.
    struct FailureFacts
    {
        int upstream_index = -1; ///< the venue that just failed
        int status = 502;        ///< what the client gets if nothing is retried
        const char* reason = ""; ///< the gateway's short literal for the failure
        int attempt = 0;         ///< venues already tried for this request, 0 on the first
        uint64_t tag = 0;        ///< Decision::tag of this request, verbatim; 0 if unset
        uint64_t seq = 0;        ///< RequestFacts::seq of this request, as RequestRecord::seq carries it
        bool retryable = true;   ///< false: the gateway gives up whatever on_failure returns
    };

    /// A venue answered: the status of a response head the gateway parsed, its 503 as much as
    /// its 200, even when the body then cannot be framed and the client gets the gateway's 502.
    /// Never a status the gateway made up. A head it refuses, or an interim 1xx, reaches
    /// neither this nor on_failure: DESIGN.md "When a venue answers".
    struct ReplyFacts
    {
        int upstream_index = -1; ///< the venue that answered
        int status = 0;          ///< its three-digit status as sent; never an interim 1xx
        int attempt = 0;         ///< venues already tried for this request, 0 on the first
        uint64_t tag = 0;        ///< Decision::tag of this request, verbatim; 0 if unset
        uint64_t seq = 0;        ///< RequestFacts::seq of this request, as RequestRecord::seq carries it
        bool streamed = false;   ///< a 200 event stream, reported at its head
    };

    /// Value-initialised means give up and let the client see the error.
    struct Retry
    {
        bool retry = false;
        int upstream_index = -1; ///< must be in range, and not the one that just failed
        /// The new venue's overrides, as Decision's; empty sends the client's own, never
        /// the failed venue's. Valid until on_failure returns.
        std::string_view model{};
        std::string_view service_tier{};
        /// As Decision::credential_header, for the venue named here. Left empty after any
        /// attempt that named one, the decision or a retry, the request is not retried.
        std::string_view credential_header{};
        /// As Decision's, for the venue named here; empty sends none of the failed one's.
        /// Still streaming, these and `service_tier` splice only if the head decision set
        /// each too (12e); else the body parks.
        std::string_view speed{};
        std::string_view inference_geo{};
        std::string_view anthropic_beta{};
        /// On a body still streaming it is spliced only when the head decision named one too (the gateway
        /// then tracked the model from the first byte); else the body parks, as for `model`.
        std::string_view substitute_model{};
        std::string_view substitute_for{};
    };

    /// Non-owning, fixed at Gateway construction, called on its loop thread once per framed
    /// request. A policy shared across workers must be thread-safe itself.
    class Policy
    {
      public:
        virtual ~Policy() = default;

        /// noexcept and allocation-free on the allow path: every request pays for it.
        virtual Decision decide(const RequestFacts& facts) noexcept = 0;

        /// Once per venue that failed with nothing sent to the client. The default never
        /// retries: llmbridge measures no health, so ordering, ejection and cooldown are the
        /// policy's. With FailureFacts::retryable false the answer is ignored: count, not route.
        virtual Retry on_failure(const FailureFacts&) noexcept { return {}; }

        virtual bool wants_prefix_hash() const noexcept { return false; }

        /// Opt in to deciding on the head of a large body, read once at construction.
        /// decide() then runs before the body is whole (RequestFacts::body_whole false),
        /// and the body may go to the venue as it arrives, its last byte withheld until
        /// confirm(). Off, every policy request is decided on its whole body, as before.
        virtual bool decides_on_head() const noexcept { return false; }

        /// Once per request decided on its head, with the whole body's facts, before its
        /// last byte leaves; never for a body decided whole, nor once the venue answered
        /// first. `early` is the decision in force: its venue (after any failover, never
        /// -1), overrides, key header and tag. Return it to keep: the last byte goes. A
        /// refusal (`allow` false, checked as decide()'s) resets a venue holding part of
        /// the body, short of its Content-Length, and the client gets its status and
        /// message. Any other allowed decision redoes the request: the venue is reset and
        /// the whole body goes as a held one under the new venue, overrides,
        /// credential_header and tag. A change of tag alone is a keep, the tag updated, and so
        /// is a decision that sends the venue the same model the one in force does, as a
        /// substitution or an override (`whole.model_substituted` says the splice is done),
        /// and the same service_tier, speed and inference_geo: an override spliced in, or with
        /// none on a spliced body the client's own (`whole.service_tier_spliced` and the rest).
        /// The request was counted at decide(): do not count it again here.
        virtual Decision confirm(const RequestFacts& whole, const Decision& early) noexcept
        {
            (void)whole;
            return early;
        }

        /// Once per venue response, before any of it reaches the client; it cannot change
        /// the reply. A whole response is reported once it is whole, a 200 stream at its
        /// head, translated or not, and a reply after a failover with its attempt. One cut
        /// short goes to on_failure instead, so each venue attempt reaches one hook at most.
        /// Not for a reply to a client already gone: the gateway closes that venue unread.
        /// The default does nothing; it runs on the loop thread on every response, so keep
        /// it as cheap as decide().
        virtual void on_reply(const ReplyFacts&) noexcept {}

      protected:
        Policy() = default;
        Policy(const Policy&) = default;
        Policy& operator=(const Policy&) = default;
    };
} // namespace llmbridge
