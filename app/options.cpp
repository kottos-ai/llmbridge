// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "options.hpp"

#include "provider/json.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace llmbridge::app
{
    namespace
    {
        namespace json = llmbridge::provider::json;

        constexpr std::string_view kDialects[] = {"openai", "anthropic", "gemini",
                                                  "cohere", "bedrock",   "azure"};
        constexpr std::string_view kBackends[] = {"auto", "epoll", "uring"};
        constexpr std::string_view kLevels[] = {"trace", "debug", "info", "warn", "error", "off"};
        constexpr double kYear = 31536000; // a bigger timeout is a unit typo, not a policy

        using K = Kind;
        using S = Settings;
        const Option kOptions[] = {
            {"--listen", "listen.port", "PORT", "listening port", K::Int, 0, 65535, {}, &S::listen_port},
            {"--listen-tls", "listen.tls", "", "serve TLS only; needs a certificate and a key",
             K::Bool, 0, 0, {}, &S::listen_tls},
            {"--tls-cert", "listen.cert", "PATH", "certificate chain, PEM", K::Text, 0, 0, {}, &S::tls_cert},
            {"--tls-key", "listen.key", "PATH", "private key, PEM, owner-only", K::Text, 0, 0, {}, &S::tls_key},
            {"--upstream", "upstream.url", "URL", "IP:PORT, HOST:PORT or http(s)://HOST[:PORT][/BASE]",
             K::Text, 0, 0, {}, &S::upstream},
            {"--upstream-dialect", "upstream.dialect", "NAME", "what the venue speaks", K::Choice, 0, 0,
             kDialects, &S::dialect},
            {"", "upstream.strip_headers", "", "header names dropped from every upstream request",
             K::List, 0, 0, {}, &S::strip_headers},
            {"--upstream-timeout", "timeouts.upstream_s", "SECONDS",
             "upstream silence before a request is aborted, 0 = off", K::Number, 0, kYear, {},
             &S::upstream_s},
            {"--client-idle", "timeouts.client_idle_s", "SECONDS", "idle client lifetime, 0 = off",
             K::Number, 0, kYear, {}, &S::client_idle_s},
            {"--pool-idle", "timeouts.pool_idle_s", "SECONDS", "idle pooled upstream lifetime, 0 = off",
             K::Number, 0, kYear, {}, &S::pool_idle_s},
            {"--connect-timeout", "timeouts.connect_s", "SECONDS",
             "upstream connect and TLS handshake limit, 0 = off", K::Number, 0, kYear, {},
             &S::connect_s},
            {"--io", "runtime.io", "NAME", "event loop", K::Choice, 0, 0, kBackends, &S::io},
            {"--log-level", "runtime.log_level", "LEVEL", "log level", K::Choice, 0, 0, kLevels,
             &S::log_level},
            {"--workers", "runtime.workers", "N", "event loops sharing the port", K::Int, 1, 4096, {},
             &S::workers},
            {"--timing-headers", "runtime.timing_headers", "", "add x-llmbridge-* timing headers",
             K::Bool, 0, 0, {}, &S::timing_headers},
            {"--duration", "runtime.duration_s", "SECONDS",
             "stop after this long and print the profile, 0 = until signalled", K::Number, 0, kYear,
             {}, &S::duration_s},
            {"--warmup", "runtime.warmup_s", "SECONDS", "leave the first seconds out of the histograms",
             K::Number, 0, kYear, {}, &S::warmup_s},
            {"--prefault-mb", "runtime.prefault_mb", "MB", "prefault every request buffer to this size",
             K::Number, 0, 4096, {}, &S::prefault_mb},
        };

        // Old spellings are refused by name: an alias would keep the old model alive.
        struct Retired
        {
            std::string_view name, why;
        };
        constexpr Retired kRetired[] = {
            {"--translate", "is now --upstream-dialect, and names what the venue speaks instead "
                            "of an action we may not perform"},
            {"upstream.translate", "is now \"dialect\", and names what the venue speaks"},
        };
        constexpr std::string_view kNoneIsOpenAi =
            " \"none\" is now \"openai\": it names an OpenAI-compatible venue, never an absence";

        bool fail(std::string& err, std::string msg)
        {
            err = std::move(msg);
            return false;
        }

        Cli refuse(std::string& err, std::string msg)
        {
            err = std::move(msg);
            return Cli::Refused;
        }

        std::string_view retired(std::string_view name)
        {
            for (const Retired& r : kRetired)
                if (r.name == name) return r.why;
            return {};
        }

        bool is_comment(std::string_view k) { return !k.empty() && k.front() == '_'; }

        std::string bounds(double v)
        {
            char b[32];
            const auto [p, ec] = std::to_chars(b, b + sizeof b, v);
            return ec == std::errc{} ? std::string(b, p) : std::string();
        }

        /// One value, as text, from either source. `where` names it the way the source does.
        bool set_text(const Option& o, std::string_view v, Settings& s, const std::string& where,
                      std::string& err)
        {
            const auto range = [&](double d) {
                return (d >= o.lo && d <= o.hi) ||
                       fail(err, where + " out of range: " + std::string(v) + " (allowed " +
                                     bounds(o.lo) + ".." + bounds(o.hi) + ")");
            };
            if (o.kind == Kind::Int)
            {
                long long n = 0;
                const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
                if (ec == std::errc::result_out_of_range) return range(o.hi + 1);
                if (v.empty() || ec != std::errc{} || p != v.data() + v.size())
                    return fail(err, where + " must be an integer: " + std::string(v));
                if (!range(static_cast<double>(n))) return false;
                s.*std::get<int Settings::*>(o.field) = static_cast<int>(n);
                return true;
            }
            if (o.kind == Kind::Number)
            {
                // strtod over a bounded copy: from_chars for double is missing on some libstdc++.
                const std::string t(v);
                char* end = nullptr;
                const bool blank = t.empty() || std::isspace(static_cast<unsigned char>(t[0]));
                const double d = blank ? 0 : std::strtod(t.c_str(), &end);
                if (end == nullptr || end == t.c_str() || *end != '\0')
                    return fail(err, where + " is not a valid number: " + t);
                if (!range(d)) return false; // NaN fails both comparisons
                s.*std::get<double Settings::*>(o.field) = d;
                return true;
            }
            if (o.kind == Kind::Choice)
            {
                if (o.key == "upstream.dialect" && v == "none")
                    return fail(err, where + std::string(kNoneIsOpenAi));
                bool known = false;
                for (std::string_view c : o.choices) known = known || c == v;
                if (!known)
                {
                    std::string m = where + " must be one of:";
                    for (std::string_view c : o.choices) m += " " + std::string(c);
                    return fail(err, m + " (got \"" + std::string(v) + "\")");
                }
            }
            s.*std::get<std::string Settings::*>(o.field) = std::string(v);
            return true;
        }

        /// Strings are copied out, so the caller may drop the zero-copy DOM. A value still
        /// holds JSON escapes; settings are paths, URLs and words, so `\` is refused.
        bool set_json(const Option& o, const json::Value& v, Settings& s, std::string& err)
        {
            const std::string where = "config: \"" + std::string(o.key) + "\"";
            const auto plain = [&](std::string_view t) {
                return t.find('\\') == std::string_view::npos ||
                       fail(err, where + " must not contain a backslash escape");
            };
            switch (o.kind)
            {
            case Kind::Bool:
                if (v.type != json::Value::Type::Bool)
                    return fail(err, where + " must be true or false");
                s.*std::get<bool Settings::*>(o.field) = v.boolean;
                return true;
            case Kind::Int:
            case Kind::Number:
                if (v.type != json::Value::Type::Number) return fail(err, where + " must be a number");
                return set_text(o, v.sv, s, where, err);
            case Kind::Text:
            case Kind::Choice:
                if (v.type != json::Value::Type::String) return fail(err, where + " must be a string");
                return plain(v.sv) && set_text(o, v.sv, s, where, err);
            case Kind::List:
                break;
            }
            // A bare string where a list was meant must not become a silent no-op.
            if (!v.is_array()) return fail(err, where + " must be an array of strings");
            std::vector<std::string> out;
            for (const json::Value& e : v.arr)
            {
                if (e.type != json::Value::Type::String)
                    return fail(err, where + " must contain only strings");
                if (e.sv.empty()) return fail(err, where + " must not contain an empty string");
                if (!plain(e.sv)) return false;
                out.emplace_back(e.sv);
            }
            s.*std::get<std::vector<std::string> Settings::*>(o.field) = std::move(out);
            return true;
        }

        bool apply_group(std::string_view group, const json::Value& g, Settings& s, std::string& err)
        {
            if (!g.is_object())
                return fail(err, "config: \"" + std::string(group) + "\" must be an object");
            for (const auto& [k, v] : g.obj)
            {
                if (is_comment(k)) continue;
                const std::string key = std::string(group) + "." + std::string(k);
                const Option* o = nullptr;
                for (const Option& c : kOptions)
                    if (c.key == key) o = &c;
                if (!o && !retired(key).empty())
                    return fail(err, "config: \"" + std::string(k) + "\" " + std::string(retired(key)));
                if (!o)
                    return fail(err, "config: unknown key \"" + std::string(k) + "\" in \"" +
                                         std::string(group) + "\"");
                if (!set_json(*o, v, s, err)) return false;
            }
            return true;
        }

        /// One object, or an array of the same object for several venues. strip_headers is
        /// gateway-wide, so every entry that sets it must agree.
        bool apply_upstream(const json::Value& g, Settings& s, std::string& err)
        {
            if (!g.is_object() && !g.is_array())
                return fail(err, "config: \"upstream\" must be an object or an array of them");
            if (g.is_array() && g.arr.empty())
                return fail(err, "config: \"upstream\" must not be an empty array");
            const size_t n = g.is_array() ? g.arr.size() : 1;
            bool strip = false;
            for (size_t i = 0; i < n; ++i)
            {
                const json::Value& e = g.is_array() ? g.arr[i] : g;
                if (!e.is_object())
                    return fail(err, "config: each entry in \"upstream\" must be an object");
                Settings one;
                one.upstream.clear();
                one.dialect.clear();
                if (!apply_group("upstream", e, one, err)) return false;
                if (g.is_array() && one.upstream.empty())
                    return fail(err, "config: every entry in an \"upstream\" array needs a url");
                if (e.find("strip_headers") != nullptr)
                {
                    if (strip && one.strip_headers != s.strip_headers)
                        return fail(err, "config: \"upstream.strip_headers\" is gateway-wide; every "
                                         "entry that sets it must list the same headers");
                    strip = true;
                    s.strip_headers = std::move(one.strip_headers);
                }
                if (i > 0)
                    s.more_upstreams.push_back(
                        {std::move(one.upstream), one.dialect.empty() ? "openai" : one.dialect});
                if (i == 0 && !one.upstream.empty()) s.upstream = std::move(one.upstream);
                if (i == 0 && !one.dialect.empty()) s.dialect = std::move(one.dialect);
            }
            return true;
        }

        bool is_group(std::string_view k)
        {
            for (const Option& o : kOptions)
                if (o.key.substr(0, o.key.find('.')) == k) return true;
            return false;
        }

        std::string row(std::string_view left, std::string_view help)
        {
            std::string r = "  " + std::string(left);
            r.resize(std::max<size_t>(r.size() + 1, 30), ' ');
            return r + std::string(help) + "\n";
        }
    } // namespace

    std::span<const Option> options() noexcept { return kOptions; }

    std::string help_text(std::string_view argv0)
    {
        std::string out = "usage: " + std::string(argv0) + " [--config FILE] [flags]\n" +
                          "Flags override the file wherever they appear. In parentheses: the file key.\n";
        out += row("--config FILE", "JSON settings; see app/llmbridge.example.json");
        for (const Option& o : kOptions)
        {
            if (o.flag.empty()) continue;
            std::string help(o.help);
            if (o.kind == Kind::Int) help += ", " + bounds(o.lo) + ".." + bounds(o.hi);
            for (size_t i = 0; i < o.choices.size(); ++i)
                help += (i ? "|" : ": ") + std::string(o.choices[i]);
            const std::string flag = std::string(o.flag) + (o.arg.empty() ? "" : " ") + std::string(o.arg);
            out += row(flag, help + " (" + std::string(o.key) + ")");
        }
        out += "File only:\n";
        for (const Option& o : kOptions)
            if (o.flag.empty()) out += row(o.key, o.help);
        return out + row("-h, --help", "print this and exit");
    }

    Cli parse_cli(std::span<char* const> args, Settings& s, std::string& err)
    {
        const auto missing = [&](size_t i) {
            return i + 1 >= args.size() || std::string_view(args[i + 1]).starts_with("--");
        };
        // The file first, so flags overwrite it; validated before any I/O.
        const char* path = nullptr;
        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string_view a = args[i];
            if (a == "--help" || a == "-h") return Cli::Help;
            if (a != "--config") continue;
            if (missing(i)) return refuse(err, "--config needs a path");
            if (path)
                return refuse(err, "--config given twice (" + std::string(path) + " and " +
                                       args[i + 1] + "); pass it once");
            path = args[++i];
        }
        if (path && !load_config(path, s, err)) return Cli::Refused;

        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string_view a = args[i];
            if (a == "--config") { ++i; continue; }
            const Option* o = nullptr;
            for (const Option& c : kOptions)
                if (!c.flag.empty() && c.flag == a) o = &c;
            if (!o && !retired(a).empty())
                return refuse(err, std::string(a) + " " + std::string(retired(a)));
            if (!o)
            {
                std::string m = "unknown argument '" + std::string(a) + "'";
                if (const size_t eq = a.find('='); eq != std::string_view::npos)
                    m += "; flags take a separate value, so write '" + std::string(a.substr(0, eq)) +
                         " " + std::string(a.substr(eq + 1)) + "'";
                return refuse(err, m + ". Run --help for the accepted flags.");
            }
            if (o->kind == Kind::Bool)
            {
                s.*std::get<bool Settings::*>(o->field) = true;
                continue;
            }
            if (missing(i))
                return refuse(err, std::string(a) + " needs a value (" + std::string(o->arg) + ")");
            if (!set_text(*o, args[++i], s, std::string(a), err)) return Cli::Refused;
        }
        return Cli::Run;
    }

    bool parse_config(std::string_view text, Settings& s, std::string& err)
    {
        bool ok = false;
        const json::Value root = json::parse(text, ok);
        if (!ok)
        {
            // Keys::Any tells a repeated key, which parse() refuses, from bad JSON.
            bool any = false;
            (void)json::parse(text, any, json::Keys::Any);
            return fail(err, any ? "config: a key appears twice in one object"
                                 : "config: not valid JSON");
        }
        if (!root.is_object()) return fail(err, "config: top level must be an object");
        for (const auto& [k, v] : root.obj)
        {
            if (is_comment(k)) continue;
            if (!is_group(k))
                return fail(err, "config: unknown key \"" + std::string(k) + "\" in \"(top level)\"");
            if (!(k == "upstream" ? apply_upstream(v, s, err) : apply_group(k, v, s, err)))
                return false;
        }
        return true;
    }

    bool load_config(const std::string& path, Settings& s, std::string& err)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return fail(err, "config: cannot read " + path);
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        if (text.empty()) return fail(err, "config: " + path + " is empty");
        if (!parse_config(text, s, err)) return fail(err, err + " (" + path + ")");
        return true;
    }
} // namespace llmbridge::app
