// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// TLS memory-BIO pump tests.
//
// These are hermetic: no sockets, no network, no live provider. A self-signed cert
// is generated in-process and a server-side SSL object is driven through its own BIO
// pair, so the two halves talk to each other purely by us shuttling byte buffers
// between them -- which is exactly what the event loop will do. If the pump is wrong
// here it is wrong in the gateway.

#include "net/tls.hpp"

#ifdef LLMBRIDGE_HAVE_TLS

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace net::tls;

namespace
{
    constexpr const char* kHost = "provider.test";

    /// A self-signed cert for kHost, with the name in a SAN (modern verifiers ignore
    /// CN entirely, so a CN-only cert would fail for a reason unrelated to the pump).
    struct SelfSigned
    {
        EVP_PKEY* key{nullptr};
        X509* crt{nullptr};

        SelfSigned()
        {
            key = EVP_RSA_gen(2048);
            crt = X509_new();
            ASN1_INTEGER_set(X509_get_serialNumber(crt), 1);
            X509_gmtime_adj(X509_getm_notBefore(crt), 0);
            X509_gmtime_adj(X509_getm_notAfter(crt), 3600);
            X509_set_pubkey(crt, key);

            X509_NAME* nm = X509_get_subject_name(crt);
            X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>(kHost), -1, -1, 0);
            X509_set_issuer_name(crt, nm);  // self-signed

            const std::string san = std::string("DNS:") + kHost;
            X509_EXTENSION* ext =
                X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str());
            X509_add_ext(crt, ext, -1);
            X509_EXTENSION_free(ext);

            X509_sign(crt, key, EVP_sha256());
        }

        ~SelfSigned()
        {
            if (crt) X509_free(crt);
            if (key) EVP_PKEY_free(key);
        }

        /// Write the cert to a temp PEM so the client Context can trust it as a CA.
        std::string write_pem() const
        {
            std::string path = std::string(::testing::TempDir()) + "llmbridge_tls_test_ca.pem";
            FILE* f = std::fopen(path.c_str(), "wb");
            EXPECT_NE(f, nullptr);
            PEM_write_X509(f, crt);
            std::fclose(f);
            return path;
        }
    };

    /// Minimal server side, also on memory BIOs. Mirrors the client's four calls.
    struct Server
    {
        SSL_CTX* ctx{nullptr};
        SSL* ssl{nullptr};
        BIO* rbio{nullptr};
        BIO* wbio{nullptr};

        Server(const SelfSigned& id)
        {
            ctx = SSL_CTX_new(TLS_server_method());
            SSL_CTX_use_certificate(ctx, id.crt);
            SSL_CTX_use_PrivateKey(ctx, id.key);
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

            ssl = SSL_new(ctx);
            rbio = BIO_new(BIO_s_mem());
            wbio = BIO_new(BIO_s_mem());
            BIO_set_mem_eof_return(rbio, -1);
            BIO_set_mem_eof_return(wbio, -1);
            SSL_set_bio(ssl, rbio, wbio);
            SSL_set_accept_state(ssl);
        }

        ~Server()
        {
            if (ssl) SSL_free(ssl);
            if (ctx) SSL_CTX_free(ctx);
        }

        void feed(const std::vector<uint8_t>& b)
        {
            if (!b.empty()) BIO_write(rbio, b.data(), static_cast<int>(b.size()));
        }

        std::vector<uint8_t> pull()
        {
            std::vector<uint8_t> out;
            uint8_t buf[4096];
            int n;
            while ((n = BIO_read(wbio, buf, sizeof buf)) > 0) out.insert(out.end(), buf, buf + n);
            return out;
        }

        void pump() { SSL_do_handshake(ssl); }
    };

    /// Shuttle bytes both ways until the client handshake completes or we give up.
    /// The loop cap is a deadlock guard: a pump bug shows up as "never converges",
    /// and an infinite loop in CI is far worse than a failed assertion.
    bool drive_handshake(Session& client, Server& server, int max_rounds = 16)
    {
        std::vector<uint8_t> buf(16384);
        for (int i = 0; i < max_rounds && !client.handshake_done(); ++i)
        {
            std::vector<uint8_t> c2s;
            size_t n;
            while ((n = client.pull_ciphertext(buf)) > 0) c2s.insert(c2s.end(), buf.data(), buf.data() + n);
            server.feed(c2s);
            server.pump();

            const std::vector<uint8_t> s2c = server.pull();
            if (!s2c.empty()) (void)client.feed_ciphertext(s2c);
            if (c2s.empty() && s2c.empty()) break;  // no progress -- stuck
        }
        if (!client.handshake_done()) return false;

        // In TLS 1.3 the CLIENT is done a flight before the server is: it sends
        // Finished and considers the handshake complete without waiting for a reply.
        // So client.handshake_done() does not imply the server can write yet -- its
        // SSL_write returns -1 until it has consumed that last flight. Flush it here,
        // otherwise every test that has the server speak first fails for a reason
        // that has nothing to do with the code under test.
        std::vector<uint8_t> tail;
        size_t n;
        while ((n = client.pull_ciphertext(buf)) > 0) tail.insert(tail.end(), buf.data(), buf.data() + n);
        if (!tail.empty())
        {
            server.feed(tail);
            server.pump();
        }
        return SSL_is_init_finished(server.ssl) == 1;
    }
}  // namespace

class TlsPump : public ::testing::Test
{
  protected:
    SelfSigned id;
    Context ctx;

    void SetUp() override
    {
        Context::Options o;
        o.ca_file = id.write_pem();
        ASSERT_TRUE(ctx.init(o)) << ctx.last_error();
    }
};

TEST_F(TlsPump, HandshakeCompletesThroughMemoryBios)
{
    Session c;
    ASSERT_TRUE(c.init_client(ctx, kHost)) << c.last_error();

    Server s(id);
    EXPECT_EQ(c.start_handshake(), Want::Write);  // ClientHello must be pending
    EXPECT_TRUE(c.has_pending_output());

    ASSERT_TRUE(drive_handshake(c, s)) << c.last_error();
    EXPECT_TRUE(c.handshake_done());
    EXPECT_NE(c.want(), Want::Error);
}

TEST_F(TlsPump, ApplicationDataRoundTrips)
{
    Session c;
    ASSERT_TRUE(c.init_client(ctx, kHost));
    Server s(id);
    (void)c.start_handshake();
    ASSERT_TRUE(drive_handshake(c, s)) << c.last_error();

    // Client -> server, the shape a real request takes.
    const std::string req = "POST /v1/messages HTTP/1.1\r\nhost: provider.test\r\n\r\n";
    const auto* p = reinterpret_cast<const uint8_t*>(req.data());
    ASSERT_EQ(c.write_plaintext({p, req.size()}), req.size());

    std::vector<uint8_t> buf(16384), wire;
    size_t n;
    while ((n = c.pull_ciphertext(buf)) > 0) wire.insert(wire.end(), buf.data(), buf.data() + n);
    ASSERT_FALSE(wire.empty());
    s.feed(wire);

    char got[512];
    const int got_n = SSL_read(s.ssl, got, sizeof got);
    ASSERT_GT(got_n, 0);
    EXPECT_EQ(std::string(got, static_cast<size_t>(got_n)), req);

    // Server -> client, an SSE-ish reply.
    const std::string resp = "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\n\r\n";
    ASSERT_GT(SSL_write(s.ssl, resp.data(), static_cast<int>(resp.size())), 0);
    (void)c.feed_ciphertext(s.pull());

    const size_t rn = c.read_plaintext(buf);
    ASSERT_GT(rn, 0u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf.data()), rn), resp);
}

// The point of the memory-BIO design is that it cannot care how the bytes were
// framed by the transport. io_uring will hand us arbitrary fragments, so prove a
// byte-at-a-time feed produces the same result as one bulk feed.
TEST_F(TlsPump, ToleratesArbitraryCiphertextFragmentation)
{
    Session c;
    ASSERT_TRUE(c.init_client(ctx, kHost));
    Server s(id);
    (void)c.start_handshake();
    ASSERT_TRUE(drive_handshake(c, s)) << c.last_error();

    const std::string msg = "data: {\"delta\":\"hello\"}\n\n";
    ASSERT_GT(SSL_write(s.ssl, msg.data(), static_cast<int>(msg.size())), 0);

    const std::vector<uint8_t> wire = s.pull();
    ASSERT_FALSE(wire.empty());
    for (uint8_t b : wire)  // one byte per "recv completion"
    {
        ASSERT_EQ(c.feed_ciphertext({&b, 1}), 1u);
        EXPECT_NE(c.want(), Want::Error) << c.last_error();
    }

    std::vector<uint8_t> buf(4096);
    const size_t rn = c.read_plaintext(buf);
    ASSERT_EQ(rn, msg.size());
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf.data()), rn), msg);
}

// Verification must actually be enforced. A context that trusts only the system
// store must reject our self-signed peer -- if this passes, every other test in the
// file is meaningless because nothing is being checked.
TEST_F(TlsPump, RejectsUntrustedCertificate)
{
    Context strict;
    ASSERT_TRUE(strict.init(Context::Options{}));  // system store only

    Session c;
    ASSERT_TRUE(c.init_client(strict, kHost));
    Server s(id);
    (void)c.start_handshake();

    EXPECT_FALSE(drive_handshake(c, s));
    EXPECT_FALSE(c.handshake_done());
    EXPECT_EQ(c.want(), Want::Error);
}

// Same idea for the hostname: a chain we trust, presented for the wrong name, must
// still fail. This is the hole SSL_VERIFY_PEER alone leaves open.
TEST_F(TlsPump, RejectsHostnameMismatch)
{
    Session c;
    ASSERT_TRUE(c.init_client(ctx, "wrong.test"));  // cert is for provider.test
    Server s(id);
    (void)c.start_handshake();

    EXPECT_FALSE(drive_handshake(c, s));
    EXPECT_EQ(c.want(), Want::Error);
}

#endif  // LLMBRIDGE_HAVE_TLS
