# gateway: the whole llmbridge proxy in one self-contained event-loop class
# (Gateway) + a tiny vendored latency histogram (metrics). No framework, no
# external deps: links only `net` (sockets + HTTP framing) and `provider`
# (dialect translation). The runnable daemon is built in app/.

add_library(gateway STATIC
        src/metrics.cpp
        src/engine.cpp
        src/resolve.cpp
        src/core/client.cpp
        src/core/pool.cpp
        src/core/sink.cpp
        src/core/stream.cpp
        src/core/sweep.cpp
        src/core/transport.cpp
        src/core/upstream.cpp
        src/gateway_epoll.cpp
        src/gateway_uring.cpp
        src/request.cpp
        src/response.cpp
)
target_include_directories(gateway PUBLIC include PRIVATE src)
target_compile_features(gateway PUBLIC cxx_std_20)
target_link_libraries(gateway PUBLIC net provider)

if (LLMBRIDGE_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
