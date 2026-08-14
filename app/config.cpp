// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "config.hpp"

#include "provider/json.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace llmbridge::app
{
    namespace
    {
        namespace json = llmbridge::provider::json;

        // A `_`-prefixed key is a comment. JSON has none and an operator edits this
        // file by hand, so the alternative is either no comments or accepting unknown
        // keys, and accepting unknown keys is the thing this parser exists to refuse.
        bool is_comment(std::string_view k) { return !k.empty() && k.front() == '_'; }

        bool fail(std::string& err, std::string msg)
        {
            err = std::move(msg);
            return false;
        }

        /// Reject any member of `group` not in `allowed`. Naming the group and the key
        /// is the whole point: "unknown key" without a name is not actionable.
        bool only(const json::Value& group, std::string_view name,
                  const std::vector<std::string_view>& allowed, std::string& err)
        {
            for (const auto& [k, v] : group.obj)
            {
                if (is_comment(k)) continue;
                bool ok = false;
                for (std::string_view a : allowed)
                    if (k == a) { ok = true; break; }
                if (!ok)
                    return fail(err, "config: unknown key \"" + std::string(k) + "\" in \"" +
                                         std::string(name) + "\"");
            }
            return true;
        }

        bool want_bool(const json::Value& g, std::string_view grp, std::string_view key,
                       bool& has, bool& out, std::string& err)
        {
            const json::Value* v = g.find(key);
            if (!v) return true;
            if (v->type != json::Value::Type::Bool)
                return fail(err, "config: \"" + std::string(grp) + "." + std::string(key) +
                                     "\" must be true or false");
            has = true;
            out = v->boolean;
            return true;
        }

        bool want_num(const json::Value& g, std::string_view grp, std::string_view key,
                      bool& has, double& out, double lo, double hi, std::string& err)
        {
            const json::Value* v = g.find(key);
            if (!v) return true;
            if (v->type != json::Value::Type::Number)
                return fail(err, "config: \"" + std::string(grp) + "." + std::string(key) +
                                     "\" must be a number");
            // strtod over a bounded copy: the DOM keeps the raw number text, and
            // from_chars for double is not available on every supported libstdc++.
            const std::string txt(v->sv);
            char* end = nullptr;
            const double d = std::strtod(txt.c_str(), &end);
            if (end == txt.c_str() || *end != '\0')
                return fail(err, "config: \"" + std::string(grp) + "." + std::string(key) +
                                     "\" is not a valid number: " + txt);
            if (d < lo || d > hi)
                return fail(err, "config: \"" + std::string(grp) + "." + std::string(key) +
                                     "\" out of range: " + txt);
            has = true;
            out = d;
            return true;
        }

        /// Strings are COPIED here, which is what lets the caller drop the DOM. The
        /// parser's `sv` is a view into the input and is still JSON-escaped; config
        /// values are paths, URLs and enum words, none of which may contain an escape,
        /// so a backslash is refused instead of silently mis-decoded.
        /// Array of non-empty strings. Rejects a bare string, because "authorization"
        /// where ["authorization"] was meant is the kind of typo that must not quietly
        /// become a no-op on a header the operator believes is being dropped.
        bool want_str_array(const json::Value& g, std::string_view grp, std::string_view key,
                            std::vector<std::string>& out, std::string& err)
        {
            const json::Value* v = g.find(key);
            if (!v) return true;
            const std::string where = std::string(grp) + "." + std::string(key);
            if (!v->is_array()) return fail(err, "config: \"" + where + "\" must be an array of strings");
            for (const json::Value& e : v->arr)
            {
                if (e.type != json::Value::Type::String)
                    return fail(err, "config: \"" + where + "\" must contain only strings");
                if (e.sv.empty()) return fail(err, "config: \"" + where + "\" must not contain an empty string");
                if (e.sv.find('\\') != std::string_view::npos)
                    return fail(err, "config: \"" + where + "\" must not contain a backslash escape");
                out.emplace_back(e.sv);
            }
            return true;
        }

        bool want_str(const json::Value& g, std::string_view grp, std::string_view key,
                      std::string& out, std::string& err)
        {
            const json::Value* v = g.find(key);
            if (!v) return true;
            if (v->type != json::Value::Type::String)
                return fail(err, "config: \"" + std::string(grp) + "." + std::string(key) +
                                     "\" must be a string");
            if (v->sv.find('\\') != std::string_view::npos)
                return fail(err, "config: \"" + std::string(grp) + "." + std::string(key) +
                                     "\" must not contain a backslash escape");
            out.assign(v->sv);
            return true;
        }

        bool one_of(const std::string& v, std::string_view grp, std::string_view key,
                    const std::vector<std::string_view>& allowed, std::string& err)
        {
            if (v.empty()) return true;
            for (std::string_view a : allowed)
                if (v == a) return true;
            std::string msg = "config: \"" + std::string(grp) + "." + std::string(key) +
                              "\" must be one of:";
            for (std::string_view a : allowed) msg += " " + std::string(a);
            return fail(err, std::move(msg));
        }
    } // namespace

    bool parse_config(std::string_view text, ConfigFile& out, std::string& err)
    {
        bool ok = false;
        const json::Value root = json::parse(text, ok);
        if (!ok) return fail(err, "config: not valid JSON");
        if (!root.is_object()) return fail(err, "config: top level must be an object");
        if (!only(root, "(top level)", {"listen", "upstream", "timeouts", "runtime"}, err))
            return false;

        if (const json::Value* g = root.find("listen"))
        {
            if (!g->is_object()) return fail(err, "config: \"listen\" must be an object");
            if (!only(*g, "listen", {"port", "tls", "cert", "key"}, err)) return false;
            double port = 0;
            bool has_port = false;
            if (!want_num(*g, "listen", "port", has_port, port, 0, 65535, err)) return false;
            if (has_port)
            {
                out.has_listen_port = true;
                out.listen_port = static_cast<uint16_t>(port);
            }
            if (!want_bool(*g, "listen", "tls", out.has_listen_tls, out.listen_tls, err))
                return false;
            if (!want_str(*g, "listen", "cert", out.tls_cert, err)) return false;
            if (!want_str(*g, "listen", "key", out.tls_key, err)) return false;
        }

        if (const json::Value* g = root.find("upstream"))
        {
            if (!g->is_object()) return fail(err, "config: \"upstream\" must be an object");
            if (!only(*g, "upstream", {"url", "translate", "strip_headers"}, err)) return false;
            if (!want_str(*g, "upstream", "url", out.upstream_url, err)) return false;
            if (!want_str(*g, "upstream", "translate", out.translate_mode, err)) return false;
            if (!want_str_array(*g, "upstream", "strip_headers", out.strip_headers, err)) return false;
            if (!one_of(out.translate_mode, "upstream", "translate",
                        {"none", "anthropic", "gemini", "cohere"}, err))
                return false;
        }

        if (const json::Value* g = root.find("timeouts"))
        {
            if (!g->is_object()) return fail(err, "config: \"timeouts\" must be an object");
            if (!only(*g, "timeouts", {"upstream_s", "client_idle_s", "pool_idle_s"}, err))
                return false;
            // Upper bounds are sanity, not policy: a value this large is a typo (an
            // operator meaning milliseconds), and silently accepting it disables the
            // timeout the operator thought they were setting.
            if (!want_num(*g, "timeouts", "upstream_s", out.has_upstream_s, out.upstream_s, 0,
                          31536000, err))
                return false;
            if (!want_num(*g, "timeouts", "client_idle_s", out.has_client_idle_s,
                          out.client_idle_s, 0, 31536000, err))
                return false;
            if (!want_num(*g, "timeouts", "pool_idle_s", out.has_pool_idle_s, out.pool_idle_s, 0,
                          31536000, err))
                return false;
        }

        if (const json::Value* g = root.find("runtime"))
        {
            if (!g->is_object()) return fail(err, "config: \"runtime\" must be an object");
            if (!only(*g, "runtime",
                      {"io", "workers", "timing_headers", "duration_s", "warmup_s", "log_level"},
                      err))
                return false;
            if (!want_str(*g, "runtime", "io", out.io, err)) return false;
            if (!one_of(out.io, "runtime", "io", {"auto", "epoll", "uring"}, err)) return false;
            if (!want_str(*g, "runtime", "log_level", out.log_level, err)) return false;
            if (!one_of(out.log_level, "runtime", "log_level",
                        {"trace", "debug", "info", "warn", "error", "off"}, err))
                return false;
            double w = 0;
            if (!want_num(*g, "runtime", "workers", out.has_workers, w, 1, 4096, err))
                return false;
            if (out.has_workers) out.workers = static_cast<int>(w);
            if (!want_bool(*g, "runtime", "timing_headers", out.has_timing_headers,
                           out.timing_headers, err))
                return false;
            if (!want_num(*g, "runtime", "duration_s", out.has_duration_s, out.duration_s, 0,
                          31536000, err))
                return false;
            if (!want_num(*g, "runtime", "warmup_s", out.has_warmup_s, out.warmup_s, 0, 31536000,
                          err))
                return false;
        }
        return true;
    }

    bool load_config(const std::string& path, ConfigFile& out, std::string& err)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return fail(err, "config: cannot read " + path);
        std::ostringstream ss;
        ss << in.rdbuf();
        // `text` is a local, and that is safe ONLY because parse_config copies every
        // value out of the zero-copy DOM. If a field is ever changed to hold a
        // string_view, this becomes a use-after-free.
        const std::string text = ss.str();
        if (text.empty()) return fail(err, "config: " + path + " is empty");
        if (!parse_config(text, out, err))
        {
            err += " (" + path + ")";
            return false;
        }
        return true;
    }
} // namespace llmbridge::app
