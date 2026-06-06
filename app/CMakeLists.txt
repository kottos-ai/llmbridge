# app — the runnable llmbridge gateway daemon. The gateway logic
# lives in the `gateway` library module; this is just the CLI/signal shell.

add_executable(llmbridge main.cpp)
target_link_libraries(llmbridge PRIVATE gateway)
