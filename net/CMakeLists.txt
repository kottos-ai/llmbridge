# net: low-level sockets + zero-alloc HTTP/1.1 framing.
#
# STATIC lib (socket setup lives in src/); the hot-path framer is header-only
# in include/net/http.hpp so it inlines into the proxy loop. No project deps.
# This is the bottom of the llmbridge stack.

add_library(net STATIC
        src/socket_util.cpp
        src/upstream.cpp
        src/secure.cpp
        src/log.cpp
)
target_include_directories(net PUBLIC include)
# PUBLIC, so ANYTHING linking net compiles against the same floor. The root sets this
# with add_compile_definitions, which reaches llmbridge's own directory scope and no
# further: a consumer that pulls llmbridge in with add_subdirectory (FetchContent does)
# silently kept the default, so `-DLLMBRIDGE_LOG_LEVEL=debug` compiled out every
# LB_DEBUG in the consumer's own code while enabling llmbridge's. The floor is a
# property of the logger, so it travels with the target that owns it.
target_compile_definitions(net PUBLIC LLMBRIDGE_LOG_COMPILE_LEVEL=${_lb_log})
target_compile_features(net PUBLIC cxx_std_20)

# io_uring backend (Phase 0 scaffolding): compile the raw wrapper only where the
# kernel headers expose it, and propagate LLMBRIDGE_HAVE_URING to consumers so
# they can gate the (Phase 1) io_uring loop and fall back to epoll otherwise.
# Secret-erasure primitive: DETECT the platform function instead of guessing from
# the OS macro. explicit_bzero exists on glibc >= 2.25 and the BSDs but needs
# _GNU_SOURCE to be declared; memset_s is C11 Annex K and optional (MSVC has it,
# glibc does not). Where neither exists, net/src/secure.cpp falls back to a
# volatile-memset barrier: correct everywhere, but only as a last resort.
include(CheckCXXSymbolExists)
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
# glibc declares explicit_bzero in <string.h>; the BSDs in <strings.h>. Check both
# instead of guess, getting this wrong silently downgrades to the fallback.
check_cxx_symbol_exists(explicit_bzero "string.h;strings.h" LLMBRIDGE_HAVE_EXPLICIT_BZERO)
check_cxx_symbol_exists(memset_s "string.h" LLMBRIDGE_HAVE_MEMSET_S)
unset(CMAKE_REQUIRED_DEFINITIONS)
if (LLMBRIDGE_HAVE_EXPLICIT_BZERO)
    target_compile_definitions(net PRIVATE LLMBRIDGE_HAVE_EXPLICIT_BZERO)
    message(STATUS "net: secure_clear -> explicit_bzero")
elseif (LLMBRIDGE_HAVE_MEMSET_S)
    target_compile_definitions(net PRIVATE LLMBRIDGE_HAVE_MEMSET_S)
    message(STATUS "net: secure_clear -> memset_s")
else ()
    message(STATUS "net: secure_clear -> volatile-memset fallback")
endif ()

include(CheckIncludeFileCXX)
check_include_file_cxx("linux/io_uring.h" LLMBRIDGE_HAVE_URING)
if (LLMBRIDGE_HAVE_URING)
    target_sources(net PRIVATE src/uring.cpp)
    target_compile_definitions(net PUBLIC LLMBRIDGE_HAVE_URING)
    message(STATUS "net: io_uring available, building raw uring wrapper")
else ()
    message(STATUS "net: linux/io_uring.h not found, epoll only")
endif ()

# TLS for outbound provider connections (option + find_package live at the ROOT
# CMakeLists so the imported OpenSSL::* targets are visible to sibling dirs, an
# imported target is directory-scoped, and gateway/tests links it too).
if (LLMBRIDGE_TLS)
    target_sources(net PRIVATE src/tls.cpp)
    target_link_libraries(net PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(net PUBLIC LLMBRIDGE_HAVE_TLS)
    message(STATUS "net: TLS enabled. OpenSSL ${OPENSSL_VERSION}")
else ()
    message(STATUS "net: TLS disabled (-DLLMBRIDGE_TLS=ON to enable)")
endif ()

if (LLMBRIDGE_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
