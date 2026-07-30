# Benchmarks

`llmbridge` publishes **two separate benchmarks**, because it does two structurally
different jobs:

| | Workload | Unit of work | Metric that matters |
|---|---|---|---|
| **A. Non-streaming** | request → response | one whole request | added latency per request, requests/sec |
| **B. Streaming (SSE)** | one long-lived stream, many tokens | one token | time to first token, added latency per token, concurrent streams |

**The numbers are not interchangeable.** A streaming request occupies a connection
for the whole generation, so "requests per second" is not a meaningful axis for it;
conversely "tokens delivered per second" says nothing about non-streaming throughput.
Any comparison that mixes them is wrong, including ours — so they are reported apart,
with their own charts and their own caveats.

Everything below is reproducible from this repository. All runs are on a **single
co-located host** (client, gateway and provider on loopback), so absolute tails are a
**dev-box upper bound**, not a datacenter figure.

---

## A. Non-streaming: added latency and throughput

Both gateways perform the full OpenAI ⇄ Anthropic translation against the same mock
backend, driven by the same open-loop, coordinated-omission-corrected load generator.

![llmbridge vs LiteLLM — added latency p99](bench/results/comparison.svg)

At the unsaturated **100 RPS** point, llmbridge adds **~0.067 ms p99** vs LiteLLM's
**~82 ms** (≈1,200×), and holds 42–78 µs p99 across 100–5,000 RPS while a single
LiteLLM uvicorn worker saturates around **250 RPS**.

![Throughput saturation — offered vs achieved RPS](bench/results/saturation.svg)

Single-thread ceiling is **~90k RPS**; on this co-located box that ceiling is the
loopback's packet-processing limit, not the CPU (the proxy uses ~1 core at saturation).

**Reproduce**

```sh
./bench/run_headtohead.sh      # latency, llmbridge vs LiteLLM vs direct
./bench/saturate.sh            # throughput ramp against the C++ backend
python3 bench/make_chart.py    # regenerate the SVGs
```

**Known asymmetry.** In this benchmark llmbridge is **proxy-self-measured** (it
reports its own request-path + response-path stamps) while LiteLLM is
**client-measured** (through-LiteLLM minus direct-to-mock), because LiteLLM cannot
self-report. The streaming benchmark below does not have this asymmetry.

---

## B. Streaming (SSE): time to first token and delivered tokens

Both gateways translate the **same Anthropic SSE stream** from the **same provider**
into OpenAI `chat.completion.chunk`s. Every number is taken by the **same client-side
instrument** — neither gateway self-reports.

![Streaming: time to first token vs concurrent streams](bench/results/stream-comparison.svg)

![Streaming: tokens delivered vs concurrent streams](bench/results/stream-saturation.svg)

### Results (median of 3 runs, 50 tok/s per stream, one worker each)

| Concurrent streams | llmbridge added/token (p50 / p99) | LiteLLM added/token (p50 / p99) | TTFT: floor / llmbridge / LiteLLM | Tokens delivered: llmbridge / LiteLLM |
|---|---|---|---|---|
| 16 | 54 / 77 µs | 1.85 ms / 229 ms | 30.8 / **30.6** / 93.8 ms | **100%** / 93% |
| 64 | 55 / 88 µs | 203 ms / 699 ms | 30.6 / **30.9** / 2,322 ms | **100%** / 34% |
| 256 | 77 / 125 µs | **> 2 s** † | 30.7 / **30.9** / 12,134 ms | **99.9%** / 9% |
| 512 | 129 / 144 µs | **> 2 s** † | 30.7 / **31.0** / 14,356 ms | **99.8%** / 3% |

† Beyond the load generator's 2 s histogram range. Reported as "> 2 s" rather than a
number: once samples overflow the tracked range, the tool's percentile function
returns the maximum, which *looks* like a percentile but is not one. The generator
prints an explicit overflow warning; it fired only on LiteLLM at ≥256 streams and
never on llmbridge.

**How to read this.** llmbridge's time to first token sits **on the no-gateway floor**
(~31 ms) at every level and it delivers ~100% of the achievable token stream. LiteLLM
holds up at 16 streams, then queues: by 512 streams it delivers 3% of the tokens and
first-token latency is **~14 s**. Its throughput ceiling is roughly **1,100 tokens/s**
regardless of offered load.

### Where llmbridge itself saturates

Measured separately against the C++ provider (so the harness is not the limit):

| Concurrent streams | offered tokens/s | epoll delivered | io_uring delivered |
|---|---|---|---|
| 512 | 25,600 | 99% | 99% |
| 1,024 | 51,200 | 98% | 98% |
| 2,048 | 102,400 | 91% | 92% |
| 4,096 | 204,800 | 82% | 72% |

llmbridge's knee is **between 2,048 and 4,096 concurrent streams (~95–100k tokens/s)**
on one worker — the levels tested are powers of two, so the knee is bracketed to a 2×
band, not localised. For scale, even the lower bound is ~90× LiteLLM's ceiling.

Both event-loop backends behave equivalently up to 2,048 streams (see "Why epoll and
io_uring measure the same"). Past the knee this run shows epoll holding 82% vs
io_uring's 72%, but that is **a single sample at one level and should not be treated as
a result**: plausible causes include the backends' different back-pressure strategies
(epoll pauses upstream reads; io_uring uses a bounded output buffer), but it has not
been investigated and could simply be run-to-run variance.

**Reproduce**

```sh
./bench/run_stream_headtohead.sh 60 20 20 6 "16 64 256 512"   # vs LiteLLM
./bench/run_stream_saturate.sh   60 20 20 5 "512 1024 2048 4096"  # find the knee
IO=epoll ./bench/run_stream_saturate.sh 60 20 20 5 "512 1024 2048 4096"
python3 bench/make_stream_chart.py
```

---

## Methodology: how the streaming numbers are taken

The streaming benchmark had to solve one problem: **how do you attribute latency to a
gateway when the provider itself has jitter?**

**The provider stamps its own emission time inside each token.** `faststream` (and the
Python mock) write `t=<CLOCK_MONOTONIC ns>` into every token's text. That stamp rides
the payload through whatever gateway is under test, so the client computes:

```
added latency = arrival_at_client − emission_at_provider
```

No clock synchronisation (same host, same monotonic clock) and — the important part —
**no assumption about when a token *should* have arrived**. This sidesteps coordinated
omission structurally: a stalled gateway cannot look good by delivering fewer tokens,
because every token that arrives carries the true age of its own data.

### Fairness controls

Each of these is a way the benchmark could have been rigged, and is therefore enforced:

1. **Same instrument, both sides.** Every number comes from `streamgen`, client-side.
   Neither gateway self-reports.
2. **Same baseline.** "Added" latency is measured against a **direct-to-provider
   control run at the same concurrency**, subtracted identically for both. The floor
   (loopback + provider jitter) is charged to neither gateway. It is not negligible:
   at 16 streams the floor is ~50 µs p50, comparable to llmbridge's own contribution.
3. **Same workers.** One worker each — llmbridge single-threaded, LiteLLM one uvicorn
   worker, no `SO_REUSEPORT` fan-out. Production LiteLLM deployments scale
   horizontally; these figures are **per worker**.
4. **Same warmup, plus a discard round.** LiteLLM's lazy imports cost far more than a
   few seconds: measured directly, its *first* level came out **134× worse** than the
   same level run later. An unmeasured warm round precedes the sweep so no gateway's
   first measured level is really its cold start.
5. **Same work.** Identical request bytes, model, provider and token rate. Neither side
   requests `stream_options.include_usage`, so neither emits an extra usage chunk.
6. **Both proxies stay up** across the sweep — no restart gives either a fresh-process
   advantage at a particular concurrency.
7. **Order alternates** per level, so drift within a level cannot systematically favour
   one side.
8. **Provider-saturation guard.** If the direct baseline stops scaling, the *provider*
   is the limiter and the gateway numbers at that level are meaningless — the level is
   flagged rather than silently reported.
9. **Equal-work check.** A gateway delivering far fewer tokens is not "faster", it is
   lossy; the delivered-token ratio is reported alongside every latency figure.

### Why there are two providers

`mock_provider.py` (Python, asyncio) saturates around **45–50k tokens/s**, which is
*below* llmbridge — so sweeping against it measures the harness. `faststream.cpp` is
wire-identical (same events, same chunked framing, same emission stamps) but is a
single-threaded epoll loop with no per-token allocation, sustaining **>100k tokens/s**.
The head-to-head and saturation sweeps use the C++ provider; the Python mock remains
available (`PROVIDER=python`) as a cross-check.

`faststream` also **staggers stream phases**. Streams that start together would
otherwise stay in lockstep and fire in one synchronised burst each interval — both
unrealistic and latency-inflating for every path (it moved the control run's p99 from
2,325 µs to 417 µs at 2,048 streams).

### Where the added latency actually goes

The obvious question about any proxy benchmark: **is the added latency the gateway's
own work, or just the price of inserting a process into the path?** `bench/nullrelay.cpp`
answers it — a proxy that does nothing but `read()` and `write()` bytes, no HTTP
parsing, no chunked decode, no translation, no per-chunk state.

At 64 concurrent streams (3,200 tokens/s offered):

| path | p50 added | p99 | gateway CPU |
|---|---|---|---|
| direct — 1 hop, no proxy | 53 µs | 111 µs | 3.6% |
| **nullrelay** — 2 hops, **zero work** | **106 µs** | 178 µs | 4.3% |
| llmbridge (epoll) — 2 hops, full translate | 108 µs | 188 µs | 5.1% |
| llmbridge (io_uring) — 2 hops, full translate | 107 µs | 183 µs | 4.8% |

**The hop costs ~53 µs. llmbridge's entire HTTP framing + chunked decode + SSE state
machine + re-serialisation costs ~2 µs on top of it** — within ~4% of the floor any
proxy pays. Every gateway sits at ~5% of one core, so none of this is compute-bound:
it is kernel wakeup and scheduler dispatch latency.

That also means the honest ceiling on optimising *our code* on this path is ~2 µs/token.
Claims of large streaming-latency wins from further micro-optimisation would be unfounded.

The hop itself, however, **is** reducible — it is mostly CPU idle-state exit latency, not
transit. Keeping the package awake takes it from 54 µs to 39 µs; a busy-polling relay that
never sleeps takes it to **6 µs**. Both cost CPU. See
[`bench/LATENCY-TUNING.md`](bench/LATENCY-TUNING.md) for the measurements, the three ways
to disable C-states on the host, and why spinning workers cannot share a core.

**All numbers in this document are taken with the host untuned** (default `intel_idle` +
`menu` governor, all C-states enabled, turbo on). Tuning shrinks every path in the
comparison, including the baselines, so the relative results stand — but the absolute
figures should always be read alongside the tuning state.

### Why epoll and io_uring measure the same (and when io_uring wins)

They are not equivalent in syscalls. Traced with `strace -c` at 64 streams:

| backend | total syscalls | **per delivered token** |
|---|---|---|
| epoll | 90,447 (32.7k `read` + 32.7k `write` + 15.2k `epoll_wait` + 2.3k `epoll_ctl`) | **3.57** |
| io_uring | 31,970 (26.5k `io_uring_enter`) | **1.26** |

io_uring genuinely batches: **2.8× fewer syscalls per token.** It shows up when
syscalls are made expensive — under `strace` the io_uring build is measurably faster
(p99 483 µs vs 749 µs).

But in normal operation a syscall costs ~1 µs, so saving ~2.3 syscalls/token saves
~2 µs — invisible against the ~53 µs hop cost above. **You cannot batch away a wakeup
you must take in order to forward a token that has just arrived.** io_uring's advantage
is in work-per-wakeup, which is why it matters on the non-streaming path (~90k RPS,
many requests per wakeup) and not in per-token streaming latency at a few thousand
tokens/s, where the loop is idle between tokens.

This is also why added latency *falls* as concurrency rises (129 µs at 512 streams,
but ~18 µs per token in earlier high-concurrency runs): a busier loop is already awake,
so the wakeup cost amortises across more tokens.

### Known limitations

- **Single co-located host.** No TLS, no WAN, no NIC. Absolute tails are an upper
  bound; the *relative* comparison is the point.
- **Localhost provider**, not a real LLM endpoint (the gateway cannot yet terminate TLS
  to `api.anthropic.com` — see the roadmap).
- **One worker per gateway.** Both scale out; neither was given the chance to.
- **LiteLLM percentiles above 2 s are not resolved** (see † above).
- **Per-token latency is dominated by the HOP, not by the gateway's work.** See
  "Where the added latency actually goes" below — measured, not assumed. This bounds
  how much further the streaming path can be optimised: almost not at all.
