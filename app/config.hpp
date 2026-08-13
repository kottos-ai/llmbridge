// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Optional JSON configuration for the gateway daemon (`--config FILE`).
//
// WHY IT EXISTS. Fourteen flags is uncomfortable; the fifteenth is impossible.
// Multi-upstream routing needs an ordered list with per-upstream fields (host, TLS,
// SNI, dialect, route tag), and that cannot be expressed in flat flags without
// inventing a mini-language with none of the tooling and worse errors. The grouped
// shape below exists so `upstream` can become an array later without disturbing
// anything else.
//
// THE CONTRACT, and the first rule is the load-bearing one:
//
//   1. UNKNOWN KEYS ARE A STARTUP ERROR, never ignored. A config parser that skips a
//      misspelled `listen_tls` fails open in exactly the shape this project has
//      already been bitten by: `--listen-tls` on a non-TLS build was accepted and
//      ignored, serving plaintext while the operator believed otherwise. A file has
//      no `ps` output to catch that, so it must refuse instead of guess. Wrong types
//      and out-of-range values are errors for the same reason.
//   2. Keys beginning with `_` are comments and are ignored. JSON has none, and an
//      operator-edited file needs them.
//   3. PATHS, NEVER SECRETS. `cert` and `key` are paths. Tokens and provider keys
//      must never become config values, or the file becomes a credential store on
//      disk, which cuts against the standing no-credential-store constraint.
//   4. The CLI wins. The file is read first and flags overwrite it, so a one-off
//      override needs no edit. `bench/*.sh` drives the daemon with eight flags and
//      must keep working.
//
// LIFETIME, and this is a real footgun. `provider::json` is a ZERO-COPY DOM: every
// string in the parsed `Value` is a `string_view` into the input buffer. Parsing a
// temporary and returning views into it is a use-after-free. `Config` therefore owns
// the file bytes in `raw` and every `std::string` field below is COPIED out during
// parsing, so a caller can drop the Config's DOM and keep the values.

#pragma once

#include <cstdint>
#include <string>

namespace llmbridge::app
{
    /// Everything `--config` can set. Defaults here are never consulted: the caller
    /// seeds an instance from its own defaults, and only keys PRESENT in the file are
    /// overwritten, so an absent key means "leave the caller's value alone".
    struct ConfigFile
    {
        // listen
        bool has_listen_port = false;
        uint16_t listen_port = 0;
        bool has_listen_tls = false;
        bool listen_tls = false;
        std::string tls_cert; // empty = absent
        std::string tls_key;

        // upstream
        std::string upstream_url;    // empty = absent
        std::string translate_mode;  // "none" | "anthropic" | "gemini" | "cohere"

        // timeouts, all in seconds
        bool has_upstream_s = false;
        double upstream_s = 0;
        bool has_client_idle_s = false;
        double client_idle_s = 0;
        bool has_pool_idle_s = false;
        double pool_idle_s = 0;

        // runtime
        std::string io;              // "auto" | "epoll" | "uring"
        std::string log_level;       // "trace".."off"
        bool has_workers = false;
        int workers = 0;
        bool has_timing_headers = false;
        bool timing_headers = false;
        bool has_duration_s = false;
        double duration_s = 0;
        bool has_warmup_s = false;
        double warmup_s = 0;
    };

    /// Parse `text` (the whole file) into `out`.
    ///
    /// Returns true on success. On failure returns false and sets `err` to a single
    /// line naming the offending key or value, because "config error" with no key is
    /// the message an operator cannot act on.
    ///
    /// `text` need not outlive the call: every value is copied into `out`.
    bool parse_config(std::string_view text, ConfigFile& out, std::string& err);

    /// Read `path` and parse it. Same contract; `err` also covers an unreadable file.
    bool load_config(const std::string& path, ConfigFile& out, std::string& err);
} // namespace llmbridge::app
