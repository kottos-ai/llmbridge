// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// libFuzzer target for the inbound TLS path: arbitrary bytes from an unauthenticated
// peer, fed to a server-role Session through the same four memory-BIO calls the
// gateway uses. This is the first surface a remote attacker reaches, before framing,
// before translation, before any credential exists, so it must never crash, never
// over-read, and never buffer without bound.
//
// What this actually exercises is our plumbing, not OpenSSL's: feed_ciphertext ->
// SSL -> read_plaintext, the error latching in Want, and the shutdown path. OpenSSL
// is fuzzed by its own project; the memory-BIO glue in net/src/tls.cpp is not fuzzed
// anywhere else.
//
// Build (Clang, and it needs a TLS build):
//   cmake -B build-fuzz -DLLMBRIDGE_BUILD_FUZZERS=ON -DLLMBRIDGE_TLS=ON \
//         -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target fuzz_tls_server
//   ./build-fuzz/bin/fuzz_tls_server -max_total_time=120 fuzz/corpus/tls

#include "net/tls.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
    using llmbridge::net::tls::Context;
    using llmbridge::net::tls::Session;
    using llmbridge::net::tls::Want;

    // A self-signed identity, generated once and written to mode-600 temp files
    // because Context::init_server takes paths and refuses a world-readable key.
    // Built lazily so a failure here is a loud abort instead of a silent no-op
    // fuzzer that reports coverage for a Context that never initialised.
    struct Identity
    {
        std::string cert_path, key_path;
        Context ctx;

        Identity()
        {
            EVP_PKEY* key = EVP_RSA_gen(2048);
            X509* crt = X509_new();
            ASN1_INTEGER_set(X509_get_serialNumber(crt), 1);
            X509_gmtime_adj(X509_getm_notBefore(crt), 0);
            X509_gmtime_adj(X509_getm_notAfter(crt), 86400);
            X509_set_pubkey(crt, key);
            X509_NAME* nm = X509_get_subject_name(crt);
            X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>("fuzz.local"), -1,
                                       -1, 0);
            X509_set_issuer_name(crt, nm);
            X509_sign(crt, key, EVP_sha256());

            const std::string base = "/tmp/llmbridge_fuzz_tls_" + std::to_string(::getpid());
            cert_path = base + ".crt";
            key_path = base + ".key";
            FILE* f = std::fopen(cert_path.c_str(), "wb");
            assert(f);
            PEM_write_X509(f, crt);
            std::fclose(f);
            f = std::fopen(key_path.c_str(), "wb");
            assert(f);
            PEM_write_PrivateKey(f, key, nullptr, nullptr, 0, nullptr, nullptr);
            std::fclose(f);
            // Not inside assert(): a side effect in an assertion vanishes under
            // NDEBUG, and init_server would then refuse the world-readable key with
            // no clue why. This exact line compiled only while asserts were dead.
            const int mode_ok = ::chmod(key_path.c_str(), 0600);
            assert(mode_ok == 0);
            (void)mode_ok;

            Context::ServerOptions o;
            o.cert_file = cert_path;
            o.key_file = key_path;
            const bool ok = ctx.init_server(o);
            assert(ok && "server context failed to initialise; the fuzzer would test nothing");
            (void)ok;

            X509_free(crt);
            EVP_PKEY_free(key);
        }

        ~Identity()
        {
            ::unlink(cert_path.c_str());
            ::unlink(key_path.c_str());
        }
    };

    Identity& identity()
    {
        static Identity id;
        return id;
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    Session server;
    if (!server.init_server(identity().ctx)) return 0;

    // Feed in slices, because a peer controls fragmentation and the framer must hold
    // partial records across calls. One whole-buffer feed would never exercise that,
    // and partial-record handling is where a memory-BIO bug lives.
    size_t off = 0;
    while (off < size)
    {
        const size_t chunk = (data[off] % 253u) + 1u; // 1..253, derived from the input
        const size_t n = std::min(chunk, size - off);
        const size_t fed = server.feed_ciphertext({data + off, n});
        assert(fed <= n && "feed_ciphertext claimed more than it was given");
        off += n;

        (void)server.start_handshake();

        uint8_t plain[4096];
        const size_t got = server.read_plaintext(plain);
        assert(got <= sizeof plain && "read_plaintext overran its buffer");

        uint8_t cipher[4096];
        const size_t out = server.pull_ciphertext(cipher);
        assert(out <= sizeof cipher && "pull_ciphertext overran its buffer");

        // Bounded buffering. The record layer caps an unfinished handshake at one
        // record; if our glue ever accumulates ahead of the Session this catches it.
        // Generous, because the point is "bounded", not a specific number.
        assert(server.pending_output_bytes() < (1u << 20));

        if (server.want() == Want::Error) break; // latched: nothing more to learn
        if (fed < n) break;                      // refused input; the caller closes
    }

    // The teardown path also runs on garbage, and it emits bytes while doing it.
    (void)server.shutdown();
    uint8_t drain[4096];
    while (server.pull_ciphertext(drain) > 0) {}
    return 0;
}
