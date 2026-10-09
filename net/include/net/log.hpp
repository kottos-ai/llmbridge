// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Structured logging that cannot cost the latency claim: compile- and runtime-gated, no
// allocation, one write(2) per line. Never log a credential at any level: header names and
// lengths, never values. DESIGN.md "Logging".

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Compile-time floor: levels below it emit no code and evaluate no arguments.
//   0=Trace 1=Debug 2=Info (default) 3=Warn 4=Error 5=Off
#ifndef LLMBRIDGE_LOG_COMPILE_LEVEL
#define LLMBRIDGE_LOG_COMPILE_LEVEL 2
#endif

namespace llmbridge::net::log
{
    enum class Level : uint8_t
    {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
        Off = 5
    };

    /// Parse "trace".."off"; false on anything else, so a typo is refused, never defaulted.
    bool level_from_name(std::string_view name, Level& out) noexcept;
    const char* level_name(Level) noexcept;

    /// Runtime level, read with a relaxed load. Set it before the workers start; changing it
    /// under load is racy by design and costs at most a stale decision for one line.
    void set_level(Level) noexcept;
    Level level() noexcept;

    // Defaults to the compile floor, so a debug-floor build emits debug without set_level.
    inline std::atomic<uint8_t> g_level_cache{static_cast<uint8_t>(LLMBRIDGE_LOG_COMPILE_LEVEL)};

    [[nodiscard]] inline bool enabled(Level l) noexcept
    {
        return static_cast<uint8_t>(l) >= g_level_cache.load(std::memory_order_relaxed);
    }

    // Identity: a thread index and name, "Class#instance" per object, and g_seq per request.

    /// Call once at the top of each thread; `name` is stored by pointer and must outlive it.
    void register_thread(const char* name, unsigned index) noexcept;
    unsigned thread_index() noexcept;
    const char* thread_name() noexcept;

    /// Monotonic, process-wide instance numbers, so an id is unique across workers.
    uint64_t next_instance() noexcept;

    /// "Class#instance", the object identity a reader greps for.
    struct Id
    {
        const char* cls;
        uint64_t inst;
    };

    // A fixed stack buffer: appends past the end are dropped and the line marked truncated.
    class Line
    {
    public:
        static constexpr size_t kCap = 1024;

        void put(char c) noexcept;
        void put(std::string_view s) noexcept;
        void put(const char* s) noexcept;
        void put(uint64_t v) noexcept;
        void put(int64_t v) noexcept;
        void put(double v) noexcept;
        void put(bool v) noexcept;
        void put(const void* p) noexcept;
        void put(Id id) noexcept;

        [[nodiscard]] std::string_view view() const noexcept { return {_b, _n}; }
        [[nodiscard]] bool truncated() const noexcept { return _trunc; }

    private:
        char _b[kCap];
        size_t _n = 0;
        bool _trunc = false;
    };

    // A type with a free `log_put(Line&, const T&)` found by ADL is loggable. Constrained,
    // or it outranks the string_view overload for std::string and fails deep in the header.
    template <class T>
        requires requires(Line& li, const T& val) { log_put(li, val); }
    inline void put_one(Line& l, const T& v)
    {
        log_put(l, v); // found by ADL: this is how a class gets a print method
    }
    inline void put_one(Line& l, std::string_view v) { l.put(v); }
    inline void put_one(Line& l, const char* v) { l.put(v); }
    inline void put_one(Line& l, char v) { l.put(v); }
    inline void put_one(Line& l, bool v) { l.put(v); }
    inline void put_one(Line& l, double v) { l.put(v); }
    inline void put_one(Line& l, Id v) { l.put(v); }
    inline void put_one(Line& l, int v) { l.put(static_cast<int64_t>(v)); }
    inline void put_one(Line& l, long v) { l.put(static_cast<int64_t>(v)); }
    inline void put_one(Line& l, long long v) { l.put(static_cast<int64_t>(v)); }
    inline void put_one(Line& l, unsigned v) { l.put(static_cast<uint64_t>(v)); }
    inline void put_one(Line& l, unsigned long v) { l.put(static_cast<uint64_t>(v)); }
    inline void put_one(Line& l, unsigned long long v) { l.put(static_cast<uint64_t>(v)); }
    inline void put_one(Line& l, const void* v) { l.put(v); }

    // The default sink does one write(2) to stderr; a deployment can install one that queues
    // lines for a writer thread. Contract: `write` runs on the loop thread and must not
    // block, throw or allocate.
    class Sink
    {
    public:
        virtual ~Sink() = default;
        virtual void write(Level, std::string_view line) noexcept = 0;
    };

    /// Install a sink (caller-owned; nullptr restores stderr) before the workers start.
    void set_sink(Sink*) noexcept;

    /// Lines the sink reports it refused or dropped.
    uint64_t dropped() noexcept;
    void note_dropped(uint64_t n) noexcept;

    // ---------------------------------------------------------------- emit
    void emit_prefix(Line&, Level, const char* file, int line) noexcept;
    void emit_line(Level, const Line&) noexcept;

    template <class... A>
    inline void emit(Level lv, const char* file, int line, const A&... args) noexcept
    {
        Line l;
        emit_prefix(l, lv, file, line);
        (put_one(l, args), ...);
        emit_line(lv, l);
    }
} // namespace llmbridge::net::log

// `if constexpr` makes the floor real: below it the arguments are never even evaluated.
#define LB_LOG_AT(lv, ...)                                                                    \
    do {                                                                                      \
        if constexpr (static_cast<int>(lv) >= LLMBRIDGE_LOG_COMPILE_LEVEL)                    \
        {                                                                                     \
            if (::llmbridge::net::log::enabled(lv))                                            \
                ::llmbridge::net::log::emit((lv), __FILE__, __LINE__, __VA_ARGS__);            \
        }                                                                                     \
    } while (0)

#define LB_TRACE(...) LB_LOG_AT(::llmbridge::net::log::Level::Trace, __VA_ARGS__)
#define LB_DEBUG(...) LB_LOG_AT(::llmbridge::net::log::Level::Debug, __VA_ARGS__)
#define LB_INFO(...) LB_LOG_AT(::llmbridge::net::log::Level::Info, __VA_ARGS__)
#define LB_WARN(...) LB_LOG_AT(::llmbridge::net::log::Level::Warn, __VA_ARGS__)
#define LB_ERROR(...) LB_LOG_AT(::llmbridge::net::log::Level::Error, __VA_ARGS__)
