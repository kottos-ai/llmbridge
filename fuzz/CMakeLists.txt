# libFuzzer targets for the two hand-rolled parsers, llmbridge's highest-risk
# surface (untrusted bytes → json.hpp / http.hpp). Clang-only: libFuzzer ships
# with Clang and needs -fsanitize=fuzzer. Enabled via -DLLMBRIDGE_BUILD_FUZZERS=ON.
#
#   cmake -B build-fuzz -DLLMBRIDGE_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build-fuzz
#   ./build-fuzz/bin/fuzz_json -max_total_time=120 fuzz/corpus/json
#   ./build-fuzz/bin/fuzz_http -max_total_time=120 fuzz/corpus/http
#
# Corpus policy. The committed corpus is worth having because CI fuzzes for only 60 s
# per target: a corpus that already encodes the interesting shapes means each run
# explores past them instead of rediscovering "\r\n\r\n". But a fuzzer appends every
# coverage-increasing input it finds, so the directory grows without bound unless
# somebody minimises it. Before committing corpus growth, merge into a fresh directory
# and keep the result:
#
#   ./build-fuzz/bin/fuzz_http -merge=1 /tmp/min fuzz/corpus/http
#
# -merge=1 keeps a minimal set with identical coverage; the 2026-08-12 pass cut
# http 40 -> 27 files and tls 19 -> 11 with no coverage lost. Hand-written seeds carry
# a descriptive name instead of a hash, deliberately: they are the half a reader can
# review, so preserve them across a merge.

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "fuzz: libFuzzer needs Clang; skipping (compiler is ${CMAKE_CXX_COMPILER_ID}). "
                    "Configure with -DCMAKE_CXX_COMPILER=clang++ to build fuzzers.")
    return()
endif ()

# Sanitize with fuzzer + ASan + UBSan for the sharpest signal. json.hpp/http.hpp
# are header-only (linked for their public include dirs); the SSE translator is
# compiled into `provider`, so fuzz_sse links it for the object too.
#
# -UNDEBUG is load-bearing and was missing until 2026-08-12. This project forces
# CMAKE_BUILD_TYPE=Release when it is unset (see the top-level CMakeLists), Release
# carries -DNDEBUG, and the CI fuzz job configures without a build type. Every
# assert() in a fuzz target was therefore compiled to nothing, so the structural
# invariants in fuzz_http were never checked and CI was fuzzing for crashes alone.
# A check that cannot fail is worse than no check: it reads as coverage.
foreach (tgt json http sse)
    add_executable(fuzz_${tgt} fuzz_${tgt}.cpp)
    target_compile_options(fuzz_${tgt} PRIVATE -g -O1 -UNDEBUG -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_${tgt} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach ()

target_link_libraries(fuzz_json PRIVATE provider) # provider/include (json.hpp)
target_link_libraries(fuzz_http PRIVATE net)      # net/include (http.hpp)
target_link_libraries(fuzz_sse PRIVATE provider)  # provider/sse.hpp + compiled sse.cpp

# The inbound TLS handshake: the first surface an unauthenticated remote peer
# reaches. Only in a TLS build, because there is no server Session without one.
if (LLMBRIDGE_TLS)
    add_executable(fuzz_tls_server fuzz_tls_server.cpp)
    target_compile_options(fuzz_tls_server PRIVATE -g -O1 -UNDEBUG -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_tls_server PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(fuzz_tls_server PRIVATE net OpenSSL::SSL OpenSSL::Crypto)
endif ()
