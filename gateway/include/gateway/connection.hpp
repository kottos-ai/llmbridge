// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// What a Gateway holds per socket and per venue: the dialect and TLS configuration,
// the upstream table entry, and Connection, the per-fd state both event loops share.
// Which buffer holds what, who may free a connection on each backend, and how the
// two TLS legs fit: GATEWAY-INTERNALS.md.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "gateway/sink.hpp"
#include "net/http.hpp"
#include "net/log.hpp"
#include "net/tls.hpp" // self-guarded by LLMBRIDGE_HAVE_TLS
#include "provider/sse.hpp"

namespace llmbridge
{
    /// What a refused request is told, in one place. The tests read these, so the
    /// wording lives here and editing it cannot break a test that is still right.
    namespace refuse
    {
        inline constexpr const char* kImage =
            "request translate: image content is not supported";
        inline constexpr const char* kAudio =
            "request translate: audio content is not supported";
        inline constexpr const char* kFile =
            "request translate: file content is not supported";
        inline constexpr const char* kPart =
            "request translate: unsupported content part; only \"text\" parts are carried";
        inline constexpr const char* kToolArgs =
            "request translate: tool_calls[].function.arguments must be a JSON object "
            "and nothing else";
        inline constexpr const char* kCredential =
            "a credential header holds bytes that cannot be forwarded "
            "(control characters are refused)";
        inline constexpr const char* kNotJson = "request translate: body is not valid JSON";
        inline constexpr const char* kNotObject =
            "request translate: body is not a JSON object";
        inline constexpr const char* kNoModel = "request translate: no \"model\" field";
        inline constexpr const char* kNoMessages = "request translate: no \"messages\" field";
        inline constexpr const char* kShape = "request translate: unsupported request shape";
    } // namespace refuse

    /// What a venue speaks. OpenAI byte-forwards; the rest translate on the way out
    /// and back.
    enum class UpstreamDialect
    {
        OpenAI,
        Anthropic,
        Gemini,
        Cohere,
        /// Bedrock's Messages endpoint: Anthropic's body, the model in the path, SigV4
        /// in place of a header swap. Needs a TLS build for the signing, so selecting
        /// it without one fails at startup instead of sending unsigned bytes.
        Bedrock,
        /// Azure OpenAI: OpenAI's body untouched, the deployment in the path, the
        /// api-version in the query, the credential in `api-key`. Byte-forwarding
        /// cannot do it, because it would have to merge our query with the client's
        /// target, which parse_upstream refuses.
        Azure,
    };

    /// TLS on either leg. Declared unconditionally so callers need no ifdefs; a build
    /// without TLS support fails run() when either flag is set, and never speaks
    /// plaintext in its place. The invariant everything hangs off: `Connection::rbuf`
    /// and `wbuf` hold plaintext always; ciphertext lives in `tls_out` and the Session.
    struct TlsConfig
    {
        /// Outbound, gateway to provider: we are the TLS client and verify them.
        bool upstream_tls = false;
        std::string sni_host; // DNS name for SNI + certificate hostname verification
        std::string ca_file;  // empty = system trust store (tests pass their own CA)

        /// Inbound, client to gateway: we are the TLS server. One listener, one mode:
        /// set, the single listener is TLS-only and there is no plaintext port, so
        /// "am I exposed in the clear?" is answered by the command line.
        bool client_tls = false;
        std::string cert_file; // PEM chain, leaf first (Let's Encrypt fullchain.pem)
        std::string key_file;  // PEM key, mode 600 or startup refuses it
    };

    /// One place a request can be sent. The gateway holds an ordered table and a policy
    /// picks the index per request; llmbridge never chooses. The dialect lives here
    /// because it is what makes cross-venue routing possible.
    struct Upstream
    {
        std::string ip{};     ///< resolved at startup; the table is not re-resolved
        uint16_t port = 0;
        bool tls = false;     ///< originate TLS to this venue
        std::string sni_host{}; ///< DNS name for SNI, hostname verification and the
                              ///< Host header. Empty for the bare IP:PORT form.
        UpstreamDialect dialect = UpstreamDialect::OpenAI;
        /// Prefixed to this venue's request target, for providers serving below the
        /// root. Empty, or "/..." with no trailing slash; net::parse_upstream enforces
        /// that, and a request whose own target is not origin-form is refused.
        std::string base_path{};

        std::string host_hdr{}; ///< derived at construction; see host_header_for()

        /// Query for this venue, without the '?'. Only a mode that builds its own
        /// target may carry one; a byte-forwarding venue would have to merge it with
        /// the client's, and the constructor refuses that.
        std::string query{};

        /// AWS region for SigV4, derived from the hostname at construction. Empty when
        /// the name carries none, which makes a Bedrock venue refuse every request
        /// instead of signing with a guess.
        std::string aws_region{};
    };

    /// Event-loop backend. Auto is io_uring when the kernel has it, else epoll.
    enum class IoBackend
    {
        Auto,
        Epoll,
        Uring,
    };

    /// Per-fd state, client or upstream. `is_client` says which, and several fields
    /// change meaning with it; the buffer names are the gateway's point of view on
    /// this socket, so the same name holds the request on a client conn and the
    /// response on an upstream one:
    ///
    ///   c->rbuf request in   u->wbuf request out
    ///   u->rbuf response in  c->wbuf response out
    ///
    /// Always plaintext on both legs. Ownership rules, including which backend may
    /// free a connection when: GATEWAY-INTERNALS.md sections 2, 5b and 7.
    struct Connection
    {
        /// Live-instance count, so a test can assert every Connection the gateway
        /// allocates is freed, without a sanitizer.
        Connection() noexcept { s_live.fetch_add(1, std::memory_order_relaxed); }
        ~Connection() { s_live.fetch_sub(1, std::memory_order_relaxed); }
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        inline static std::atomic<long> s_live{0};

        int fd = -1;
        bool is_client = true;
        /// Client only: the original request, kept so a failover can rebuild it for
        /// another venue; the upstream's wbuf was translated for the venue that
        /// failed. Empty in a stock build, so nobody pays for a copy they cannot use.
        std::string failover_req;
        int failover_attempts = 0; ///< venues already tried for the request in flight
        uint64_t policy_tag = 0;   ///< Decision::tag of the request in flight; see policy.hpp
        /// Sink capture: bounded header copies plus the wall clock at framing, taken
        /// then because the request buffer is reused by completion.
        int64_t wall_t0 = 0;
        char sink_cap[kSinkCaptureMax][kSinkCaptureBytes];
        uint8_t sink_cap_len[kSinkCaptureMax] = {};
        /// The client's model, for the sink. 64 bytes holds Bedrock's 43-character
        /// form, and a model name is not a prompt.
        char sink_model[64] = {};
        uint8_t sink_model_len = 0;

        /// Index into the upstream table, -1 when none applies. On an upstream
        /// connection the venue this socket talks to, so release finds the right
        /// pool; on a client connection the venue serving the request in flight,
        /// which is how the response leg knows the dialect after the upstream went
        /// back to its pool.
        int upstream_slot = -1;
        /// The translation resolved for the request in flight, from the client dialect
        /// and the venue's. See gateway/dialect.hpp. `effective_dialect` is meaningful
        /// only when `translate_body`.
        UpstreamDialect effective_dialect = UpstreamDialect::OpenAI;
        bool translate_body = false;
        /// Log identity, distinct from `id`: `id` is the client-map key and is 0 on
        /// every upstream. This one is process-unique for either kind and never
        /// changes, so a connection's whole life is greppable as "Connection#42".
        uint64_t log_inst = net::log::next_instance();
        /// Client conns: the sequencer value for the request in flight, assigned once
        /// at framing and read by the log lines and the x-llmbridge-seq header.
        uint64_t req_seq = 0;
        /// Provider-reported token counts for the request in flight, -1 when not
        /// reported. Non-streaming: scanned out of the translated body. Streaming:
        /// read off the SSE translator at finalize. Metadata, never content.
        long long tok_in = -1;
        long long tok_out = -1;
        long long tok_cached = -1; // non-streaming: prompt_tokens_details.cached_tokens
        long long tok_cache_write = -1; // usage.cache_creation_input_tokens, when stated
        long long tok_cw_5m = -1, tok_cw_1h = -1; // usage.cache_creation, when stated
        bool write_armed = false;     // epoll backend only: EPOLLOUT currently registered
        bool connected = false;       // upstream-only: non-blocking connect done
        bool request_pending = false; // client-only: full request buffered, awaiting forward
        /// Closed, not yet freed. epoll frees at the end of the event batch; io_uring
        /// only once `inflight` hits 0, because a submitted SQE still references this
        /// object. Freeing on `doomed` alone is a use-after-free in a process holding
        /// customer credentials. GATEWAY-INTERNALS.md section 7.
        bool doomed = false;
        bool close_after_resp = false; // client-only: this is an error reply, so close once it flushes

        uint64_t id = 0; // client conns: stable id; upstream conns: 0

        /// Non-streaming chunked decode state, per connection so the decoder is fed
        /// only new bytes across reads; the shared-scratch form was quadratic.
        net::http::ResponseDecoder rdec;

        std::string rbuf;
        std::string wbuf;

        /// How much of wbuf has been dealt with. Plaintext counts bytes on the socket,
        /// TLS counts bytes fed into the Session; wire progress is tls_out_off. The
        /// write path never clears wbuf, which is what keeps a request resendable on
        /// a dead pooled connection. GATEWAY-INTERNALS.md section 2b.
        size_t woff = 0;

        /// Two stamps change meaning with `is_client`: `ts_accepted` is accept() on a
        /// client conn and socket creation on an upstream; `ts_first_byte` is the
        /// first request byte against the first response byte. The `client_` prefix
        /// marks a field meaningless on an upstream conn; `from_pool` and `retried`
        /// are upstream-only.
        int64_t ts_accepted = 0;
        /// Client conns: when this connection last completed a request. The setup
        /// deadline reaps a client that never framed anything; this reaps one that
        /// framed something long ago and then sat on the descriptor.
        int64_t ts_client_activity = 0;
        bool ever_framed = false;
        bool client_conn_reused = false;
        int64_t client_conn_setup_ns = 0;
        /// Total length of the request being buffered, learned from its headers on
        /// the first partial read, or 0 when none is in progress.
        size_t client_frame_want = 0;
        /// An interim `100 Continue` was handed to the TLS layer and its ciphertext
        /// has not fully left the socket.
        bool client_interim_inflight = false;

        Connection* peer = nullptr; // linked counterpart for the in-flight request
        net::http::Message msg{};

        /// Latency stamps (ns), held on the client conn for the active request; the
        /// t0-t6 scheme is in gateway.hpp and LATENCY.md. `client_upload_ns` is t0
        /// minus the first byte: how long the request took to arrive, which is the
        /// client's network and not our work.
        int64_t ts_first_byte = 0;
        int64_t client_upload_ns = 0;
        int64_t ts_req_recvd = 0;
        /// t1, the end of our request-side compute. Separate from what follows because
        /// a cold connection puts ~50 ms of TCP+TLS setup between it and the write,
        /// against ~5 us on a pooled one, and one number cannot honestly carry both.
        int64_t ts_req_built = 0;
        int64_t ts_wire_ready = 0;
        int64_t ts_up_sent = 0;
        int64_t ts_up_recvd = 0;
        /// Streaming only, outside the t0-t6 scheme: the first content token, the
        /// first thinking delta, and the longest silence between chunks once tokens
        /// have started.
        int64_t ts_first_token = 0;
        int64_t ts_first_thinking = 0;
        int64_t ts_last_chunk = 0;
        int64_t max_chunk_gap_ns = 0;
        /// What the provider said about its own limits on this response.
        uint8_t quota_exhausted = 0;
        uint16_t retry_after_s = 0;
        /// Last upstream progress, request forwarded or bytes received; the idle
        /// sweep measures against it.
        int64_t ts_up_activity = 0;

        /// io_uring only: submitted-but-uncompleted SQEs referencing this conn, one
        /// protocol with `doomed`. GATEWAY-INTERNALS.md section 5b.
        int inflight = 0;
        /// io_uring stale-connection handling: reused from the pool, so a failure
        /// before any response is retry-eligible, and already retried once.
        bool from_pool = false;
        bool retried = false;

        /// Streaming state, held on the client conn for the active request. `streaming`
        /// is a one-way latch set in ep_begin_stream / ur_begin_stream only.
        bool streaming = false;
        bool stream_chunked = false; // upstream body uses chunked transfer-encoding

        /// No further stream output will come. With close_after_resp set it was
        /// truncated, with it clear the end was clean. GATEWAY-INTERNALS.md 6b.
        bool stream_ended = false;

        /// Epoll only, and the one place the backends deliberately differ: epoll applies
        /// back-pressure to a slow client, io_uring drops the stream past kUrStreamBufCap.
        /// Written only by ep_pause_read / ep_resume_read. GATEWAY-INTERNALS.md 6c.
        bool read_paused = false;
        bool wants_usage = false;    // client set stream_options.include_usage
        /// What the policy asked this request's `model` and `service_tier` to become,
        /// empty for none, and what the client's own body carried, copied out because
        /// the request buffer is reused before the sink runs.
        std::string_view model_override{};
        std::string_view tier_override{};
        char asked_tier[16] = {};
        uint8_t asked_tier_len = 0;
        /// What the venue called this failure, from its own error body on a non-2xx.
        char upstream_error[32] = {};
        uint8_t upstream_error_len = 0;
        /// Client-only: the upstream this request took was already connected.
        bool upstream_pooled = false;
        char served_tier[16] = {};
        uint8_t served_tier_len = 0;
        /// Reads searched for the served tier so far, so a venue without the field
        /// is given up on quickly instead of searched on every chunk.
        uint8_t served_tier_tries = 0;

        /// The Anthropic-to-OpenAI SSE translator, null when the stream needs none.
        std::unique_ptr<provider::AnthropicToOpenAiSse> sse_xlate;
        /// Tail of a byte-forwarded stream, for the final usage chunk. Bounded; see
        /// stream_note_usage.
        std::string stream_tail{};
        /// Scratch for one streaming step's decoded bytes, reused across chunks.
        std::string sse_scratch{};
        /// The stream is framed to the client with chunked transfer-encoding instead
        /// of being close-delimited. False for an HTTP/1.0 caller and for one that
        /// asked to close.
        bool stream_chunked_out = false;
        /// Usage accumulated as a byte-forwarded stream runs. -1 = not reported, which
        /// must never be rendered as a number. Fields and not a tail scan, because
        /// Anthropic states input and cache tokens in its first event.
        long long usage_in = -1, usage_out = -1, usage_cached = -1;
        long long usage_cache_write = -1; // cache_creation_input_tokens, first-wins
        long long usage_cw_5m = -1, usage_cw_1h = -1; // usage.cache_creation, first-wins
        net::http::ChunkDecoder chunkdec;                          // decodes the upstream chunked body
        /// io_uring streaming only: translated output accumulates here while a client
        /// send SQE is in flight, so `wbuf` is never reallocated under the kernel.
        std::string wpending;

        /// io_uring: an SQE referencing this connection's send buffer is outstanding,
        /// so that buffer must not move. Callers must never set it; two once did, and
        /// one of them hung every TLS stream. GATEWAY-INTERNALS.md section 5b.
        bool send_inflight = false;
        /// Upstream said keep-alive on the streaming response, so the connection may
        /// be pooled once the body's terminal chunk has been consumed.
        bool stream_keep_alive = false;

        /// Upstream conns only: when this connection entered the idle pool. Reaped
        /// after _pool_idle_ns; providers drop idle keep-alives on their own schedule,
        /// and a pooled corpse costs a retry to discover.
        int64_t ts_pooled = 0;

#ifdef LLMBRIDGE_HAVE_TLS
        /// Null = plaintext. The Session stays attached across keep-alive pool cycles,
        /// so a pooled reuse pays no second handshake.
        std::unique_ptr<net::tls::Session> tls;
        /// Ciphertext awaiting the socket; wbuf keeps the plaintext for stale-conn
        /// retry. `woff >= wbuf.size()` means fully encrypted, never fully sent; that
        /// is tls_out_off. GATEWAY-INTERNALS.md 10b.
        std::string tls_out;
        size_t tls_out_off = 0;
#endif
    };

    /// Connection's print method, found by ADL: `LB_INFO("closed ", *c)` renders
    /// `ClientConnection#42(fd=17,cid=2)`, so one grep separates the two halves of
    /// the proxy. Prints nothing from rbuf or wbuf: those hold the customer's
    /// request, including its credential.
    inline void log_put(net::log::Line& l, const Connection& c)
    {
        l.put(net::log::Id{c.is_client ? "ClientConnection" : "UpstreamConnection", c.log_inst});
        l.put("(fd=");
        l.put(static_cast<int64_t>(c.fd));
        if (c.is_client && c.id)
        {
            l.put(",cid=");
            l.put(c.id);
        }
        l.put(')');
    }

    /// A request is not an object with a lifetime, so it is named by the sequencer:
    /// `Request#123`, the same number the timing header carries.
    struct ReqId
    {
        uint64_t seq;
    };
    inline void log_put(net::log::Line& l, ReqId r)
    {
        l.put(net::log::Id{"Request", r.seq});
    }
} // namespace llmbridge
