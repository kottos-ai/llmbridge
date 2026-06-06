# app — the runnable Kottos gateway daemon. The gateway logic
# lives in the `gateway` library module; this is just the CLI/signal shell.

add_executable(kottos main.cpp)
target_link_libraries(kottos PRIVATE gateway)
