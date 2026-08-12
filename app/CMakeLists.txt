# app: the runnable llmbridge gateway daemon. The gateway logic
# lives in the `gateway` library module; this is just the CLI/signal shell.

add_executable(llmbridge main.cpp)
target_link_libraries(llmbridge PRIVATE gateway)

# Install the daemon (so `cmake --install` puts `llmbridge` on PATH). Top-level only.
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
        # TIMEOUT matters: a guard that fails to fire leaves a gateway RUNNING, so
        # without it the regression hangs the suite instead of failing it.
        set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expect}" TIMEOUT 10)
    endfunction()

    llmbridge_cli_refuses(CliRefusesListenTlsWithoutCert
            "--listen-tls needs --tls-cert and --tls-key"
            --listen 0 --upstream 127.0.0.1:9 --listen-tls)
    llmbridge_cli_refuses(CliRefusesCertWithoutListenTls
            "--tls-cert/--tls-key given without --listen-tls"
            --listen 0 --upstream 127.0.0.1:9 --tls-cert /nonexistent --tls-key /nonexistent)

    if (NOT LLMBRIDGE_TLS)
        # Both fail-open cases a non-TLS build must refuse instead of downgrading.
        llmbridge_cli_refuses(CliRefusesListenTlsWithoutTlsBuild
                "--listen-tls requires a TLS build"
                --listen 0 --upstream 127.0.0.1:9 --listen-tls
                --tls-cert /nonexistent --tls-key /nonexistent)
        llmbridge_cli_refuses(CliRefusesHttpsUpstreamWithoutTlsBuild
                "https:// upstream requires a TLS build"
                --listen 0 --upstream https://example.invalid)
    endif ()
endif ()
