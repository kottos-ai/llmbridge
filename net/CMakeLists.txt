# net — low-level sockets + zero-alloc HTTP/1.1 framing.
#
# STATIC lib (socket setup lives in src/); the hot-path framer is header-only
# in include/net/http.hpp so it inlines into the proxy loop. No project deps —
# this is the bottom of the llmbridge stack.

add_library(net STATIC
        src/socket_util.cpp
        src/upstream.cpp
)
target_include_directories(net PUBLIC include)
target_compile_features(net PUBLIC cxx_std_20)

# io_uring backend (Phase 0 scaffolding): compile the raw wrapper only where the
# kernel headers expose it, and propagate LLMBRIDGE_HAVE_URING to consumers so
# they can gate the (Phase 1) io_uring loop and fall back to epoll otherwise.
include(CheckIncludeFileCXX)
check_include_file_cxx("linux/io_uring.h" LLMBRIDGE_HAVE_URING)
if (LLMBRIDGE_HAVE_URING)
    target_sources(net PRIVATE src/uring.cpp)
    target_compile_definitions(net PUBLIC LLMBRIDGE_HAVE_URING)
    message(STATUS "net: io_uring available — building raw uring wrapper")
else ()
    message(STATUS "net: linux/io_uring.h not found — epoll only")
endif ()

# TLS for outbound provider connections. OpenSSL is the ONE sanctioned runtime
# dependency (CLAUDE.md) and it is confined to the gateway path — the translator
# library stays dependency-free, so `provider` must never link this. OFF by default
# keeps the OSS build dependency-free for anyone who only wants the translator.
option(LLMBRIDGE_TLS "Enable TLS for outbound gateway connections (needs OpenSSL)" OFF)
if (LLMBRIDGE_TLS)
    find_package(OpenSSL 3.0 REQUIRED)
    target_sources(net PRIVATE src/tls.cpp)
    target_link_libraries(net PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(net PUBLIC LLMBRIDGE_HAVE_TLS)
    message(STATUS "net: TLS enabled — OpenSSL ${OPENSSL_VERSION}")
else ()
    message(STATUS "net: TLS disabled (-DLLMBRIDGE_TLS=ON to enable)")
endif ()

if (LLMBRIDGE_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
