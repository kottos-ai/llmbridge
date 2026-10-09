// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Optional JSON configuration for the daemon (`--config FILE`). Strict: unknown keys, wrong
// types and bad values fail startup; `_` keys are comments; paths, never secrets; the CLI
// wins. Every value is copied out of the zero-copy DOM. DESIGN.md "Configuration".

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llmbridge::app
{
    /// Everything `--config` can set. Only keys present in the file are written; absent keys
    /// leave the caller's value, so these defaults are never consulted.
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
        /// One entry per venue, in the order the file lists them; a policy selects by index.
        struct UpstreamEntry
        {
            std::string url;       // IP:PORT, HOST:PORT or http(s)://HOST[:PORT]
            std::string dialect; // "openai" | "anthropic" | "gemini" | "cohere"
        };
        std::vector<UpstreamEntry> upstreams;
        std::vector<std::string> strip_headers; // dropped from upstream requests

        // timeouts, all in seconds
        bool has_upstream_s = false;
        double upstream_s = 0;
        bool has_client_idle_s = false;
        double client_idle_s = 0;
        bool has_pool_idle_s = false;
        double pool_idle_s = 0;
        bool has_connect_s = false;
        double connect_s = 0;

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
        bool has_prefault_mb = false;
        double prefault_mb = 0;
    };

    /// Parse `text` into `out`; on failure `err` is one line naming the offending key.
    bool parse_config(std::string_view text, ConfigFile& out, std::string& err);

    /// Read `path` and parse it. Same contract; `err` also covers an unreadable file.
    bool load_config(const std::string& path, ConfigFile& out, std::string& err);
} // namespace llmbridge::app
