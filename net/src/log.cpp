// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "net/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace llmbridge::net::log
{
    namespace
    {
        // The built-in sink: one write(2) per line, no mutex. A single write of a
        // short buffer to the same fd is not formally atomic, but the kernel does
        // not interleave a lone small write with another, so whole lines survive
        // concurrent workers. Fragments would be worse than a lock here; a lock
        // would be worse than both on the event loop.
        class StderrSink final : public Sink
        {
        public:
            void write(Level, std::string_view line) noexcept override
            {
                // Deliberately ignoring the result. A logger that reacts to a failed
                // write has nowhere to report it, and retrying would block the loop.
                const ssize_t n = ::write(STDERR_FILENO, line.data(), line.size());
                (void)n;
            }
        };

        StderrSink g_stderr_sink;
        std::atomic<Sink*> g_sink{nullptr}; // nullptr = the built-in above
        std::atomic<uint64_t> g_dropped{0};
        std::atomic<uint64_t> g_instances{0};

        // Thread identity. Stored by pointer: `name` must outlive the thread, which
        // is why the API documents a string literal.
        thread_local const char* t_name = "main";
        thread_local unsigned t_index = 0;

        int64_t now_wall_ns() noexcept
        {
            timespec ts{};
            ::clock_gettime(CLOCK_REALTIME, &ts);
            return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
        }
    } // namespace

    const char* level_name(Level l) noexcept
    {
        switch (l)
        {
            case Level::Trace: return "TRACE";
            case Level::Debug: return "DEBUG";
            case Level::Info: return "INFO ";
            case Level::Warn: return "WARN ";
            case Level::Error: return "ERROR";
            case Level::Off: return "OFF  ";
        }
        return "?????";
    }

    bool level_from_name(std::string_view n, Level& out) noexcept
    {
        if (n == "trace") { out = Level::Trace; return true; }
        if (n == "debug") { out = Level::Debug; return true; }
        if (n == "info") { out = Level::Info; return true; }
        if (n == "warn") { out = Level::Warn; return true; }
        if (n == "error") { out = Level::Error; return true; }
        if (n == "off") { out = Level::Off; return true; }
        return false; // refuse, never default: a typo must not silently pass
    }

    void set_level(Level l) noexcept
    {
        g_level_cache.store(static_cast<uint8_t>(l), std::memory_order_relaxed);
    }
    Level level() noexcept
    {
        return static_cast<Level>(g_level_cache.load(std::memory_order_relaxed));
    }

    void register_thread(const char* name, unsigned index) noexcept
    {
        t_name = name ? name : "?";
        t_index = index;
    }
    unsigned thread_index() noexcept { return t_index; }
    const char* thread_name() noexcept { return t_name; }

    uint64_t next_instance() noexcept
    {
        return g_instances.fetch_add(1, std::memory_order_relaxed);
    }

    void set_sink(Sink* s) noexcept { g_sink.store(s, std::memory_order_relaxed); }
    uint64_t dropped() noexcept { return g_dropped.load(std::memory_order_relaxed); }
    void note_dropped(uint64_t n) noexcept
    {
        g_dropped.fetch_add(n, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------ Line
    void Line::put(char c) noexcept
    {
        if (_n + 1 > kCap) { _trunc = true; return; }
        _b[_n++] = c;
    }

    void Line::put(std::string_view s) noexcept
    {
        const size_t room = kCap - _n;
        if (s.size() > room)
        {
            std::memcpy(_b + _n, s.data(), room);
            _n = kCap;
            _trunc = true;
            return;
        }
        std::memcpy(_b + _n, s.data(), s.size());
        _n += s.size();
    }

    void Line::put(const char* s) noexcept { put(s ? std::string_view(s) : "(null)"); }

    void Line::put(uint64_t v) noexcept
    {
        char tmp[20];
        size_t i = sizeof tmp;
        do { tmp[--i] = static_cast<char>('0' + (v % 10)); v /= 10; } while (v);
        put(std::string_view(tmp + i, sizeof tmp - i));
    }

    void Line::put(int64_t v) noexcept
    {
        if (v < 0)
        {
            put('-');
            // Negating INT64_MIN is UB; go through the unsigned domain instead.
            put(static_cast<uint64_t>(~static_cast<uint64_t>(v) + 1));
            return;
        }
        put(static_cast<uint64_t>(v));
    }

    void Line::put(bool v) noexcept { put(v ? std::string_view("true") : std::string_view("false")); }

    void Line::put(double v) noexcept
    {
        // Three decimals, no locale, no allocation. Enough for a duration in ms; a
        // logger is not the place to render a full IEEE double.
        if (v < 0) { put('-'); v = -v; }
        const auto whole = static_cast<uint64_t>(v);
        put(whole);
        put('.');
        auto frac = static_cast<uint64_t>((v - static_cast<double>(whole)) * 1000.0 + 0.5);
        if (frac > 999) frac = 999;
        put(static_cast<char>('0' + (frac / 100) % 10));
        put(static_cast<char>('0' + (frac / 10) % 10));
        put(static_cast<char>('0' + frac % 10));
    }

    void Line::put(const void* p) noexcept
    {
        put("0x");
        auto v = reinterpret_cast<uintptr_t>(p);
        char tmp[16];
        size_t i = sizeof tmp;
        do { tmp[--i] = "0123456789abcdef"[v & 0xf]; v >>= 4; } while (v);
        put(std::string_view(tmp + i, sizeof tmp - i));
    }

    void Line::put(Id id) noexcept
    {
        put(id.cls ? id.cls : "?");
        put('#');
        put(id.inst);
    }

    // ------------------------------------------------------------------ emit
    void emit_prefix(Line& l, Level lv, const char* file, int line) noexcept
    {
        // "2026-08-12T18:04:05.123456Z LEVEL worker0/1 gateway.cpp:1234 "
        //
        // Wall time here, deliberately, and it is the ONE place the project uses it:
        // a log line has to correlate with the outside world (a provider's dashboard,
        // a customer's incident). LATENCY.md's rule still holds for measurement,
        // where every interval is monotonic.
        const int64_t ns = now_wall_ns();
        const auto secs = static_cast<time_t>(ns / 1'000'000'000);
        tm g{};
        ::gmtime_r(&secs, &g);
        char stamp[32];
        const int n = std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02dT%02d:%02d:%02d.%06lldZ ",
                                    g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min,
                                    g.tm_sec,
                                    static_cast<long long>((ns % 1'000'000'000) / 1000));
        if (n > 0) l.put(std::string_view(stamp, static_cast<size_t>(n)));

        l.put(level_name(lv));
        l.put(' ');
        l.put(thread_name());
        l.put('/');
        l.put(static_cast<uint64_t>(thread_index()));
        l.put(' ');

        // Basename only. An absolute build path is noise and leaks the build layout.
        std::string_view f(file ? file : "?");
        const size_t slash = f.find_last_of('/');
        if (slash != std::string_view::npos) f.remove_prefix(slash + 1);
        l.put(f);
        l.put(':');
        l.put(static_cast<uint64_t>(line));
        l.put(' ');
    }

    void emit_line(Level lv, const Line& l) noexcept
    {
        // Assemble the terminator here, not in Line, so `view()` stays exactly
        // what the caller built and a sink that frames records itself is not forced
        // to strip a newline.
        char out[Line::kCap + 16];
        const std::string_view v = l.view();
        const size_t n = v.size() > Line::kCap ? Line::kCap : v.size();
        std::memcpy(out, v.data(), n);
        size_t k = n;
        if (l.truncated())
        {
            static constexpr std::string_view kMark = " ...[truncated]";
            std::memcpy(out + k, kMark.data(), kMark.size());
            k += kMark.size();
        }
        out[k++] = '\n';

        Sink* s = g_sink.load(std::memory_order_relaxed);
        if (s) s->write(lv, std::string_view(out, k));
        else g_stderr_sink.write(lv, std::string_view(out, k));
    }
} // namespace llmbridge::net::log
