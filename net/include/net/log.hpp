// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Structured logging for the gateway, sized for a process that must not lose its
// latency claim to its own diagnostics.
//
// THE CONSTRAINT. Roughly twenty interesting events per request at ~84k RPS is 1.7M
// records/second, against a published budget of 41-80 us added p99 for the WHOLE
// request. One fprintf to stderr costs 1-5 us and takes a lock, so twenty of them
// exceed the entire number the project is sold on. A logger here is therefore not a
// convenience, it is a thing that can invalidate the benchmark.
//
// THE SHAPE THAT FOLLOWS.
//
//   1. TWO LEVEL GATES. A compile-time floor removes call sites entirely, so a
//      Release build carries no trace/debug code at all and the published numbers
//      are measured on what ships. A runtime level then gates what remains, as one
//      relaxed atomic load and a predictable branch.
//   2. NO ALLOCATION, EVER. A line is formatted into a fixed stack buffer. Past the
//      buffer it truncates and says so; it never grows, never allocates, never
//      throws.
//   3. NO IOSTREAMS. `operator<<` on the event loop means locales, virtual dispatch
//      and allocation. The variadic form below reads the same and costs none of it.
//   4. ONE write(2) PER LINE, so concurrent workers interleave whole lines instead
//      of fragments. There is no mutex.
//   5. A SINK SEAM. The default sink writes to stderr.
//
// WHAT MUST NEVER BE LOGGED. No credential, at any level, ever: not an
// Authorization value, not an x-api-key, not a bearer token, not a TLS private key.
// Log a header's NAME and LENGTH, never its value. `SECURITY.md` promises
// credentials are never logged, and this is the file where that promise is kept or
// broken.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Compile-time floor. Anything below it is not compiled at all: the arguments are
// not evaluated and no code is emitted. Release keeps Info and above so a running
// process still says something; a debugging build passes 0 to get Trace.
//   0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Off
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

    /// Parse "trace".."off". Returns false on anything else, so a typo in a config
    /// or a flag is refused, never silently becoming a default.
    bool level_from_name(std::string_view name, Level& out) noexcept;
    const char* level_name(Level) noexcept;

    /// Runtime level. Set before the worker threads start; it is read on the hot
    /// path with a relaxed load, so changing it under load is racy by design and
    /// only ever costs a stale decision for one line.
    void set_level(Level) noexcept;
    Level level() noexcept;

    inline std::atomic<uint8_t> g_level_cache{static_cast<uint8_t>(Level::Info)};

    [[nodiscard]] inline bool enabled(Level l) noexcept
    {
        return static_cast<uint8_t>(l) >= g_level_cache.load(std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------- identity
    //
    // A log line is worthless if you cannot tell WHICH connection or WHICH request
    // it belongs to. Three identities, all cheap:
    //
    //   thread   a small index and a name, assigned once per thread. NOT
    //            pthread_self(), which prints as a meaningless 15-digit number.
    //   object   a class name plus a monotonic instance number, so a connection is
    //            "Connection#42" for its whole life and can be grepped.
    //   request  the existing g_seq sequencer, which already gives a total order
    //            across workers without trusting any clock.

    /// Call once at the top of each thread. `name` must be a string literal or
    /// otherwise outlive the thread; it is stored by pointer, never copied.
    void register_thread(const char* name, unsigned index) noexcept;
    unsigned thread_index() noexcept;
    const char* thread_name() noexcept;

    /// Monotonic instance numbers for loggable objects. Process-wide and shared
    /// across workers, so an id is unique in the log without a per-class counter.
    uint64_t next_instance() noexcept;

    /// "Class#instance", the object identity a reader greps for.
    struct Id
    {
        const char* cls;
        uint64_t inst;
    };

    // ---------------------------------------------------------------- line buffer
    //
    // Fixed capacity on the stack. Appends past the end are dropped and the line is
    // marked truncated, because a logger that reallocates under load is a latency
    // bug that only appears under load.
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

    // Anything with a free `log_put(Line&, const T&)` in its own namespace is
    // loggable. This is how an object gets a "print method" without iostreams:
    // Connection defines one, and `LB_LOG_INFO("closed ", *conn)` just works.
    // Constrained so it participates ONLY for types that actually define log_put.
    // Unconstrained, it beat the string_view overload for std::string (which needs a
    // user-defined conversion) and the error surfaced as a confusing ADL failure
    // deep inside the header.
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

    // ---------------------------------------------------------------- sink
    //
    // THE OPEN-CORE SEAM. The default sink formats nothing and does one write(2) to
    // stderr, which is correct and adequate for a sidecar. A deployment that cares
    // about the syscall installs a sink that copies the finished line into a
    // lock-free ring and lets a writer thread do the write; that removes the 1-5 us
    // syscall from the event loop without changing a single call site.
    //
    // Contract: `write` is called from the loop thread, must not block, must not
    // throw, and must not allocate. A sink that blocks turns a diagnostic into an
    // outage.
    class Sink
    {
    public:
        virtual ~Sink() = default;
        virtual void write(Level, std::string_view line) noexcept = 0;
    };

    /// Install a sink. Not thread-safe: call before the worker threads start.
    /// Passing nullptr restores the built-in stderr sink. The caller owns it.
    void set_sink(Sink*) noexcept;

    /// Lines the sink refused or dropped, if it reports any. A sink that silently
    /// loses records produces a log nobody can reason about.
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

// The `if constexpr` is what makes the compile-time floor real: below it the pack is
// never instantiated, so the arguments are not even evaluated and nothing is emitted.
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
