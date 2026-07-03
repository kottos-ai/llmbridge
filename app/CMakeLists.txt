# app — the runnable llmbridge gateway daemon. The gateway logic
# lives in the `gateway` library module; this is just the CLI/signal shell.

add_executable(llmbridge main.cpp)
target_link_libraries(llmbridge PRIVATE gateway)

# Install the daemon (so `cmake --install` puts `llmbridge` on PATH). Top-level only.
if (PROJECT_IS_TOP_LEVEL)
    install(TARGETS llmbridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()
