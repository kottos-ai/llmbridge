# Kottos — Phase A benchmark proof-of-concept

**A hundred hands to route your inference.**

Kottos is a sub-millisecond LLM gateway for agentic workloads. This directory is
the **Phase A proof-of-concept**: the smallest artifact that proves (or kills)
the founding thesis —

> A C++ proxy can sit between an agent and a model provider and add **< 1 ms of
> p99 overhead at 1000+ RPS**, where LiteLLM adds 10–20 ms and OpenRouter ~50 ms.

**Result: thesis validated with ~16× margin.** On a Linux laptop (i7-9750H,
6c/12t, all processes co-located), doing the **same work LiteLLM does** (full
OpenAI↔Anthropic translation), the proxy adds **30 µs median / 63 µs p99 at
1000 RPS** (self-measured) — every request far under 1 ms.

![Kottos vs LiteLLM](bench/results/comparison.svg)

Equal-work head-to-head — **both** gateways translate OpenAI↔Anthropic, same
200 ms mock backend, same open-loop load gen (Linux, 2026-06-04):

| RPS  | Kottos added p99 (self) | LiteLLM added p99 | LiteLLM throughput   |
|------|-------------------------|-------------------|----------------------|
| 100  | **0.075 ms**            | 124.79 ms         | 100/100 ✓            |
| 1000 | **0.063 ms**            | 9 650 ms          | 248/1000 (saturated) |
| 5000 | **0.045 ms**            | 12 513 ms         | 228/5000 (saturated) |

The unsaturated **100 RPS** row is the apples-to-apples number: **~1660× lower
p99**. LiteLLM (1 uvicorn worker) saturates at ~250 RPS on this box, then
degrades into multi-second latency; Kottos holds 45–75 µs p99 across the whole
range. (Kottos is proxy-self-measured; LiteLLM is client-measured e2e − backend,
since it can't self-report — its own client-measured delta is sub-noise.)

## Saturation point

How far does one thread go? Driven through an **instant C++ backend** (so the
backend never saturates first — verified: direct-to-backend tracked target up
to 100k RPS while the proxy capped at ~58k, proving the proxy is the
bottleneck):

![Throughput saturation](bench/results/saturation.svg)

| Offered RPS | Kottos achieved | client p99 | verdict                                         |
|-------------|-----------------|------------|-------------------------------------------------|
| 10 000      | 10 000          | 200 µs     | clean                                           |
| 30 000      | 30 001          | 140 µs     | clean                                           |
| 40 000      | 40 000          | 160 µs     | sub-ms p99                                      |
| 50 000      | 50 000          | 280 µs     | sub-ms p99, tail starting to fray               |
| 60 000+     | **~58–60 000**  | seconds    | saturated (achieved plateaus, backlog explodes) |

- **Throughput ceiling: ~58,000 RPS on a single thread** (achieved plateaus at
  57.7–60.5k across 60k/80k/100k offered, while the direct-to-backend leg keeps
  tracking 100k — so the proxy, not the backend, is the limit).
- **Sub-millisecond p99 holds up to ~50 k RPS** — i.e. 50× the Phase A bar's
  1000 RPS, on one core. (At 1000 RPS the byte-forward self-measured added p99 is
  ~44 µs; ~63 µs with full OpenAI↔Anthropic translation.)
- For contrast, LiteLLM (1 worker) saturates at **~250 RPS** — Kottos's
  single-thread ceiling is **~230× higher**, and scales further with more
  loops (SO_REUSEPORT, Phase D).
- The proxy does ~2× the socket work per request as the backend (read client →
  write upstream → read upstream → write client), which is why it caps at ~58k
  vs the backend's 100k+ — consistent, not mysterious.

Reproduce: `./bench/saturate.sh 5 2 "10000 30000 50000 60000 80000 100000"`

## Architecture

**One class, one event loop, zero external runtime dependencies.** Kottos is a
single-threaded, non-blocking **epoll** event loop wrapped in a `Gateway` class:
accept clients, frame HTTP requests, optionally translate the provider dialect,
forward over a **keep-alive upstream pool**, read the response, (translate
back), write to the client. No framework. No DAG dispatcher. Just C++20.

```
CMakeLists.txt          root — pure CMake, no third-party fetches (GoogleTest only for tests)
net/                    LIB  — sockets + zero-alloc HTTP/1.1 framing
  include/net/{socket_util.hpp, http.hpp}   src/socket_util.cpp   tests/
provider/               LIB  — hand-rolled JSON + OpenAI↔Anthropic dialect translation
  include/provider/{json.hpp, translate.hpp}   src/translate.cpp   tests/
gateway/                LIB  — the Gateway event loop + a tiny vendored latency histogram
  include/gateway/{gateway.hpp, metrics.hpp}   src/{gateway.cpp, metrics.cpp}   tests/
app/                    EXE  — the `kottos` daemon (CLI/signal shell)
bench/                  EXE  — loadgen + fastbackend + Python mock + scripts
```

### Overhead measurement (the number that matters)

For each request the gateway stamps the clock at four points and reports
`added = (upstream_request_sent − client_request_received) + (client_response_sent − upstream_response_received)`
— i.e. request-path work + response-path work, with the upstream wait excluded
entirely. That isolates *Kottos's* contribution from the backend's response
time. Recorded into a tiny vendored linear-bucket `Histogram` (20 ns buckets).

### Provider translation (the equal-work comparison)

With `--translate anthropic`, the gateway does the *same work LiteLLM does* —
parse the OpenAI request, translate to the Anthropic Messages dialect, call the
provider, translate the Anthropic response back to OpenAI (the `provider` module,
hand-rolled zero-alloc JSON). This exists so the LiteLLM comparison is fair, not
passthrough-vs-translation. Result (same Anthropic mock, same load gen, both
translating):

| | added p99 @ 100 RPS | sustained throughput |
|---|---|---|
| **Kottos** (translating) | ~75 µs self / <1 ms client | ~58k RPS/thread |
| **LiteLLM** (translating) | ~125 ms | ~250 RPS/worker, then seconds |

The translation itself costs Kottos only a few µs over byte-forwarding (byte-forward
self p99 ~44 µs → ~63 µs translating at 1000 RPS). LiteLLM adds ~125 ms p99 doing
the same translation and saturates at ~250 RPS/worker — confirming the gap is
structural (C++ event loop vs Python per-request object churn), not a measurement
artifact. Run it: `--translate anthropic` +
`bench/mock_provider.py --format anthropic`.

## Provider translations

OpenAI chat-completions is the canonical client format. Kottos translates it to
each structurally-different provider wire format and back, in hand-rolled
zero-alloc C++ (the `provider/` module). Select one with `--translate`:

| `--translate` | Upstream dialect | Maps (request → / ← response) |
|---|---|---|
| `none` (default) | OpenAI-compatible — byte-forward | — |
| `anthropic` | Anthropic Messages | system extraction, content blocks, `max_tokens` ⟷ `stop_reason`, usage |
| `gemini` | Google Gemini `generateContent` | `contents`/`parts`, role `model`, `systemInstruction`, `generationConfig` ⟷ `candidates`, `finishReason`, `usageMetadata` |
| `cohere` | Cohere Chat v2 (`/v2/chat`) | messages, `top_p`→`p` ⟷ content blocks, `finish_reason`, `usage.tokens` |

**OpenAI-compatible providers need no body translation** — Groq, Together,
Fireworks, DeepInfra, Mistral, Perplexity, xAI, OpenRouter, Cerebras, vLLM,
Ollama, … speak the OpenAI dialect, so `--translate none` byte-forwards them
(only auth/endpoint differ). The dialects above are the cases where the body
actually changes shape — i.e. the work that costs LiteLLM milliseconds.

**Coverage:** the common chat path — model, system, user/assistant turns,
`max_tokens`/`temperature`/`top_p`; response content / finish-reason / usage.
Representative, not 100% provider-complete: **streaming deltas, tool-calling,
vision/multimodal, and Anthropic `cache_control` are Phase B.** Every dialect
has GoogleTest coverage (request, response, finish-reason mapping, round-trip).

## Build

Requires CMake ≥ 3.20 and a C++20 compiler. **No other runtime dependencies.**
First configure fetches GoogleTest *only* (and only when building tests).

**CLion:** open this directory as the project root — it configures automatically.

**CLI:**

```sh
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j
ctest --test-dir cmake-build-release --output-on-failure   # run module tests
```

## Tests

GoogleTest, one executable per class/concern under each module's `tests/`
**400 tests**, run with `ctest --test-dir <build>`:

| Suite | File | Tests | What |
|-------|------|------:|------|
| http framer | `net/tests/http_test.cpp` | 296 | Complete/NeedMore/Error/Pipeline tables + byte-by-byte incremental-arrival property + lenient-parse quirks |
| JSON + translate | `provider/tests/{json,translate}_test.cpp` | 32 | hand-rolled JSON parser/builder; OpenAI↔Anthropic request/response translation + round-trip |
| socket utils | `net/tests/socket_util_test.cpp` | 24 | loopback: flag setters, listener bind/reuse/accept, non-blocking connect (success/refused/bad-IP) |
| Histogram + clock | `gateway/tests/metrics_test.cpp` | 26 | linear-bucket histogram (config, record, percentile, overflow, clear) + monotonic `now_ns()` |
| Stats | `gateway/tests/stats_test.cpp` | 5 | zeroed counters + three independent sub-ms-resolution histograms |
| Gateway integration | `gateway/tests/gateway_test.cpp` | 17 | round-trip, keep-alive, multi-client + pool reuse, warm-up gating, upstream-refused, shutdown, construction |

> The integration suite caught a real bug: a use-after-free in the event loop
> when an upstream connect failed and one connection was freed while still
> referenced by a later event in the same poll batch. Fixed by deferring
> connection deletion to the end of each batch (`Gateway::close_*` mark `doomed`;
> the sweep happens after each batch in `run()`).

Executables land in `cmake-build-release/bin/` (`kottos`, `loadgen`, `fastbackend`).

## Run it

```sh
BIN=cmake-build-release/bin   # wherever you built

# terminal 1 — mock provider, 200 ms simulated generation latency
python3 bench/mock_provider.py --port 9001 --latency-ms 200

# terminal 2 — the proxy
$BIN/kottos --listen 8088 --upstream 127.0.0.1:9001

# terminal 3 — drive load (open-loop, coordinated-omission corrected)
$BIN/loadgen --target 127.0.0.1:8088 --rps 1000 --duration 20 --warmup 5
```

The proxy prints its added-latency histogram on SIGINT/SIGTERM (or after
`--duration`). Use `--warmup S` to exclude the connection-pool ramp.

## Reproduce the full benchmark + chart

The scripts auto-locate the binaries in `cmake-build-*/bin` (override with
`BIN=/path/to/bin`):

```sh
./bench/run_bench.sh 200 20 5 "100 500 1000 5000"   # Kottos vs mock: latency_ms dur warmup rps...
./bench/saturate.sh 8 2 "20000 40000 60000 80000"   # single-thread saturation sweep
./bench/run_litellm.sh 200 15 5 "100 500 1000 5000" # LiteLLM head-to-head (needs the venv below)
python3 bench/make_chart.py                          # -> bench/results/comparison.svg
python3 bench/make_saturation_chart.py               # -> bench/results/saturation.svg
```

LiteLLM venv (one-time): `python3 -m venv bench/.litellm-venv && bench/.litellm-venv/bin/pip install 'litellm[proxy]'`

## Honesty / caveats (read before quoting numbers)

- **Dev box, co-located.** Mock + proxy + loadgen share one Linux laptop
  (i7-9750H, 6 cores / 12 threads). Absolute tails are an upper bound; the delta
  methodology cancels most co-location noise. A clean public number wants
  separate hosts (Phase D).
- **Localhost mock, no TLS/WAN.** Phase A measures pure proxy overhead, not
  real-provider round trips. TLS + real providers come in Phase C.
- **LiteLLM ran with 1 worker** (CLI default, v1.87.0). Its ~250 RPS ceiling is
  per-worker and scales with more workers; its **per-request** ~125 ms p99
  overhead (doing the same OpenAI↔Anthropic translation) does not.
- **Two measurement methods.** Kottos's headline is *proxy-self-measured*
  (rigorous, no cross-run sampling noise). LiteLLM's is *client-measured*
  (e2e − backend). Kottos's own client-measured delta is < 0.25 ms — below the
  noise floor, consistent with the µs self-report.
- **Content-Length framing only** (no chunked transfer-encoding yet); **epoll
  only** (Linux). Per-connection `new`/`delete` (a slab arena is staged for the
  multi-loop phase).

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). No third-party
runtime dependencies; GoogleTest (BSD-3-Clause) is fetched at build time only,
for the test suite.