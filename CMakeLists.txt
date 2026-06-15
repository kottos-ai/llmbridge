# llmbridge — sub-millisecond LLM gateway. Self-contained, dependency-free C++20.
#
#   net      (lib)  — sockets + zero-alloc HTTP/1.1 framing
#   provider (lib)  — hand-rolled JSON + OpenAI<->Anthropic dialect translation
#   gateway  (lib)  — the whole proxy in one epoll event-loop class + a tiny
#                     vendored latency histogram
#   app      (exe)  — the runnable `llmbridge` daemon
#   bench    (exe)  — loadgen + fastbackend
#
# No third-party runtime dependencies: just a C++20 compiler. GoogleTest is
# fetched only when building tests. Open the project root in CLion (or use the
# CLI below); nothing else is required.

cmake_minimum_required(VERSION 3.20)
project(llmbridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif ()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=native -DNDEBUG")

# All executables collect in <build>/bin so the bench scripts find them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Tests (GoogleTest — the only fetched dependency, and only for tests) ──────
option(LLMBRIDGE_BUILD_TESTS "Build llmbridge module tests" ${PROJECT_IS_TOP_LEVEL})
if (LLMBRIDGE_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif ()

# ── llmbridge modules (bottom-up) ────────────────────────────────────────────────
add_subdirectory(net)
add_subdirectory(provider)
add_subdirectory(gateway)
add_subdirectory(app)
add_subdirectory(bench)

# Developer debug aids (tools/debug_translate — a steppable translator harness).
# Top-level builds only; consumers embedding llmbridge don't get it.
option(LLMBRIDGE_BUILD_TOOLS "Build developer debug tools (tools/)" ${PROJECT_IS_TOP_LEVEL})
if (LLMBRIDGE_BUILD_TOOLS)
    add_subdirectory(tools)
endif ()
