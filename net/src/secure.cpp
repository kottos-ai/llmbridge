// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Out-of-line on purpose. _GNU_SOURCE must be defined before ANY libc header for
// explicit_bzero to be declared, which a header cannot reliably guarantee, and
// the caller should not have to care. The call cost is irrelevant here (once per
// request, off the token path).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "net/secure.hpp"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <string.h>  // glibc declares explicit_bzero here
#include <strings.h> // the BSDs declare it here
#endif

namespace llmbridge::net
{
    namespace
    {
        // Last-resort barrier: a memset reached through a volatile function pointer.
        // The compiler cannot prove what it points to, so it cannot elide the call.
        // Correct everywhere, but used only when no named primitive exists; see the
        // header for why "works for an indirect reason" is a real downside.
        void* (*const volatile memset_barrier)(void*, int, size_t) = &std::memset;
    } // namespace

    void secure_clear(void* p, size_t n) noexcept
    {
        if (p == nullptr || n == 0) return;
#if defined(_WIN32)
        ::SecureZeroMemory(p, n);
#elif defined(LLMBRIDGE_HAVE_EXPLICIT_BZERO)
        ::explicit_bzero(p, n);
#elif defined(LLMBRIDGE_HAVE_MEMSET_S)
        ::memset_s(p, n, 0, n);
#else
        memset_barrier(p, 0, n);
#endif
    }
} // namespace llmbridge::net
