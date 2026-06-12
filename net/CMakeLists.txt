# net — low-level sockets + zero-alloc HTTP/1.1 framing.
#
# STATIC lib (socket setup lives in src/); the hot-path framer is header-only
# in include/net/http.hpp so it inlines into the proxy loop. No project deps —
# this is the bottom of the llmbridge stack.

add_library(net STATIC
        src/socket_util.cpp
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

if (LLMBRIDGE_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
