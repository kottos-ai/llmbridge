# app: the runnable llmbridge gateway daemon. The gateway logic
# lives in the `gateway` library module; this is just the CLI/signal shell.

add_executable(llmbridge main.cpp config.cpp)
target_link_libraries(llmbridge PRIVATE gateway provider)
target_include_directories(llmbridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The config parser as a library too, so it can be tested without running main().
add_library(llmbridge_config STATIC config.cpp)
target_link_libraries(llmbridge_config PUBLIC provider)
target_include_directories(llmbridge_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Install the daemon (so `cmake --install` puts `llmbridge` on path). Top-level only.
if (PROJECT_IS_TOP_LEVEL)
    install(TARGETS llmbridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()

# ── CLI refusal tests ────────────────────────────────────────────────────────
# These guards live in main.cpp, which no unit test links, so until 2026-08-12
# nothing covered them. That is how --listen-tls came to be silently ignored in a
# non-TLS build: accepted, listener plaintext, operator none the wiser. Each case
# asserts the exact message, so a guard cannot pass by refusing for another reason.
if (LLMBRIDGE_BUILD_TESTS)
    function(llmbridge_cli_refuses name expect)
        add_test(NAME ${name} COMMAND llmbridge ${ARGN})
        # Timeout matters: a guard that fails to fire leaves a gateway running, so
        # without it the regression hangs the suite instead of failing it.
        set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expect}" TIMEOUT 10)
    endfunction()

    llmbridge_cli_refuses(CliRefusesListenTlsWithoutCert
            "a TLS listener needs a certificate and a key"
            --listen 0 --upstream 127.0.0.1:9 --listen-tls)
    llmbridge_cli_refuses(CliRefusesConfigWithoutPath
            "--config needs a path"
            --config)
    llmbridge_cli_refuses(CliRefusesDuplicateConfig
            "given twice"
            --config /nonexistent/a.json --config /nonexistent/b.json)
    llmbridge_cli_refuses(CliRefusesUnreadableConfig
            "cannot read"
            --config /nonexistent/llmbridge.json)
    llmbridge_cli_refuses(CliRefusesCertWithoutListenTls
            "certificate/key was given without enabling the"
            --listen 0 --upstream 127.0.0.1:9 --tls-cert /nonexistent --tls-key /nonexistent)

    if (NOT LLMBRIDGE_TLS)
        # Both fail-open cases a non-TLS build must refuse instead of downgrading.
        llmbridge_cli_refuses(CliRefusesListenTlsWithoutTlsBuild
                "requires a TLS build"
                --listen 0 --upstream 127.0.0.1:9 --listen-tls
                --tls-cert /nonexistent --tls-key /nonexistent)
        llmbridge_cli_refuses(CliRefusesHttpsUpstreamWithoutTlsBuild
                "https:// upstream requires a TLS build"
                --listen 0 --upstream https://example.invalid)
    endif ()
endif ()

if (LLMBRIDGE_BUILD_TESTS)
    include(GoogleTest)
    add_executable(app_config_test tests/config_test.cpp)
    target_link_libraries(app_config_test PRIVATE llmbridge_config gateway GTest::gtest_main)
    target_compile_definitions(app_config_test PRIVATE
            LLMBRIDGE_EXAMPLE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/llmbridge.example.json")
    gtest_discover_tests(app_config_test)
endif ()
