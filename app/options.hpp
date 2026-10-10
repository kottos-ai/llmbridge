// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Every daemon setting, once: one Option table feeds the flags, the `--config` file and
// --help, so the two inputs share one parser and one set of bounds. DESIGN.md "Configuration".

#pragma once

#include "gateway/gateway.hpp"

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace llmbridge::app
{
    struct Venue
    {
        std::string url, dialect;
    };

    /// Initialised to the built-in defaults; the file is applied first, then the flags.
    struct Settings
    {
        int listen_port = 8088;
        bool listen_tls = false;
        std::string tls_cert, tls_key;
        std::string upstream = "127.0.0.1:9001", dialect = "openai"; // venue 0
        std::vector<Venue> more_upstreams;                           // venues 1..N, file only
        std::vector<std::string> strip_headers;                      // gateway-wide
        double upstream_s = static_cast<double>(Gateway::kDefaultUpstreamIdleNs) / 1e9;
        double client_idle_s = static_cast<double>(Gateway::kDefaultClientIdleNs) / 1e9;
        double pool_idle_s = static_cast<double>(Gateway::kDefaultPoolIdleNs) / 1e9;
        double connect_s = static_cast<double>(Gateway::kDefaultConnectNs) / 1e9;
        std::string io = "auto", log_level = "info";
        int workers = 1;
        bool timing_headers = false;
        double duration_s = 0, warmup_s = 0, prefault_mb = 0;
    };

    enum class Kind { Bool, Int, Number, Text, Choice, List };
    using Field = std::variant<bool Settings::*, int Settings::*, double Settings::*,
                               std::string Settings::*, std::vector<std::string> Settings::*>;

    /// An empty `flag` is file-only; `key` is "group.name". Bounds apply to Int and Number.
    struct Option
    {
        std::string_view flag, key, arg, help;
        Kind kind;
        double lo, hi;
        std::span<const std::string_view> choices;
        Field field;
    };
    std::span<const Option> options() noexcept;
    std::string help_text(std::string_view argv0);

    enum class Cli { Run, Help, Refused };
    /// `args` excludes argv[0]. Reads `--config` first, so a flag wins wherever it sits.
    Cli parse_cli(std::span<char* const> args, Settings& s, std::string& err);
    /// Writes only the keys present; `err` is one line naming the key.
    bool parse_config(std::string_view text, Settings& s, std::string& err);
    bool load_config(const std::string& path, Settings& s, std::string& err);
} // namespace llmbridge::app
