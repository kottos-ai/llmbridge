// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "net/tls.hpp"

#ifdef LLMBRIDGE_HAVE_TLS

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace llmbridge::net::tls
{
    namespace
    {
        /// Drain OpenSSL's error queue into a string. Setup/teardown path only --
        /// never called on the steady-state hot path, where we only look at the
        /// SSL_get_error code.
        std::string drain_errors()
        {
            std::string out;
            char buf[256];
            while (const unsigned long e = ERR_get_error())
            {
                ERR_error_string_n(e, buf, sizeof buf);
                if (!out.empty()) out += "; ";
                out += buf;
            }
            return out.empty() ? std::string{"unknown OpenSSL error"} : out;
        }

        /// BIO_* and SSL_* take int lengths. Clamp instead of cast: a span past
        /// INT_MAX would otherwise go negative (error) or, past UINT_MAX, wrap to a
        /// small positive and make us report bytes consumed that never were.
        constexpr int clamp_len(size_t n) noexcept
        {
            constexpr size_t kMax = static_cast<size_t>(std::numeric_limits<int>::max());
            return static_cast<int>(std::min(n, kMax));
        }
    }  // namespace

    // ── Context ─────────────────────────────────────────────────────────────────

    Context::~Context()
    {
        if (_ctx) SSL_CTX_free(_ctx);
    }

    Context::Context(Context&& o) noexcept : _ctx(std::exchange(o._ctx, nullptr)), _err(std::move(o._err)) {}

    Context& Context::operator=(Context&& o) noexcept
    {
        if (this != &o)
        {
            if (_ctx) SSL_CTX_free(_ctx);
            _ctx = std::exchange(o._ctx, nullptr);
            _err = std::move(o._err);
        }
        return *this;
    }

    bool Context::init(const Options& opts) noexcept
    {
        _ctx = SSL_CTX_new(TLS_client_method());
        if (!_ctx)
        {
            _err = drain_errors();
            return false;
        }

        // 1.2 floor unconditionally; 1.3 when asked. Providers all speak 1.3, but a
        // 1.2 floor keeps corporate TLS-terminating proxies working.
        const int floor_ver = opts.require_tls13 ? TLS1_3_VERSION : TLS1_2_VERSION;
        if (SSL_CTX_set_min_proto_version(_ctx, floor_ver) != 1)
        {
            _err = drain_errors();
            return false;
        }

        // Verification is not optional. SSL_VERIFY_PEER makes a failed chain fail the
        // handshake instead of being reported after the fact -- there is no window in
        // which we could send an API key to an unverified peer.
        SSL_CTX_set_verify(_ctx, SSL_VERIFY_PEER, nullptr);

        const bool ok = opts.ca_file.empty()
                            ? SSL_CTX_set_default_verify_paths(_ctx) == 1
                            : SSL_CTX_load_verify_locations(_ctx, opts.ca_file.c_str(), nullptr) == 1;
        if (!ok)
        {
            _err = drain_errors();
            return false;
        }

        // Let OpenSSL retry a write with a moved buffer. Our plaintext spans are not
        // guaranteed to live at a stable address across a WANT_WRITE retry, and
        // without this OpenSSL treats that as a fatal API misuse.
        SSL_CTX_set_mode(_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);

        // Refuse server-initiated renegotiation (TLS 1.2 only; 1.3 removed it). We
        // never request it, and accepting it hands the peer a free CPU-amplification
        // lever -- each forced renegotiation costs us asymmetric crypto.
        SSL_CTX_set_options(_ctx, SSL_OP_NO_RENEGOTIATION);
        return true;
    }

    // ── Session ─────────────────────────────────────────────────────────────────

    Session::~Session()
    {
        // Frees the attached BIOs too -- SSL_set_bio transfers ownership, so
        // explicitly freeing _rbio/_wbio here would be a double free.
        if (_ssl) SSL_free(_ssl);
    }

    Session::Session(Session&& o) noexcept
        : _ssl(std::exchange(o._ssl, nullptr)),
          _rbio(std::exchange(o._rbio, nullptr)),
          _wbio(std::exchange(o._wbio, nullptr)),
          _want(o._want),
          _hs_done(o._hs_done),
          _err(std::move(o._err))
    {
    }

    Session& Session::operator=(Session&& o) noexcept
    {
        if (this != &o)
        {
            if (_ssl) SSL_free(_ssl);
            _ssl = std::exchange(o._ssl, nullptr);
            _rbio = std::exchange(o._rbio, nullptr);
            _wbio = std::exchange(o._wbio, nullptr);
            _want = o._want;
            _hs_done = o._hs_done;
            _err = std::move(o._err);
        }
        return *this;
    }

    bool Session::init_client(const Context& ctx, std::string_view host) noexcept
    {
        if (!ctx.ready()) return false;

        _ssl = SSL_new(ctx.native());
        if (!_ssl)
        {
            _err = drain_errors();
            return false;
        }

        _rbio = BIO_new(BIO_s_mem());
        _wbio = BIO_new(BIO_s_mem());
        if (!_rbio || !_wbio)
        {
            _err = drain_errors();
            if (_rbio) BIO_free(_rbio);
            if (_wbio) BIO_free(_wbio);
            _rbio = _wbio = nullptr;
            SSL_free(_ssl);
            _ssl = nullptr;
            return false;
        }

        // A memory BIO returns 0 at EOF by default, which SSL_read reads as "peer
        // closed" instead of "nothing yet". Without this the first read on an empty
        // BIO tears the connection down.
        BIO_set_mem_eof_return(_rbio, -1);
        BIO_set_mem_eof_return(_wbio, -1);

        SSL_set_bio(_ssl, _rbio, _wbio);  // takes ownership of both
        SSL_set_connect_state(_ssl);

        const std::string host_z{host};

        // SNI. Providers front many certs off one IP; without this we get whichever
        // default cert the edge serves and verification fails for the wrong reason.
        if (SSL_set_tlsext_host_name(_ssl, host_z.c_str()) != 1)
        {
            _err = drain_errors();
            SSL_free(_ssl);  // fail CLOSED: a live _ssl here could be handshaken
            _ssl = nullptr;  // without SNI/hostname checks by a caller that
            _rbio = _wbio = nullptr;  // ignored our return value
            return false;
        }

        // Hostname verification. SSL_VERIFY_PEER alone only proves the chain is
        // trusted -- NOT that it was issued for the host we dialled. Omitting this is
        // the classic "valid certificate, wrong server" hole.
        if (SSL_set1_host(_ssl, host_z.c_str()) != 1)
        {
            _err = drain_errors();
            SSL_free(_ssl);  // fail CLOSED -- same reasoning as above: without
            _ssl = nullptr;  // set1_host the handshake would verify the chain
            _rbio = _wbio = nullptr;  // but not WHO it was issued to
            return false;
        }

        return true;
    }

    Want Session::classify(int rc) noexcept
    {
        if (rc > 0)
        {
            _want = has_pending_output() ? Want::Write : Want::None;
            return _want;
        }

        switch (SSL_get_error(_ssl, rc))
        {
            case SSL_ERROR_WANT_READ:
                // Pending output still wins: OpenSSL may need us to flush a flight
                // before the peer can possibly answer. Reporting Read here would
                // deadlock the handshake.
                _want = has_pending_output() ? Want::Write : Want::Read;
                break;
            case SSL_ERROR_WANT_WRITE:
                _want = Want::Write;
                break;
            case SSL_ERROR_ZERO_RETURN:
                _want = Want::Closed;
                break;
            case SSL_ERROR_NONE:
                _want = Want::None;
                break;
            default:
                _err = drain_errors();
                _want = Want::Error;
                break;
        }
        return _want;
    }

    Want Session::start_handshake() noexcept
    {
        if (!_ssl) return Want::Error;
        ERR_clear_error();  // stale queue entries corrupt SSL_get_error's verdict
        const int rc = SSL_do_handshake(_ssl);
        if (rc == 1) _hs_done = true;
        return classify(rc);
    }

    size_t Session::feed_ciphertext(std::span<const uint8_t> in) noexcept
    {
        if (!_ssl || in.empty()) return 0;

        const int n = BIO_write(_rbio, in.data(), clamp_len(in.size()));
        if (n <= 0)
        {
            _err = "TLS read BIO refused input";
            _want = Want::Error;
            return 0;
        }

        // Feeding bytes may complete the handshake, so drive it here instead of
        // making the caller remember to. Once done, the caller's read_plaintext()
        // picks up application data from the same buffered bytes.
        if (!_hs_done)
        {
            ERR_clear_error();
            const int rc = SSL_do_handshake(_ssl);
            if (rc == 1) _hs_done = true;
            classify(rc);
        }
        return static_cast<size_t>(n);
    }

    size_t Session::pull_ciphertext(std::span<uint8_t> out) noexcept
    {
        if (!_ssl || out.empty()) return 0;
        const int n = BIO_read(_wbio, out.data(), clamp_len(out.size()));
        return n > 0 ? static_cast<size_t>(n) : 0;
    }

    size_t Session::read_plaintext(std::span<uint8_t> out) noexcept
    {
        if (!_ssl || !_hs_done || out.empty()) return 0;
        ERR_clear_error();
        const int n = SSL_read(_ssl, out.data(), clamp_len(out.size()));
        if (n > 0)
        {
            _want = has_pending_output() ? Want::Write : Want::None;
            return static_cast<size_t>(n);
        }
        classify(n);
        return 0;
    }

    size_t Session::write_plaintext(std::span<const uint8_t> in) noexcept
    {
        if (!_ssl || !_hs_done || in.empty()) return 0;
        ERR_clear_error();
        const int n = SSL_write(_ssl, in.data(), clamp_len(in.size()));
        if (n > 0)
        {
            _want = has_pending_output() ? Want::Write : Want::None;
            return static_cast<size_t>(n);
        }
        classify(n);
        return 0;
    }

    Want Session::shutdown() noexcept
    {
        if (!_ssl) return Want::Error;
        ERR_clear_error();
        const int rc = SSL_shutdown(_ssl);
        // 0 = our close_notify is out, peer's not seen yet. That is a normal
        // half-closed state, not an error; the caller flushes and may stop there.
        if (rc == 0)
        {
            _want = Want::Write;
            return _want;
        }
        if (rc == 1)
        {
            _want = has_pending_output() ? Want::Write : Want::Closed;
            return _want;
        }
        return classify(rc);
    }

    bool Session::has_pending_output() const noexcept
    {
        return _wbio && BIO_ctrl_pending(_wbio) > 0;
    }

}  // namespace llmbridge::net::tls

#endif  // LLMBRIDGE_HAVE_TLS
