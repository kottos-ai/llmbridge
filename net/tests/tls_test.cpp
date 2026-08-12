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

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace llmbridge::net::tls;

namespace
{
    constexpr const char* kHost = "provider.test";

    /// A self-signed cert for kHost, with the name in a SAN (modern verifiers ignore
    /// CN entirely, so a CN-only cert would fail for a reason unrelated to the pump).
    struct SelfSigned
    {
        EVP_PKEY* key{nullptr};
        X509* crt{nullptr};

        /// `lifetime_s` is seconds from now. Negative produces an ALREADY EXPIRED
        /// certificate, which init_server() must refuse at startup.
        explicit SelfSigned(long lifetime_s = 3600)
        {
            key = EVP_RSA_gen(2048);
            crt = X509_new();
            ASN1_INTEGER_set(X509_get_serialNumber(crt), 1);
            X509_gmtime_adj(X509_getm_notBefore(crt), lifetime_s < 0 ? lifetime_s * 2 : 0);
            X509_gmtime_adj(X509_getm_notAfter(crt), lifetime_s);
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
            // Unique per process AND per call. ctest gives each gtest case its own
            // process and runs them with -j, so a fixed name meant one process
            // truncated the file another was mid-way through loading, surfacing as a
            // spurious TLS failure in whichever lost the race.
            //
            // This is the SAME bug, in the SAME copy-pasted SelfSigned helper, as the
            // one fixed in gateway/tests/gateway_tls_test.cpp, and it was left behind
            // when that one was fixed. Duplicated code, one-sided fix: exactly the
            // failure mode the ep_/ur_ audit was about. If a third copy ever appears,
            // hoist this helper into a shared test header instead.
            static std::atomic<unsigned> seq{0};
            std::string path = std::string(::testing::TempDir()) + "llmbridge_tls_test_ca_" +
                               std::to_string(static_cast<long>(::getpid())) + "_" +
                               std::to_string(seq.fetch_add(1, std::memory_order_relaxed)) + ".pem";
            FILE* f = std::fopen(path.c_str(), "wb");
            EXPECT_NE(f, nullptr);
            PEM_write_X509(f, crt);
            std::fclose(f);
            return path;
        }

        /// Write the private key to a temp PEM, mode 600 by default. `mode` exists
        /// so a test can produce a deliberately world-readable key and check that
        /// init_server() refuses it.
        std::string write_key_pem(mode_t mode = 0600) const
        {
            static std::atomic<unsigned> seq{0};
            std::string path = std::string(::testing::TempDir()) + "llmbridge_tls_test_key_" +
                               std::to_string(static_cast<long>(::getpid())) + "_" +
                               std::to_string(seq.fetch_add(1, std::memory_order_relaxed)) + ".pem";
            FILE* f = std::fopen(path.c_str(), "wb");
            EXPECT_NE(f, nullptr);
            PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
            std::fclose(f);
            EXPECT_EQ(::chmod(path.c_str(), mode), 0);
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

    /// Feed the server one byte at a time, pumping after each, so BOTH directions
    /// of the handshake see worst-case fragmentation.
    void server_feed_bytewise(Server& s, const std::vector<uint8_t>& bytes)
    {
        for (uint8_t b : bytes)
        {
            BIO_write(s.rbio, &b, 1);
            s.pump();
        }
    }

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
        Context::ClientOptions o;
        o.ca_file = id.write_pem();
        ASSERT_TRUE(ctx.init_client(o)) << ctx.last_error();
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
    ASSERT_TRUE(strict.init_client(Context::ClientOptions{}));  // system store only

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

// Handshake bytes are as fragmentable as application bytes -- the ClientHello can
// arrive split across recv completions too. Same property, other phase.
TEST_F(TlsPump, ToleratesHandshakeFragmentation)
{
    Session c;
    ASSERT_TRUE(c.init_client(ctx, kHost));
    Server s(id);
    (void)c.start_handshake();

    std::vector<uint8_t> buf(16384);
    for (int round = 0; round < 16 && !c.handshake_done(); ++round)
    {
        std::vector<uint8_t> c2s;
        size_t n;
        while ((n = c.pull_ciphertext(buf)) > 0) c2s.insert(c2s.end(), buf.data(), buf.data() + n);
        server_feed_bytewise(s, c2s);

        for (uint8_t b : s.pull())  // server flight, one byte at a time
        {
            ASSERT_EQ(c.feed_ciphertext({&b, 1}), 1u);
            ASSERT_NE(c.want(), Want::Error) << c.last_error();
        }
    }
    EXPECT_TRUE(c.handshake_done()) << c.last_error();
}

// A clean close_notify from the peer must surface as Want::Closed -- this is the
// only test that exercises the SSL_ERROR_ZERO_RETURN branch of classify(), and the
// gateway relies on it to tell "stream finished" from "stream died".
TEST_F(TlsPump, PeerCloseNotifySurfacesAsClosed)
{
    Session c;
    ASSERT_TRUE(c.init_client(ctx, kHost));
    Server s(id);
    (void)c.start_handshake();
    ASSERT_TRUE(drive_handshake(c, s)) << c.last_error();

    SSL_shutdown(s.ssl);              // server sends close_notify
    (void)c.feed_ciphertext(s.pull());

    std::vector<uint8_t> buf(256);
    EXPECT_EQ(c.read_plaintext(buf), 0u);
    EXPECT_EQ(c.want(), Want::Closed);
}

// The inverse property: the ABSENCE of close_notify must NOT read as a clean
// close. A TCP FIN with no close_notify is a truncation attack (strip the end of
// a response, e.g. the SSE final-usage chunk); the session must stay un-Closed so
// the gateway can flag the upstream response as truncated.
TEST_F(TlsPump, TruncationWithoutCloseNotifyIsNotClosed)
{
    Session c;
    ASSERT_TRUE(c.init_client(ctx, kHost));
    Server s(id);
    (void)c.start_handshake();
    ASSERT_TRUE(drive_handshake(c, s)) << c.last_error();

    // Peer "closes" the TCP stream without a close_notify: nothing more arrives.
    std::vector<uint8_t> buf(256);
    EXPECT_EQ(c.read_plaintext(buf), 0u);
    EXPECT_NE(c.want(), Want::Closed);   // must look like "waiting", never "done"
    EXPECT_EQ(c.want(), Want::Read);
}

// Corrupt ciphertext must classify as Want::Error (SSL_ERROR_SSL), not crash, not
// hang, not decrypt. This is the branch a malicious or broken middlebox hits.
TEST_F(TlsPump, GarbageCiphertextIsFatal)
{
    Session c;
    ASSERT_TRUE(c.init_client(ctx, kHost));
    Server s(id);
    (void)c.start_handshake();
    ASSERT_TRUE(drive_handshake(c, s)) << c.last_error();

    std::vector<uint8_t> junk(512);
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<uint8_t>(i * 7 + 13);
    (void)c.feed_ciphertext(junk);

    std::vector<uint8_t> buf(4096);
    EXPECT_EQ(c.read_plaintext(buf), 0u);
    EXPECT_EQ(c.want(), Want::Error);
    EXPECT_FALSE(c.last_error().empty());
}

// A payload over the 16 KB TLS record limit forces multiple records each way.
// Catches any assumption that one write_plaintext() == one pull_ciphertext() == one
// record -- an assumption the fragmentation tests cannot catch from the other side.
TEST_F(TlsPump, MultiRecordPayloadRoundTrips)
{
    Session c;
    ASSERT_TRUE(c.init_client(ctx, kHost));
    Server s(id);
    (void)c.start_handshake();
    ASSERT_TRUE(drive_handshake(c, s)) << c.last_error();

    std::string big(70 * 1024, '\0');  // > 4 records
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);

    // Client -> server. write_plaintext may take it in gulps; loop until consumed.
    size_t off = 0;
    std::vector<uint8_t> buf(32 * 1024), wire;
    while (off < big.size())
    {
        const auto* p = reinterpret_cast<const uint8_t*>(big.data()) + off;
        const size_t w = c.write_plaintext({p, big.size() - off});
        ASSERT_GT(w, 0u) << c.last_error();
        off += w;
        size_t n;
        while ((n = c.pull_ciphertext(buf)) > 0) wire.insert(wire.end(), buf.data(), buf.data() + n);
    }
    s.feed(wire);

    std::string got;
    char rb[16384];
    int rn;
    while ((rn = SSL_read(s.ssl, rb, sizeof rb)) > 0) got.append(rb, static_cast<size_t>(rn));
    EXPECT_EQ(got, big);
}

#endif  // LLMBRIDGE_HAVE_TLS

// ── Task 2: server-side context and session ─────────────────────────────────
//
// Inbound TLS makes llmbridge the SERVER for the first time. Every failure below
// is a startup failure on purpose: a process that starts happily and then fails
// every handshake gives the client an opaque alert and gives the operator
// nothing. These tests pin that behaviour so a later "cleanup" cannot soften it
// into a warning.

TEST(ServerContext, ValidCertAndKeyInitialises)
{
    SelfSigned ca;
    Context ctx;
    Context::ServerOptions o;
    o.cert_file = ca.write_pem();
    o.key_file = ca.write_key_pem();
    ASSERT_TRUE(ctx.init_server(o)) << ctx.last_error();
    EXPECT_TRUE(ctx.ready());
}

TEST(ServerContext, RefusesWorldReadablePrivateKey)
{
    SelfSigned ca;
    Context ctx;
    Context::ServerOptions o;
    o.cert_file = ca.write_pem();
    o.key_file = ca.write_key_pem(0644);  // the mistake this exists to catch
    EXPECT_FALSE(ctx.init_server(o));
    EXPECT_NE(ctx.last_error().find("readable beyond its owner"), std::string::npos)
        << ctx.last_error();
}

TEST(ServerContext, RefusesKeyThatDoesNotMatchTheCert)
{
    SelfSigned a, b;  // two independent keypairs
    Context ctx;
    Context::ServerOptions o;
    o.cert_file = a.write_pem();
    o.key_file = b.write_key_pem();  // the stale-renewal / swapped-file case
    EXPECT_FALSE(ctx.init_server(o));
    // Assert WHICH guard fired, not just that one did. Measured 2026-08-10:
    // SSL_CTX_use_PrivateKey_file rejects it, because the cert is already loaded
    // by then. Deleting SSL_CTX_check_private_key leaves this test passing, so a
    // bare EXPECT_FALSE would have been decoration.
    EXPECT_NE(ctx.last_error().find("mismatch"), std::string::npos) << ctx.last_error();
}

TEST(ServerContext, RefusesExpiredCertificate)
{
    SelfSigned expired(-3600);  // notAfter an hour in the past
    Context ctx;
    Context::ServerOptions o;
    o.cert_file = expired.write_pem();
    o.key_file = expired.write_key_pem();
    EXPECT_FALSE(ctx.init_server(o));
    EXPECT_NE(ctx.last_error().find("expired"), std::string::npos) << ctx.last_error();
}

TEST(ServerContext, RefusesMissingFiles)
{
    Context ctx;
    Context::ServerOptions o;
    o.cert_file = "/nonexistent/cert.pem";
    o.key_file = "/nonexistent/key.pem";
    EXPECT_FALSE(ctx.init_server(o));
}

TEST(ServerContext, RefusesEmptyPaths)
{
    Context ctx;
    EXPECT_FALSE(ctx.init_server(Context::ServerOptions{}));
    EXPECT_NE(ctx.last_error().find("needs both"), std::string::npos) << ctx.last_error();
}

// The point of the whole exercise: our own client Session and our own server
// Session complete a handshake against each other through nothing but the four
// memory-BIO calls. If this passes, the claim that Session is direction-agnostic
// is demonstrated instead of asserted.
TEST(ServerSession, OurClientAndOurServerCompleteAHandshake)
{
    SelfSigned ca;
    const std::string cert = ca.write_pem();
    const std::string key = ca.write_key_pem();

    Context sctx;
    Context::ServerOptions so;
    so.cert_file = cert;
    so.key_file = key;
    ASSERT_TRUE(sctx.init_server(so)) << sctx.last_error();

    Context cctx;
    Context::ClientOptions co;
    co.ca_file = cert;  // trust our own self-signed leaf
    ASSERT_TRUE(cctx.init_client(co)) << cctx.last_error();

    Session client, server;
    ASSERT_TRUE(client.init_client(cctx, kHost)) << client.last_error();
    ASSERT_TRUE(server.init_server(sctx)) << server.last_error();

    (void)client.start_handshake();

    uint8_t buf[8192];
    bool done = false;
    for (int round = 0; round < 24 && !done; ++round)
    {
        for (;;)  // client -> server
        {
            const size_t n = client.pull_ciphertext(buf);
            if (n == 0) break;
            ASSERT_EQ(server.feed_ciphertext({buf, n}), n);
        }
        (void)server.start_handshake();
        for (;;)  // server -> client
        {
            const size_t n = server.pull_ciphertext(buf);
            if (n == 0) break;
            ASSERT_EQ(client.feed_ciphertext({buf, n}), n);
        }
        (void)client.start_handshake();
        done = client.handshake_done() && server.handshake_done();
    }
    ASSERT_TRUE(done) << "client: " << client.last_error() << " server: " << server.last_error();

    // Application data both ways, which is what the gateway actually needs.
    const std::string req = "GET /v1/models HTTP/1.1\r\n\r\n";
    ASSERT_EQ(client.write_plaintext({reinterpret_cast<const uint8_t*>(req.data()), req.size()}),
              req.size());
    for (;;)
    {
        const size_t n = client.pull_ciphertext(buf);
        if (n == 0) break;
        ASSERT_EQ(server.feed_ciphertext({buf, n}), n);
    }
    const size_t got = server.read_plaintext(buf);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), got), req);
}

// A handshake that never completes must not let a peer buffer without bound.
// Measured 2026-08-12 before writing any cap, because the cap would otherwise be a
// fix for a condition nobody had shown occurs: OpenSSL's record layer already
// bounds it at ONE record. Two shapes, both refused at ~16 KiB.
//
// This test exists to keep that true. It fails if a future change adds buffering of
// our own ahead of the Session, which is the only way the bound could be lost.
namespace
{
    // A server Session that has been handed `hdr` + dribbled bytes, never a valid
    // handshake. Returns total bytes accepted before it refuses.
    size_t bytes_accepted_before_refusal(Session& server, const uint8_t* hdr, size_t hdr_len)
    {
        size_t fed = server.feed_ciphertext({hdr, hdr_len});
        (void)server.start_handshake();
        const uint8_t one = 'A';
        for (size_t i = 0; i < 5u * 1024 * 1024; ++i)
        {
            fed += server.feed_ciphertext({&one, 1});
            (void)server.start_handshake();
            if (server.want() == Want::Error) break;
        }
        return fed;
    }
} // namespace

// No failure path may put private key MATERIAL into an error string. Paths and
// OpenSSL reason strings are fine and useful; the base64 body of the key is not.
//
// Audited alongside this (2026-08-12): `Session::last_error()` is consumed nowhere in
// shipped code, so a failed handshake emits nothing at all, and `Context::last_error()`
// reaches exactly two call sites, both `std::runtime_error` at startup. There is no
// per-request path from OpenSSL text to a client body or a stats line.
namespace
{
    /// The base64 lines of a PEM file, which is the material itself. The BEGIN/END
    /// armour is not secret and appears in no error string anyway.
    std::vector<std::string> pem_body_lines(const std::string& path)
    {
        std::ifstream in(path);
        std::vector<std::string> out;
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("-----", 0) == 0) continue;
            if (line.size() >= 32) out.push_back(line);
        }
        return out;
    }

    void expect_no_key_material(const std::string& text, const std::vector<std::string>& secret,
                                const char* where)
    {
        ASSERT_FALSE(secret.empty()) << "the key had no body to look for";
        for (const std::string& line : secret)
            EXPECT_EQ(text.find(line), std::string::npos)
                << "key material surfaced in " << where << ": " << text;
    }
} // namespace

TEST(ServerContext, NoFailurePathLeaksKeyMaterial)
{
    SelfSigned ca;
    const std::string cert = ca.write_pem();
    const std::string key = ca.write_key_pem();
    const std::vector<std::string> secret = pem_body_lines(key);

    // Positive control: the detector must be able to detect. Without this the test
    // passes for any error string at all, including one that leaks.
    {
        std::ifstream in(key);
        const std::string whole((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        bool found = false;
        for (const std::string& line : secret)
            if (whole.find(line) != std::string::npos) found = true;
        ASSERT_TRUE(found) << "the leak detector cannot detect a leak";
    }

    // Every distinct startup failure, each of which builds an error string, and one
    // of which interpolates the key PATH (which is allowed and wanted).
    {   // key that does not match the certificate
        SelfSigned other;
        Context ctx;
        Context::ServerOptions o;
        o.cert_file = cert;
        o.key_file = other.write_key_pem();
        EXPECT_FALSE(ctx.init_server(o));
        expect_no_key_material(ctx.last_error(), secret, "mismatched-key error");
    }
    {   // key readable beyond its owner
        SelfSigned loose;
        Context ctx;
        Context::ServerOptions o;
        o.cert_file = loose.write_pem();
        o.key_file = loose.write_key_pem(0644);
        EXPECT_FALSE(ctx.init_server(o));
        EXPECT_NE(ctx.last_error().find(o.key_file), std::string::npos) << "path should be named";
        expect_no_key_material(ctx.last_error(), secret, "permissions error");
    }
    {   // already-expired certificate
        SelfSigned expired(-3600);
        Context ctx;
        Context::ServerOptions o;
        o.cert_file = expired.write_pem();
        o.key_file = expired.write_key_pem();
        EXPECT_FALSE(ctx.init_server(o));
        expect_no_key_material(ctx.last_error(), pem_body_lines(o.key_file), "expiry error");
    }
    {   // unreadable key path
        Context ctx;
        Context::ServerOptions o;
        o.cert_file = cert;
        o.key_file = "/nonexistent/llmbridge/key.pem";
        EXPECT_FALSE(ctx.init_server(o));
        expect_no_key_material(ctx.last_error(), secret, "missing-file error");
    }

    // And a Session that fails its handshake: the error text is derived from the same
    // OpenSSL queue, on a context that HAS loaded the key.
    {
        Context sctx;
        Context::ServerOptions o;
        o.cert_file = cert;
        o.key_file = key;
        ASSERT_TRUE(sctx.init_server(o)) << sctx.last_error();
        Session server;
        ASSERT_TRUE(server.init_server(sctx)) << server.last_error();
        const std::string junk = "GET / HTTP/1.1\r\nHost: x\r\n\r\n"; // plaintext at a TLS server
        (void)server.feed_ciphertext(
            {reinterpret_cast<const uint8_t*>(junk.data()), junk.size()});
        (void)server.start_handshake();
        EXPECT_EQ(server.want(), Want::Error);
        expect_no_key_material(server.last_error(), secret, "handshake error");
    }
}

TEST(ServerSession, UnfinishedHandshakeCannotBufferWithoutBound)
{
    SelfSigned ca;
    const std::string cert = ca.write_pem();
    const std::string key = ca.write_key_pem();
    Context sctx;
    Context::ServerOptions so;
    so.cert_file = cert;
    so.key_file = key;
    ASSERT_TRUE(sctx.init_server(so)) << sctx.last_error();

    // Shape 1: a record declaring the maximum payload, dribbled a byte at a time and
    // never completed. Measured 16,389 bytes accepted, then refused.
    {
        Session server;
        ASSERT_TRUE(server.init_server(sctx)) << server.last_error();
        const uint8_t hdr[5] = {0x16, 0x03, 0x01, 0x40, 0x00}; // handshake, 16384 bytes
        const size_t fed = bytes_accepted_before_refusal(server, hdr, sizeof hdr);
        EXPECT_EQ(server.want(), Want::Error);
        EXPECT_LT(fed, 64u * 1024) << "dribbled record buffered " << fed << " bytes";
    }

    // Shape 2: a well-formed record whose handshake message declares a 16 MB
    // ClientHello. Refused on the FIRST record, 16,009 bytes including the header.
    {
        Session server;
        ASSERT_TRUE(server.init_server(sctx)) << server.last_error();
        std::string rec;
        rec.push_back('\x16');
        rec.push_back('\x03');
        rec.push_back('\x01');
        rec.push_back('\x3e');
        rec.push_back('\x84'); // 16004-byte payload: 1 type + 3 length + 16000 body
        rec.push_back('\x01'); // client_hello
        rec.append("\xff\xff\xff", 3);      // declaring 16 MB
        rec.append(16000, 'A');
        const size_t n = server.feed_ciphertext(
            {reinterpret_cast<const uint8_t*>(rec.data()), rec.size()});
        (void)server.start_handshake();
        EXPECT_EQ(server.want(), Want::Error);
        EXPECT_LT(n, 64u * 1024);
    }
}
