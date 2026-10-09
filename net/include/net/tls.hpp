// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// TLS as a pure byte transform over memory BIOs: the loop owns the socket and OpenSSL never
// touches it. Why, and the call protocol (drain pull_ciphertext() after every call that
// advances the state machine): DESIGN.md "Why memory BIOs, not SSL_set_fd".

#pragma once

#ifdef LLMBRIDGE_HAVE_TLS

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Forward-declared so <openssl/ssl.h> stays out of every unit that includes the gateway.
extern "C"
{
    struct ssl_ctx_st;
    struct ssl_st;
    struct bio_st;
    struct bio_method_st;
}

namespace llmbridge::net::tls
{
    /// What a session wants next: arm a read, flush a write, or tear the connection down.
    enum class Want : uint8_t
    {
        None,    ///< nothing pending; safe to idle
        Read,    ///< needs more ciphertext from the peer -- arm a recv
        Write,   ///< has ciphertext to emit -- drain pull_ciphertext() and send
        Closed,  ///< clean shutdown (close_notify seen)
        Error,   ///< fatal; see last_error()
    };

    /// One SSL_CTX shared by every session. Verification has no off switch: skipping it is
    /// a credential-exfiltration bug. Tests with a self-signed peer pass their CA in `ca_file`.
    class Context
    {
      public:
        /// Outbound: gateway -> provider. We are the client and we verify them.
        struct ClientOptions
        {
            /// PEM bundle to trust; empty is the system store, which production wants.
            std::string ca_file{};
            /// The floor is TLS 1.2; set this to require 1.3.
            bool require_tls13{false};
        };

        /// Inbound: client -> gateway. A separate type so client options cannot reach a server.
        struct ServerOptions
        {
            /// PEM chain, leaf first: fullchain.pem, since cert.pem omits the intermediate.
            std::string cert_file{};
            /// PEM private key. Must be mode 600 or init_server() refuses it.
            std::string key_file{};
            bool require_tls13{false};
        };

        Context() noexcept = default;
        ~Context();
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        Context(Context&&) noexcept;
        Context& operator=(Context&&) noexcept;

        /// Build a client SSL_CTX, once at startup; false sets last_error().
        [[nodiscard]] bool init_client(const ClientOptions& opts) noexcept;

        /// Build the listener's SSL_CTX. An unreadable, mismatched or wrong-mode key, or an
        /// expired certificate, fails startup instead of every handshake afterwards.
        [[nodiscard]] bool init_server(const ServerOptions& opts) noexcept;

        [[nodiscard]] bool ready() const noexcept { return _ctx != nullptr; }
        [[nodiscard]] const std::string& last_error() const noexcept { return _err; }

        /// Raw handle, for Session construction. Non-owning.
        [[nodiscard]] ssl_ctx_st* native() const noexcept { return _ctx; }

      private:
        ssl_ctx_st* _ctx{nullptr};
        std::string _err{};
    };

    /// One connection's SSL object and BIO pair. One `host` sets both SNI and hostname
    /// verification, so the two cannot disagree.
    class Session
    {
      public:
        Session() noexcept = default;
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&) noexcept;
        Session& operator=(Session&&) noexcept;

        /// Attach as a client; `host` must be a DNS name for verification to mean anything.
        [[nodiscard]] bool init_client(const Context& ctx, std::string_view host) noexcept;

        /// Attach as a server: no host, since a server receives SNI and verifies no peer.
        [[nodiscard]] bool init_server(const Context& ctx) noexcept;

        /// Begin the handshake. After this, drain pull_ciphertext() and send it.
        Want start_handshake() noexcept;

        [[nodiscard]] bool handshake_done() const noexcept { return _hs_done; }

        /// Ciphertext from the socket; returns bytes consumed (all, unless the BIO is wedged).
        [[nodiscard]] size_t feed_ciphertext(std::span<const uint8_t> in) noexcept;

        /// Ciphertext to send, written into `out`; call until it returns 0.
        [[nodiscard]] size_t pull_ciphertext(std::span<uint8_t> out) noexcept;

        /// Decrypted data into `out`; 0 is either "nothing yet" or EOF, which want() tells apart.
        [[nodiscard]] size_t read_plaintext(std::span<uint8_t> out) noexcept;

        /// Encrypt from `in`; a short count means drain pull_ciphertext(), then retry the rest.
        [[nodiscard]] size_t write_plaintext(std::span<const uint8_t> in) noexcept;

        /// Begin a clean shutdown (emits close_notify into the write BIO).
        Want shutdown() noexcept;

        [[nodiscard]] Want want() const noexcept { return _want; }
        [[nodiscard]] bool has_pending_output() const noexcept;
        /// Ciphertext bytes staged in the write BIO, for bounding a buffer.
        [[nodiscard]] size_t pending_output_bytes() const noexcept;
        void set_sink(std::string* out) noexcept { _sink = out; }
        [[nodiscard]] const std::string& last_error() const noexcept { return _err; }

      private:
        /// Create the SSL object and BIO pair; the role and peer checks stay in the callers.
        [[nodiscard]] bool attach(const Context& ctx) noexcept;

        /// An OpenSSL return code as Want, in one place so every call classifies alike.
        Want classify(int rc) noexcept;
        static int bio_write(bio_st* b, const char* d, int n) noexcept;
        static long bio_ctrl(bio_st* b, int cmd, long larg, void* parg) noexcept;
        static const bio_method_st* sink_method() noexcept;

        ssl_st* _ssl{nullptr};
        bio_st* _rbio{nullptr};  ///< ciphertext in  (we write, OpenSSL reads)
        bio_st* _wbio{nullptr};  ///< ciphertext out: bio_write() into _sink or _staging
        Want _want{Want::None};
        bool _hs_done{false};
        std::string _err{};
        std::string* _sink{nullptr};
        std::string _staging{};
    };

}  // namespace llmbridge::net::tls

#endif  // LLMBRIDGE_HAVE_TLS
