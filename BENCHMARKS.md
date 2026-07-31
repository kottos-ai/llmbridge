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

> **Host configuration changes the competitor's numbers, not just ours — and not every
> setting is unfair.** Measured separately on one host: the `performance` governor helps
> *both* sides (our throughput 79k → 85k; LiteLLM's capacity unchanged at ~246 req/s and
> its latency improved), so it is used. Capping idle states (`cpupower idle-set -D 5`)
> improves our p99 (63 → 37 µs) while cutting **LiteLLM's capacity from 247 to
> 150 req/s** — it heats the machine and LiteLLM is CPU-bound — so it is excluded. Every
> competitive figure here comes from **one stated configuration**: `performance`
> governor, stock idle states. See
> [`bench/BENCHMARK-CONFIG.md`](bench/BENCHMARK-CONFIG.md) §0.

> **Before running or quoting any number here, read
> [`bench/BENCHMARK-CONFIG.md`](bench/BENCHMARK-CONFIG.md).** It lists every knob that
> changes a result and which configuration produced which published figure. Four results
> in this document's history were invalidated by undocumented configuration rather than
> by code — none of which produced an error, only a plausible wrong number. The single
> most important one: the ~84k RPS ceiling requires `BACKENDS=4`; at the default of 1 the
> mock backend is the ceiling, not the gateway.

---

## A. Non-streaming: added latency and throughput

Both gateways perform the full OpenAI ⇄ Anthropic translation against the same mock
backend, driven by the same open-loop, coordinated-omission-corrected load generator.

![llmbridge vs LiteLLM — added latency p99](bench/results/comparison.svg)

At the unsaturated **100 RPS** point, llmbridge adds **80 µs p99** vs LiteLLM's
**87 ms** — a **1,088×** difference — and holds **41–80 µs p99 across 100–5,000 RPS**,
while a single LiteLLM uvicorn worker saturates at **~246 req/s**.

| RPS | **llmbridge** p50 / p99 | LiteLLM added p99 | LiteLLM achieved |
|---|---|---|---|
| 100 | **36 / 80 µs** | 87 ms | 100 / 100 |
| 250 | **30 / 63 µs** | 1,735 ms | 227 (not saturated) |
| 500 | **28 / 60 µs** | 9,798 ms | **246 (saturated)** |
| 1,000 | **21 / 47 µs** | 14,213 ms | 244 |
| 2,000 | **18 / 55 µs** | 18,679 ms | 243 |
| 5,000 | **16 / 41 µs** | 18,695 ms | 236 |

Measured on a cold-booted host, `performance` governor, **stock idle states**, IDE
closed, starting at 57 °C with load 0.29; `ListenOverflows` did not move during the run.

**On the governor.** `performance` is used because it improves *both* sides — LiteLLM's
capacity is unchanged (246 vs 247 req/s) and its latency at 100 RPS improves from 104 ms
to 87 ms. Capping idle states (`cpupower idle-set -D 5`) is deliberately **not** used: it
would improve our p99 further (63 → 37 µs) but cuts LiteLLM's capacity from 247 to
150 req/s by heating the machine, which is not a comparison worth publishing. Both
configurations are documented in
[`bench/BENCHMARK-CONFIG.md`](bench/BENCHMARK-CONFIG.md) §0.

![Throughput saturation — offered vs achieved RPS](bench/results/saturation.svg)

Single-thread ceiling is **~84k RPS** — mean of three cold-boot runs; best single run
86,982, reproducing an earlier 87.6k measurement on the same code. Requires `BACKENDS=4`,
or the mock is the limit (at the default of 1 the mock caps it at ~65k). The conservative
mean is quoted rather than the best run, because the best run is only achievable on a
cold machine.

Throughput is strongly thermally dependent on this laptop, so the number degrades within
a session: three consecutive runs measured **86,982 → 84,928 → 82,380** as the package
climbed 57 → 79 → 85 °C. **Run saturation first and cold.** The `powersave` governor
costs a further ~8% (79k), because `intel_pstate` ramps frequency reactively and a 5s
measurement level is short enough for the ramp to matter.

**What sets that ceiling: the gateway's own CPU, on its single thread.** Three
percentages follow and they have three different denominators, so to be explicit:

- **87-92% of ONE CORE** — the worker thread's own utilisation. llmbridge is
  single-threaded, so 100% of one core is its hard ceiling; at saturation it is nearly
  there, and that is the cap.
- **~95% idle SYSTEM-WIDE** — across all 12 logical CPUs. One saturated core is only ~8%
  of this box, so the machine looks idle while the gateway is maxed. It cannot reach the
  other 11 cores because it is single-threaded; that is what `--workers N` is for.
- **89% kernel / 6.7% llmbridge** — a split of the worker's OWN CPU time (that 87-92%),
  not of the machine. Nine of every ten cycles the gateway burns execute kernel code.

Softirq time is ~0%, so it is not the loopback packet path — an earlier revision of this
document claimed that, and the measurement contradicts it. Throughput therefore tracks
clock speed almost linearly: the same build measures ~78k on this laptop at a thermally
throttled 4.0 GHz and ~88k at 4.5 GHz. Scaling past it is `--workers N` (SO_REUSEPORT,
shared-nothing) or less CPU per request. **C-states are irrelevant here** in the direct
sense — at saturation the CPU never idles — but *disabling* them lowers throughput
indirectly by heating the machine and costing turbo headroom.

### What sets the single-thread ceiling (profiled)

`perf record` on the worker at 75k RPS, 20,016 samples. **89% of its CPU is in the
kernel; 6.9% is llmbridge's own code.**

All percentages below are **shares of the worker thread's own CPU time**, not of the
machine (the machine is ~95% idle throughout — see above).

| cost centre | % of the worker's CPU time |
|---|---|
| TCP stack | 32.7% |
| long tail (418 symbols, each <0.8%) | 31.7% |
| **llmbridge — all of our code** | **6.7%** |
| AppArmor (LSM socket permission checks) | 6.1% |
| locking | 4.7% |
| skb alloc/free | 4.5% |
| socket/skb copies | 3.9% |
| wakeup / scheduler | 3.6% |
| fd lookup per op (`fget`, `sock_from_file`) | 3.2% |
| io_uring core | 2.8% |

Our hottest functions are `u_on_cqe` (1.54%), `http::parse` (1.29%), `u_on_recv`
(1.03%). **Optimising llmbridge's code caps out at roughly 7% more throughput** — the
ceiling is the kernel's TCP path, which is the irreducible price of being a TCP proxy.
The levers that actually matter are, in order: clock speed (throughput tracks it nearly
linearly — the same build measures ~78k at a throttled 4.0 GHz and ~88k at 4.5 GHz),
`--workers N` (the system is 95% idle at saturation, so a second worker nearly doubles
it), and disabling AppArmor.

**AppArmor costs ~6.3%** of the worker's CPU on `aa_inet_msg_perm` /
`apparmor_socket_recvmsg` / `apparmor_socket_sendmsg` — a permission check on every send
and recv. Note that **stopping the AppArmor service does not remove this**: that unloads
profiles, while the LSM's socket hooks are built into the kernel and enabled at boot.
Verified by re-profiling with the service inactive: still 6.37%. Removing it needs
`apparmor=0` on the kernel command line and a reboot — a security tradeoff, so treat it
as a measurement step rather than a deployment recommendation.

**What did NOT help, measured:** `IORING_REGISTER_FILES` (fixed files) was implemented to
attack the 3.2% fd-lookup line. It delivers **~0.5% less CPU per request and no
detectable throughput change** (three interleaved A/B repeats at saturation: 75.9k vs
75.3k, inside ±3% noise) — most of that 3.2% is not addressable this way, since
`io_uring_enter` resolves the ring fd regardless and the per-connection registration is
itself a syscall. **The change was reverted**: it added a slot allocator and a
release-before-close hazard for no measurable benefit. Recorded here so it is not
proposed again from reading the profile.

**Reproduce**

```sh
# Host: stock C-states + governor. Only this sysctl is required (see BENCHMARK-CONFIG.md).
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=8192 net.core.somaxconn=8192

./bench/run_headtohead.sh 200 15 4 100 250 500 1000 2000 5000  # latency vs LiteLLM
BACKENDS=4 ./bench/saturate.sh 5 2 90000 130000                # throughput ramp
#         ^^^^^^^^^^ without this the MOCK is the ceiling (~65k), not the gateway
python3 bench/make_chart.py                                     # regenerate the SVGs
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

> Charts regenerated from this run (`bench/make_stream_chart.py`). Note the source CSV
> is append-only across runs and the chart takes a median over it, so it must be reset
> to a single run's rows before charting — otherwise it silently blends incompatible
> configurations.

### Results — llmbridge vs LiteLLM (50 tok/s per stream, one worker each)

Per-token latency is measured client-side against the provider's own embedded emission
timestamp, so it is the true age of each token on arrival. Raw path latency is shown
rather than a subtracted "added" figure, because subtracting percentiles does not yield a
percentile (see Known limitations).

| Concurrent streams | direct (floor) p50 / p99 | **llmbridge** p50 / p99 | LiteLLM p50 / p99 | TTFT: floor / **llmbridge** / LiteLLM | Tokens delivered: **llmbridge** / LiteLLM |
|---|---|---|---|---|---|
| 16  | 58 / 121 us | **108 / 189 us** | 1.65 ms / 230 ms | 30.8 / **30.3** / 84.3 ms | **100%** / 94% |
| 64  | 53 / 113 us | **109 / 195 us** | 213 ms / 704 ms | 30.8 / **30.8** / 2,206 ms | **100%** / 37% |
| 256 | 53 / 154 us | **124 / 265 us** | > 2 s † | 30.7 / **30.7** / 9,703 ms | **99.9%** / 9% |
| 512 | 54 / 234 us | **172 / 340 us** | > 2 s † | 30.7 / **30.8** / 13,130 ms | **99.9%** / 3% |

† Beyond the load generator's 2 s histogram range, so not a percentile — past the tracked
range the percentile function returns the maximum. The generator prints an explicit
overflow warning; it fired on **LiteLLM at 256 and 512 only**, and on **neither llmbridge
nor the direct baseline at any level**, so every number in those columns is a real
percentile.

**How to read this.** llmbridge's time to first token sits **on the no-gateway floor**
(30.3-30.8 ms vs the floor's 30.7-30.8 ms) at every concurrency, and it delivers
essentially every token the provider emits. Its own per-token cost is **~50-120 us**
against a 20 ms inter-token interval — under 1% of the token budget.

LiteLLM tracks well at 16 streams (94% delivered) and then queues: by 64 streams it
delivers 37% and first-token latency is 2.2 s; by 512 streams it delivers **3%** with a
**13 s** TTFT. Its throughput ceiling is roughly **700-1,200 tokens/s** regardless of
offered load.

**Measurement conditions.** Cold-booted host, `performance` governor, **stock idle
states** (`cpupower idle-set -D 5` deliberately not used — it would improve our numbers
while cutting LiteLLM's capacity, see section A), IDE closed, starting at 68 °C.
`ListenOverflows` did not move; the client-side load generator reported `failures=0` at
every level for every arm.

*One caveat, stated because it was checked rather than assumed:* LiteLLM's server log
contains a handful of connect errors, all timestamped **10 seconds after the final
measurement ended** — they occur during teardown, when the harness stops the mock while
LiteLLM still has a backlog queued. An earlier run of this benchmark had to be discarded
because equivalent errors occurred *during* measurement; the check is now part of the
runbook.

### Steady-state added latency at 4,096 concurrent streams

This is the cleanest streaming measurement in the repository, because it removes the
variable that distorted everything else: **connection churn.** `streamgen` opens a new
connection per stream, so any run in which streams *complete* is also measuring connect
cost. Here each stream carries 1,000 tokens at 100 ms, so no stream finishes inside the
30 s window and no reconnect happens on any path (`completed_streams = 0`, asserted).

Three runs, 4,096 concurrent streams, one worker, idle states capped at C1:

| path | p50 | p99 | p99.9 |
|---|---|---|---|
| direct (no gateway) | 12-15 us | 21-33 us | 33-48 us |
| llmbridge, epoll | 179-211 us | 505-559 us | 622-786 us |
| **llmbridge, io_uring** | **156-178 us** | **298-413 us** | **507-608 us** |

**io_uring is the faster backend at every percentile in every run** - the opposite of
what a churn-heavy benchmark shows. See "epoll vs io_uring" below.

llmbridge adds **~0.3-0.6 ms at p99 while carrying 4,096 concurrent streams**, against a
33 us no-gateway floor. The gateway is ~17x the floor here and that gap is not yet
explained; it is present on both backends, so it is a gateway-wide question rather than
an io_uring one.

Raw data: `bench/results/stream-steadystate.csv`.

### Connection reuse (and why the earlier numbers were worse)

Until recently a streaming request closed its upstream connection at stream end, so
every stream paid a fresh connect. At 4,096 streams that was **706 connects/sec**, and
it was the single largest term in time-to-first-token on *both* backends. Upstream
connections are now returned to the keep-alive pool when the framing proves it is safe
(chunked body, terminal chunk consumed, no trailing bytes, upstream keep-alive, clean
end); the pool is capped at 256 with 30 s idle eviction.

Measured A/B at 4,096 streams, same load, reuse off -> on:

| | epoll | io_uring |
|---|---|---|
| upstream connects | 17,661 -> **4,330** | 17,661 -> **5,762** |
| TTFT p50 | 178.9 -> **154.4 ms** | 247.1 -> **171.8 ms** |
| per-token p99 | 26.7 -> **2.5 ms** (10.7x) | 149.7 -> **72.4 ms** (2.1x) |

The per-token improvement is far larger than the connect cost alone, because the churn
was loading the whole path - including the provider's accept queue.

### Where llmbridge saturates

**Withdrawn pending re-measurement.** The previous knee ("between 2,048 and 4,096
streams") was measured before connection reuse existed and against a provider that was
itself saturating, so it reflected connect churn and the mock as much as the gateway.
The related claim that epoll degrades more gracefully past the knee is withdrawn too: it
was a single sample, and the churn-free data above shows io_uring ahead at every
percentile.

**Reproduce**

```sh
# Host first: cap idle states, or the numbers are dominated by C-state exit latency.
sudo cpupower idle-set -D 5
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=8192 net.core.somaxconn=8192

./bench/run_stream_headtohead.sh 60 20 20 6 "16 64 256 512"   # vs LiteLLM
./bench/run_stream_cpu.sh 20 5                                 # CPU/token + saturation
python3 bench/make_stream_chart.py

# Steady-state latency (no connection churn): 1000 tokens/stream so no stream
# completes inside the window. This is the configuration the p99 table above uses.
build-linux/bin/faststream --port 9601 --tokens 1000 --token-interval-us 100000 --prefill-us 100000 &
build-linux/bin/llmbridge --listen 8088 --upstream 127.0.0.1:9601 --translate anthropic --workers 1 --io uring &
for i in 1 2 3 4; do build-linux/bin/streamgen --port 8088 --streams 1024 --duration 30 --warmup 5 & done
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
10. **Connection churn is stated, not hidden.** `streamgen` opens a new connection per
    stream, so a run in which streams complete measures connect cost as well as
    streaming. This is not a detail: it is the difference between io_uring looking 32x
    worse than epoll and 1.5x better. Streaming latency is therefore reported from a
    churn-free configuration (streams long enough that none complete in the window), and
    any figure taken with churn says so.
11. **The harness must not be the limiter — including on the connect path.** The
    provider-saturation guard (8) watches token delivery, but it does not watch the
    accept queue. A host whose SYN backlog overflows will refuse connects, and it will
    do so unequally between a gateway that pools upstream connections and one that does
    not. Check `nstat | grep ListenOverflows` before trusting a head-to-head.

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

The obvious question about any proxy benchmark: **is the added latency the gateway's own
work, or just the price of inserting a process into the path?** `bench/nullrelay.cpp`
answers it - a proxy that does nothing but `read()` and `write()` bytes: no HTTP parsing,
no chunked decode, no translation, no per-chunk state.

At 64 concurrent streams, p50 added latency, measured at two host tuning levels:

|                                        | C-states default | C-states capped at C1 |
|---|---|---|
| direct - 1 hop, no proxy               | 53 us | 13 us |
| **nullrelay** - 2 hops, **zero work**  | 106 us | 27 us |
| llmbridge - 2 hops, full translate     | 108 us | 32 us |
| **=> cost of the HOP itself**          | **53 us** | **14 us** |
| **=> cost of ALL of llmbridge's work** | 2 us (noise) | **5 us** |

Two things follow, and the second one is a host-configuration result, not a code result:

1. **llmbridge's own work costs ~5 us/token** - HTTP framing, chunked decode, the SSE
   state machine and re-serialisation combined. Everything else is the price of putting a
   process in the path. Every gateway sits at ~5% of one core, so none of it is
   compute-bound. The honest ceiling on optimising *our code* here is therefore ~5 us.
   (An earlier revision of this document said ~2 us; that was inside the noise floor of
   an untuned host, and the tuned measurement supersedes it.)
2. **The hop is mostly CPU idle-state exit latency, not transit.** Capping idle states at
   C1 (2 us exit, versus 70-890 us for C3-C10) cuts it 3.9x. Of the 14 us that remains,
   busy-polling the relay recovers only ~3 us, so the residual is the loopback data path
   rather than this process's own wakeup.

See [`bench/LATENCY-TUNING.md`](bench/LATENCY-TUNING.md) for the three ways to configure
this, and for why the *aggressive* setting backfires: disabling C1 as well leaves the
cores spinning in POLL, which drove this laptop to 94 C and thermal throttling and made
latency **worse** (13 us -> 17 us floor).

**Host state for the streaming numbers in this document:** idle states capped at C1
(`cpupower idle-set -D 5`), `performance` governor, turbo enabled. The reference box is a
mobile i7-9750H that reaches TjMax under sustained load, so absolute figures are a
throttled-laptop floor; the relative comparisons are the point.

### epoll vs io_uring

The short version: **io_uring is faster in steady state and slower under connection
churn**, and which one a benchmark reports depends entirely on whether streams are being
opened and closed while it runs.

**Steady state (no churn), 4,096 streams** - io_uring wins at every percentile in three
of three runs: p99 298-413 us versus epoll's 505-559 us (full table above).

**Syscalls** - traced with `strace -c` at 64 streams:

| backend | total syscalls | **per delivered token** |
|---|---|---|
| epoll | 90,447 (32.7k `read` + 32.7k `write` + 15.2k `epoll_wait` + 2.3k `epoll_ctl`) | **3.57** |
| io_uring | 31,970 (26.5k `io_uring_enter`) | **1.26** |

io_uring issues **2.31 fewer syscalls per token**. A syscall costs ~537 ns on this box
(measured), so the whole theoretical saving is ~1.24 us/token - under 4% of the added
latency, and below run-to-run variance. It is therefore invisible on the latency axis and
shows up instead as **CPU efficiency**: 10.5 versus 12.4 us of CPU per token at 1,024
streams, which is where the throughput headroom comes from.

**Connection churn is where io_uring loses.** `IORING_OP_CONNECT` measured **43.9 ms
mean with 2,133 connects above 100 ms**, against epoll's 18.9 ms mean and **zero** above
100 ms, over 17,661 connects each. Under a churn-heavy workload that inverts the result
completely - io_uring's p99 was 81 ms versus epoll's 2.5 ms. Two contributors are
confirmed: the connect latency above, and `IORING_SETUP_DEFER_TASKRUN` (disabling it
halves the excess inter-arrival gaps and cuts gaps beyond 3x the token interval by ~25x,
n=3). A residual remains unexplained.

Since connection reuse landed, steady-state operation is the common case and io_uring
remains the default. A deployment with very high connection churn and no reuse should
measure both.

**Hypotheses that were tested and are NOT the cause** of the churn-time io_uring tail,
recorded so they are not re-investigated: provided-buffer exhaustion (`ENOBUFS` counter
0, multishot terminations 0, and an 8x larger pool changed nothing), CQ-ring overflow
(kernel overflow counter 0; an 8x larger CQ changed nothing), send serialisation (never
once blocked on an in-flight send), event-loop turnaround (io_uring's loop is 5.5x
*faster* per iteration than epoll's), a slow client (splitting the load generator across
four processes improved the *baseline* 55x and left both gateways unchanged), the
gateway's own 200 ms tick (a 32x sweep of it moved nothing), TCP retransmission and
netdev backlog drops (epoll suffers *more* of both while performing better), Nagle
(`TCP_NODELAY` is set on all four socket paths), and thermal throttling (arms were
interleaved and ran at identical clocks).

### Known limitations

- **Single co-located host.** No TLS, no WAN, no NIC. Absolute tails are an upper
  bound; the *relative* comparison is the point.
- **Localhost provider**, not a real LLM endpoint (the gateway cannot yet terminate TLS
  to `api.anthropic.com` — see the roadmap).
- **One worker per gateway.** Both scale out; neither was given the chance to.
- **LiteLLM percentiles above 2 s are not resolved** — the generator's histogram tops
  out at 2 s and says so. llmbridge and the direct baseline never overflowed it.
- **The reference box thermally throttles**, which penalises the CPU-bound competitor far
  more than the C++ gateway. See the caveat under the results table.
- **Per-token latency is dominated by the HOP, not by the gateway's work** (~5 us of
  ~32 us) - measured, not assumed. This bounds how much further *our code* can move the
  streaming path.
- **"Added" p99 figures are a difference of percentiles**, not a percentile of the
  difference: the harness reports `p99(through-path) - p99(direct)`. That is a usable
  estimate but not a true percentile, and at low added latency it can even come out
  below the corresponding p50. Treat small "added p99" values as indicative.
- **Connection churn changes which backend looks better.** Any streaming benchmark in
  which streams complete is also measuring connect cost; see "epoll vs io_uring".
