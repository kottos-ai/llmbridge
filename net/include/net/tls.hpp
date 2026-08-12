// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// TLS for outbound (gateway -> provider) connections, driven through MEMORY BIOs.
//
// WHY MEMORY BIOs AND NOT SSL_set_fd
// ----------------------------------
// The obvious approach is to hand OpenSSL the socket and call SSL_read/SSL_write.
// That cannot work here, because it assumes whoever owns the fd also owns the
// syscalls -- and on the io_uring backend we do not. io_uring hands us bytes that
// have ALREADY been read (multishot recv into provided buffers); there is no read
// for OpenSSL to perform. Giving OpenSSL the fd would mean it issuing its own
// blocking-ish recv() behind the loop's back, defeating the completion model and
// bypassing the registered-buffer path.
//
// So the event loop keeps the socket, and OpenSSL never touches it. The SSL object
// gets a pair of memory BIOs and becomes a pure byte transform:
//
//     socket --recv--> feed_ciphertext()  -> [rbio] -> SSL -> read_plaintext()
//     socket <-send--  pull_ciphertext()  <- [wbio] <- SSL <- write_plaintext()
//
// This is backend-agnostic: epoll and io_uring feed the same four calls, and the
// handshake, renegotiation and close_notify all fall out of the same pump. The cost
// is one extra copy through the BIO pair, which is the price of not caring how the
// bytes arrived.
//
// USAGE (identical on both backends)
//   1. start_handshake(), then loop: pull_ciphertext() -> send; recv -> feed_ciphertext()
//      until handshake_done().
//   2. Steady state: write_plaintext() then pull_ciphertext() -> send;
//      recv -> feed_ciphertext() then read_plaintext().
//   3. ALWAYS drain pull_ciphertext() after any call that advances the state machine --
//      OpenSSL can emit protocol bytes (key updates, alerts, close_notify) with no
//      application data involved. Forgetting this is the classic memory-BIO stall.
//
// Nothing here allocates on the steady-state path beyond OpenSSL's own internals;
// the BIO pair is created once per connection at construction.

#pragma once

#ifdef LLMBRIDGE_HAVE_TLS

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Forward-declared so this header does not drag <openssl/ssl.h> into every
// translation unit that includes the gateway. Only tls.cpp needs the real types.
extern "C"
{
    struct ssl_ctx_st;
    struct ssl_st;
    struct bio_st;
}

namespace llmbridge::net::tls
{
    /// What a session wants next. The loop uses this to decide whether to arm a
    /// read, flush a write, or tear the connection down.
    enum class Want : uint8_t
    {
        None,    ///< nothing pending; safe to idle
        Read,    ///< needs more ciphertext from the peer -- arm a recv
        Write,   ///< has ciphertext to emit -- drain pull_ciphertext() and send
        Closed,  ///< clean shutdown (close_notify seen)
        Error,   ///< fatal; see last_error()
    };

    /// Process-wide TLS configuration: one SSL_CTX shared by every session.
    ///
    /// Verification is ON by default and there is deliberately no "disable" knob in
    /// the constructor -- a gateway that silently skips certificate checks is a
    /// credential-exfiltration bug, not a convenience. Tests that need a self-signed
    /// peer pass their own CA via `ca_file`.
    class Context
    {
      public:
        /// Outbound: gateway -> provider. We are the client and we verify them.
        struct ClientOptions
        {
            /// PEM bundle to trust. Empty = the system default store, which is what
            /// production wants (Ubuntu: /etc/ssl/certs/ca-certificates.crt).
            std::string ca_file{};
            /// Minimum protocol version. 1.2 floor: every provider we target speaks
            /// 1.3, and 1.0/1.1 are deprecated.
            bool require_tls13{false};
        };

        /// Inbound: client -> gateway. We are the server and we PROVE identity
        /// instead of checking it. These are separate structs on purpose: passing
        /// client options to a server context, or the reverse, should not compile.
        struct ServerOptions
        {
            /// PEM certificate CHAIN (leaf first, then intermediates). Let's
            /// Encrypt calls this fullchain.pem. Using cert.pem instead omits the
            /// intermediate and clients that do not fetch it will fail to verify.
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

        /// Build a CLIENT SSL_CTX. Returns false and sets last_error() on failure.
        /// Call once at startup -- this is a setup path, so it may allocate freely.
        [[nodiscard]] bool init_client(const ClientOptions& opts) noexcept;

        /// Build a SERVER SSL_CTX for the inbound listener.
        ///
        /// Every failure here is a startup failure, deliberately: an unreadable
        /// certificate, a key the wrong mode, a key that does not match the
        /// certificate and an already-expired certificate all refuse to build a
        /// context. The alternative is a process that starts happily and then fails
        /// every handshake, which is far harder to diagnose from the client side.
        [[nodiscard]] bool init_server(const ServerOptions& opts) noexcept;

        [[nodiscard]] bool ready() const noexcept { return _ctx != nullptr; }
        [[nodiscard]] const std::string& last_error() const noexcept { return _err; }

        /// Raw handle, for Session construction. Non-owning.
        [[nodiscard]] ssl_ctx_st* native() const noexcept { return _ctx; }

      private:
        ssl_ctx_st* _ctx{nullptr};
        std::string _err{};
    };

    /// One TLS connection's worth of state. Owns an SSL object and its BIO pair.
    ///
    /// `host` is used for BOTH the SNI extension and hostname verification -- they
    /// must agree, and taking one argument makes it impossible to set one and forget
    /// the other (a real and quiet way to end up with a valid cert for the wrong
    /// host).
    class Session
    {
      public:
        Session() noexcept = default;
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&) noexcept;
        Session& operator=(Session&&) noexcept;

        /// Attach to a context as a CLIENT (gateway -> provider). Returns false on
        /// failure. `host` must be the DNS name, not an IP literal, for verification
        /// to mean anything.
        [[nodiscard]] bool init_client(const Context& ctx, std::string_view host) noexcept;

        /// Attach to a context as a SERVER (client -> gateway). Takes no host: a
        /// server RECEIVES the SNI name and does not verify a peer, so there is
        /// nothing to check a hostname against. The context supplies the identity.
        [[nodiscard]] bool init_server(const Context& ctx) noexcept;

        /// Begin the handshake. After this, drain pull_ciphertext() and send it.
        Want start_handshake() noexcept;

        [[nodiscard]] bool handshake_done() const noexcept { return _hs_done; }

        /// Feed ciphertext that arrived from the socket. Returns bytes consumed
        /// (always all of `in` unless the BIO is wedged, which is fatal).
        [[nodiscard]] size_t feed_ciphertext(std::span<const uint8_t> in) noexcept;

        /// Pull ciphertext that must be written to the socket. Returns bytes written
        /// into `out`; call until it returns 0.
        [[nodiscard]] size_t pull_ciphertext(std::span<uint8_t> out) noexcept;

        /// Decrypt application data. Returns bytes written into `out`; 0 means
        /// "nothing available right now" -- check want() to distinguish from EOF.
        [[nodiscard]] size_t read_plaintext(std::span<uint8_t> out) noexcept;

        /// Encrypt application data. Returns bytes consumed from `in`; a short
        /// count means the SSL object is back-pressured and the caller must drain
        /// pull_ciphertext() before retrying the remainder.
        [[nodiscard]] size_t write_plaintext(std::span<const uint8_t> in) noexcept;

        /// Begin a clean shutdown (emits close_notify into the write BIO).
        Want shutdown() noexcept;

        [[nodiscard]] Want want() const noexcept { return _want; }
        [[nodiscard]] bool has_pending_output() const noexcept;
        /// Bytes of ciphertext staged in the write BIO. Measurement seam: the
        /// bool above answers "any?", and bounding a buffer needs "how much?".
        [[nodiscard]] size_t pending_output_bytes() const noexcept;
        [[nodiscard]] const std::string& last_error() const noexcept { return _err; }

      private:
        /// Allocate the SSL object and its memory BIO pair. Shared by both
        /// directions; the handshake role and the peer checks stay in the callers.
        [[nodiscard]] bool attach(const Context& ctx) noexcept;

        /// Translate an OpenSSL return code into Want, recording errors. Central so
        /// every entry point classifies WANT_READ/WANT_WRITE identically.
        Want classify(int rc) noexcept;

        ssl_st* _ssl{nullptr};
        bio_st* _rbio{nullptr};  ///< ciphertext IN  (we write, OpenSSL reads)
        bio_st* _wbio{nullptr};  ///< ciphertext OUT (OpenSSL writes, we read)
        Want _want{Want::None};
        bool _hs_done{false};
        std::string _err{};
    };

}  // namespace llmbridge::net::tls

#endif  // LLMBRIDGE_HAVE_TLS
