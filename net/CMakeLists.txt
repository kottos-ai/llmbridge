# net — low-level sockets + zero-alloc HTTP/1.1 framing.
#
# STATIC lib (socket setup lives in src/); the hot-path framer is header-only
# in include/net/http.hpp so it inlines into the proxy loop. No project deps —
# this is the bottom of the Kottos stack.

add_library(net STATIC
        src/socket_util.cpp
)
target_include_directories(net PUBLIC include)
target_compile_features(net PUBLIC cxx_std_20)

if (KOTTOS_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
