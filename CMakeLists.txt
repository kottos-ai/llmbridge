# llmbridge: sub-millisecond LLM gateway. Self-contained, dependency-free C++20.
#
#   net      (lib), sockets + zero-alloc HTTP/1.1 framing
#   provider (lib), hand-rolled JSON + OpenAI<->Anthropic dialect translation
#   gateway  (lib), the whole proxy in one epoll event-loop class + a tiny
#                     vendored latency histogram
#   app      (exe), the runnable `llmbridge` daemon
#   bench    (exe), loadgen + fastbackend
#
# No third-party runtime dependencies: just a C++20 compiler. GoogleTest is
# fetched only when building tests. Open the project root in CLion (or use the
# CLI below); nothing else is required.

cmake_minimum_required(VERSION 3.20)
project(llmbridge VERSION 0.19.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Warnings ────────────────────────────────────────────────────────────────
# On by default; there were none at all before 0.9.0, which is a strange gap for a
# codebase whose pitch is systems rigour. -Werror is CI-only (LLMBRIDGE_WERROR=ON)
# so a warning never blocks a local edit-compile loop, but also never reaches main.
# Scoped to our own targets via add_compile_options AFTER FetchContent, so
# GoogleTest's warnings are not our problem.
option(LLMBRIDGE_WERROR "Treat warnings as errors (CI)" OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif ()
# Optimization. Default Release is -O2, applied via add_compile_options so it does
# NOT clobber a user's CMAKE_CXX_FLAGS_RELEASE (which the default `-DNDEBUG` etc.
# still supply). We deliberately do NOT force -march=native: it bakes in the build
# machine's instruction set and crashes with SIGILL on a different/older CPU, a
# footgun for packagers and CI. Opt in for a local, non-portable perf build (this
# is what reproduces the published benchmark numbers):
#   cmake -B build -DCMAKE_BUILD_TYPE=Release -DLLMBRIDGE_NATIVE_ARCH=ON
option(LLMBRIDGE_NATIVE_ARCH "Tune for THIS machine's CPU (-march=native; non-portable)" OFF)
if (NOT MSVC)
    add_compile_options($<$<CONFIG:Release>:-O2>)
    if (LLMBRIDGE_NATIVE_ARCH)
        add_compile_options($<$<CONFIG:Release>:-march=native>)
    endif ()
endif ()

include(GNUInstallDirs) # CMAKE_INSTALL_{LIB,INCLUDE,BIN}DIR for the install rules below

# All executables collect in <build>/bin so the bench scripts find them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Tests (GoogleTest, the only fetched dependency, and only for tests) ──────
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
# TLS for outbound gateway connections. OpenSSL is the ONE sanctioned runtime
# dependency (CLAUDE.md), confined to the gateway path; the translator library
# (`provider`) stays dependency-free and must never link it. OFF by default so a
# plain build of the OSS remains dependency-free end to end. find_package here at
# the root because imported targets are directory-scoped and both net/ and
# gateway/tests/ link them.
# Compile-time log floor. Release keeps info and above so a running process still
# says something; a lab or debugging build passes `debug` (or `trace`) to compile the
# per-connection and per-request lines IN. They are compiled OUT by default so the
# published latency numbers are measured on exactly what ships.
set(LLMBRIDGE_LOG_LEVEL "info" CACHE STRING "Compiled-in log floor: trace|debug|info|warn|error|off")
set_property(CACHE LLMBRIDGE_LOG_LEVEL PROPERTY STRINGS trace debug info warn error off)
if (LLMBRIDGE_LOG_LEVEL STREQUAL "trace")
    set(_lb_log 0)
elseif (LLMBRIDGE_LOG_LEVEL STREQUAL "debug")
    set(_lb_log 1)
elseif (LLMBRIDGE_LOG_LEVEL STREQUAL "info")
    set(_lb_log 2)
elseif (LLMBRIDGE_LOG_LEVEL STREQUAL "warn")
    set(_lb_log 3)
elseif (LLMBRIDGE_LOG_LEVEL STREQUAL "error")
    set(_lb_log 4)
elseif (LLMBRIDGE_LOG_LEVEL STREQUAL "off")
    set(_lb_log 5)
else ()
    message(FATAL_ERROR
            "LLMBRIDGE_LOG_LEVEL must be trace|debug|info|warn|error|off, got '${LLMBRIDGE_LOG_LEVEL}'")
endif ()
add_compile_definitions(LLMBRIDGE_LOG_COMPILE_LEVEL=${_lb_log})
message(STATUS "llmbridge: compiled-in log floor = ${LLMBRIDGE_LOG_LEVEL}")

option(LLMBRIDGE_TLS "Enable TLS for outbound gateway connections (needs OpenSSL)" OFF)
if (LLMBRIDGE_TLS)
    find_package(OpenSSL 3.0 REQUIRED)
    message(STATUS "llmbridge: TLS enabled. OpenSSL ${OPENSSL_VERSION}")
endif ()

add_subdirectory(net)
# Applied here, after FetchContent has configured GoogleTest, so these apply to our
# targets and not to fetched third-party code.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
    if (LLMBRIDGE_WERROR)
        add_compile_options(-Werror)
    endif ()
endif ()

add_subdirectory(provider)
add_subdirectory(gateway)
add_subdirectory(app)
add_subdirectory(bench)

# Developer debug aids (tools/debug_translate, a steppable translator harness).
# Top-level builds only; consumers embedding llmbridge don't get it.
option(LLMBRIDGE_BUILD_TOOLS "Build developer debug tools (tools/)" ${PROJECT_IS_TOP_LEVEL})
if (LLMBRIDGE_BUILD_TOOLS)
    add_subdirectory(tools)
endif ()

# Fuzz targets for the hand-rolled parsers (json.hpp / http.hpp). Off by default;
# Clang-only. Enable with -DLLMBRIDGE_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++.
option(LLMBRIDGE_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)

if (LLMBRIDGE_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif ()

# ── Install / packaging (top-level builds only) ──────────────────────────────
# Installs the embeddable translator library (llmbridge::provider) + its headers
# (rules live in provider/), the `llmbridge` daemon (rule in app/), and a package
# config so a downstream project can:
#   find_package(llmbridge CONFIG REQUIRED)
#   target_link_libraries(myapp PRIVATE llmbridge::provider)
if (PROJECT_IS_TOP_LEVEL)
    install(EXPORT llmbridgeTargets
            FILE llmbridgeTargets.cmake
            NAMESPACE llmbridge::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llmbridge)

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/llmbridgeConfig.cmake.in
            ${CMAKE_CURRENT_BINARY_DIR}/llmbridgeConfig.cmake
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llmbridge)
    write_basic_package_version_file(
            ${CMAKE_CURRENT_BINARY_DIR}/llmbridgeConfigVersion.cmake
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion)
    install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/llmbridgeConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/llmbridgeConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llmbridge)
endif ()
