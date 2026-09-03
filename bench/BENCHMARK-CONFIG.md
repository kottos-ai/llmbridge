# Benchmark configuration reference

Every knob that changes a published number, and which configuration produced which
number. **Read this before running or quoting any benchmark.**

This document exists because configuration (not code) invalidated four separate
results in a single session:

| what was undocumented | what it did |
|---|---|
| `BACKENDS=1` instead of `4` | mock capped throughput at ~65k, looked like a 30% gateway regression |
| C-states left at default | added ~40 µs to every low-load latency figure |
| `tcp_max_syn_backlog=1024` | mock refused connects; **only** penalised the competitor, which does not pool |
| connection churn in the load generator | inverted the epoll-vs-io_uring result (io_uring 32× worse → 1.5× better) |

None of these produce an error. They produce a *plausible wrong number*.


## 0. Cold-boot runbook (start here)

Two benchmark families, two boots. **Reboot between them**; this box needs ~30 min of
idle to shed the heat a benchmark generates, and a reboot is faster and more reliable
than waiting.

### Both boots: do this first, every time

```sh
# 1. Close the IDE (CLion idles at ~13% of a core and re-indexes after every build).
# 2. The sysctls the benchmarks require:
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=8192 net.core.somaxconn=8192
sudo sysctl -w net.ipv4.ip_local_port_range="10000 65535"

# 3. Confirm the machine is actually idle and cool before starting:
awk '{print "load: "$1}' /proc/loadavg                             # want < 0.2
cat /sys/class/thermal/thermal_zone*/temp | sort -rn | head -1     # want < 60000 (60 C)
nstat -az | grep ListenOverflows                                   # note the number
```

**Set the governor to `performance`. Do not cap idle states.** The two are not
equivalent, and the difference is fairness:

```sh
sudo cpupower frequency-set -g performance     # DO, helps both sides equally
# sudo cpupower idle-set -D 5                  # DON'T for competitive runs (see below)
```

| setting | effect on llmbridge | effect on LiteLLM | verdict |
|---|---|---|---|
| `performance` governor | throughput 79k -> **85k**; p99 slightly better | capacity unchanged (~246 req/s), latency at 100 RPS **improves** 104 -> 87 ms | **fair, use it** |
| `idle-set -D 5` | latency p99 66 -> 37 us | capacity **247 -> 150 req/s** | **unfair, exclude** |

`performance` helps because `intel_pstate`'s `powersave` ramps frequency reactively, and
with 5s measurement levels that ramp is a meaningful fraction of the run. It does not
heat the machine the way disabling idle states does, so it does not cost the CPU-bound
competitor its turbo headroom.

### Boot 1: non-streaming

```sh
sudo cpupower frequency-set -g performance
BACKENDS=4 ./bench/saturate.sh 5 2 90000 130000        # throughput ceiling
./bench/run_headtohead.sh 200 15 4 100 250 500 1000 2000 5000   # latency vs LiteLLM
```

**Run saturation first and cold.** Throughput falls as the box heats even within a
session: 86,982 -> 84,928 -> 82,380 across three consecutive runs as the package went
57 -> 79 -> 85 C.

### Boot 2: streaming

```sh
./bench/run_stream_headtohead.sh 60 20 20 6 "16 64 256 512"     # vs LiteLLM

# Then, optionally, the llmbridge-only steady-state latency table. This one MAY use the
# idle-state tuning, because there is no competitor in it to disadvantage:
sudo cpupower idle-set -D 5
#   ... run the churn-free config from section 3 ...
sudo cpupower idle-set -E        # ALWAYS restore
```

### After every run

```sh
nstat -az | grep -E 'ListenOverflows|TcpAttemptFails'   # both must be UNCHANGED
grep -i overflow bench/results/*.log                    # histogram overflow => not percentiles
grep -ci 'cannot connect' bench/results/*litellm*.log   # competitor connect failures => void
```

### Ephemeral ports: the third harness limit that only bites the competitor

`ip_local_port_range` defaults to `32768 60999` = **28,232 ports**. A streaming
head-to-head puts **32,673 sockets into TIME_WAIT**, which exceeds it, and connects then
fail with `TcpAttemptFails`. As with the SYN backlog, this is **asymmetric**: llmbridge
pools its upstream connections and creates almost none, while a gateway that opens a
fresh connection per request exhausts the range. A run in that state reports the
competitor's connect failures as if they were its latency.

Measured: 20 LiteLLM connect failures *during measurement* with the default range and
`ListenOverflows` at zero; i.e. the accept queue was fine and the ports were not.

```sh
sudo sysctl -w net.ipv4.ip_local_port_range="10000 65535"   # 55,536 ports
```

### The fourth: a listen port inside the widened ephemeral range

The fix above has a sting in its tail. Widening `ip_local_port_range` to `10000 65535`
means **any listen port at or above 10000 can be stolen** by a lingering client socket
from a previous run. `bind()` then fails and the gateway exits with
`failed to bind listen port`.

Measured: an A/B harness using listen ports 19xxx/20xxx had roughly half its runs die
this way, *intermittently*, depending on whether that particular port happened to be in
use. Because the harness recorded the dead runs as `achieved=0`, the first reading was
"the new build crashes under load", which was wrong in both directions: nothing crashed,
and the build was fine. The gateway aborting when it cannot bind is correct behaviour.

**Rule: pick benchmark listen ports below 10000** (or whatever
`/proc/sys/net/ipv4/ip_local_port_range` starts at) and check before choosing:

```sh
cat /proc/sys/net/ipv4/ip_local_port_range   # listen ports must be below the low bound
```

A harness must also **verify the process is alive after starting it** and fail loudly
instead of recording a zero, a silent `achieved=0` reads like a catastrophic
regression and costs an hour of chasing a bug that does not exist.

Four separate host limits have now produced the same failure mode (mock listen backlog,
SYN backlog, ephemeral-port exhaustion, and a listen port inside the ephemeral range).
**Assume there is a fifth**: always verify the competitor recorded zero connect failures
before trusting a head-to-head, and verify the *control* held the offered rate before
trusting any delta.

### Why no `idle-set -D 5` for the competitive runs

Capping idle states keeps the machine ~20 C hotter, which costs turbo headroom, and it
penalises the CPU-bound competitor far more than it penalises us:

| | with `-D 5` | **without** |
|---|---|---|
| llmbridge throughput | 75k RPS | **85k RPS** |
| llmbridge added p99 @ 250 RPS | **37 us** | 63 us |
| **LiteLLM capacity** | 150 req/s | **246 req/s** |

It makes our latency look better *and* LiteLLM's capacity look 65% worse. Any number
published from that configuration alongside a competitor is indefensible.

**The governor is different** and was initially lumped in with it by mistake: measured
separately, `performance` improves *both* sides and leaves LiteLLM's capacity intact, so
it is fair to use. The distinction is heat; the governor does not stop cores sleeping,
so it does not drive the package toward TjMax.


## 1. Host prerequisites

**The idle-state / governor tuning is per-benchmark, not global; applying it to the
throughput benchmark makes the result worse.** See the table below.

```sh
# ALWAYS: listen/SYN queues. The load generators open a connection per stream; the
# defaults overflow above ~256 concurrent and the kernel silently drops SYNs.
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=8192 net.core.somaxconn=8192

# STREAMING LATENCY ONLY: cap idle states at C1 so wakeups do not pay a deep
# C-state exit (70-890 us). Revert with `sudo cpupower idle-set -E` afterwards.
sudo cpupower idle-set -D 5
sudo cpupower frequency-set -g performance
```

| benchmark | idle-state tuning | why |
|---|---|---|
| **streaming latency** | **apply `-D 5`** | client-measured; a deep C-state exit adds ~40 us to every token. The gateway runs at ~5% of a core, so the extra heat is tolerable. |
| **non-streaming latency** | **irrelevant** | llmbridge self-measures from client-request-received onward, so the wakeup is outside the measurement. Verified: 76 us p99 tuned vs 67 us untuned, no improvement. |
| **throughput / saturation** | **do not apply** | at saturation the CPU never idles, so C-states are never entered, but the tuning keeps the machine ~20 C hotter at idle, which caps turbo. **Measured: 81.6k mean achieved with defaults versus ~75k with the tuning applied, on the same host in the same session.** |

**The tuning heats the machine.** `idle-set -D 5` prevents cores sleeping and the
`performance` governor pins them at max clock, so this box idles at ~80 C instead of
~60 C. On a thermally-limited laptop that costs turbo headroom: max clock measured
4002 MHz tuned versus 4289-4396 MHz untuned. Restore defaults between runs:

```sh
sudo cpupower idle-set -E && sudo cpupower frequency-set -g powersave
```

**Do not use `cpupower idle-set -D 0`.** Disabling C1 as well leaves cores spinning in
Poll; on a laptop that drives the package to TjMax and throttles the clock, making
latency *worse* (measured: 13 µs floor → 17 µs). See
[`LATENCY-TUNING.md`](LATENCY-TUNING.md).

**C-states do not affect saturation throughput** directly: at saturation the CPU never
idles, but disabling them *indirectly lowers* it by keeping the machine hot enough to
cap turbo. That is the opposite of the intuition, and it is measured.

### Verify before/after every run; any of these invalidates a result

```sh
# 1. The machine must be IDLE. An IDE indexing in the background is not idle.
ps -eo pcpu,comm --sort=-pcpu --no-headers | head -5
awk '{print "load: "$1}' /proc/loadavg          # want < ~0.1 before starting

# 2. The harness must not be the limiter on the connect path.
nstat -az | grep ListenOverflows                # snapshot before AND after; must not move

# 3. Percentiles must be real percentiles.
grep -i overflow bench/results/*.log            # generator histogram overflow warnings

# 4. Thermal headroom.
cat /sys/class/thermal/thermal_zone*/temp | sort -rn | head -1   # want well under TjMax
```

**Background load is the easiest one to miss.** On the reference box an open CLion idles
at ~13% of a core (more while indexing, which it does after every rebuild), plus
~9% for the terminal/agent and ~6% for Xwayland. That is ~30% of a core of variable
competing work, and it holds the package ~15-20 C above a truly idle floor. **Close the
IDE before measuring.** Numbers taken with it running are usable for A/B comparisons
both arms see the same load, but they understate absolute throughput.

A histogram overflow means the reported "percentile" is really the maximum; it *looks*
like a percentile and is not one. Overflowed values must be reported as "> range", never
as a number.

### Thermal state

The reference box (i7-9750H) reaches TjMax under sustained load and throttles from
4.5 GHz to ~4.0 GHz, costing ~10% of throughput. Record it:

```sh
cat /sys/devices/system/cpu/cpu0/thermal_throttle/package_throttle_count   # delta over the run
grep 'cpu MHz' /proc/cpuinfo | sort -u | tail -1
```

This penalises **CPU-bound competitors far more than llmbridge** (which runs at ~5% of a
core in streaming), so it flatters us. Any competitive claim should be reproduced on a
machine with thermal headroom.


## 2. Non-streaming benchmarks

### Throughput / saturation: `bench/saturate.sh`

```sh
# BACKENDS=4 is THE ONE THAT MATTERS (see the knob table below).
BACKENDS=4 ./bench/saturate.sh 5 2 90000 130000
```

| knob | default | published runs use | why it matters |
|---|---|---|---|
| `BACKENDS` | **1** | **4** | mock backend instances (`SO_REUSEPORT`). At 1 the **mock** is the ceiling (~65k), not the gateway. **The published ~84k figure requires `BACKENDS=4`.** |
| `WORKERS` | 1 | 1 | gateway `SO_REUSEPORT` worker threads. The headline is a *single-thread* ceiling; >1 measures something else. |
| `IO` | `uring` | `uring` | event-loop backend |
| `BODY` | 64 | 64 | request body bytes; 1024 and 8192 were also measured historically |
| arg 1 / arg 2 | 6 / 2 | 5 / 2 | seconds per level / warmup. Longer levels soak the CPU thermally. |
| `BIN` | build dir |. | binary directory; used to A/B two builds on one host |

**Gateway process:** `--workers 1 --io uring --translate anthropic`.

### Latency head-to-head vs LiteLLM: `bench/run_headtohead.sh`

```sh
./bench/run_headtohead.sh 200 15 4 100 250 500 1000 2000 5000
#                         ^lat_ms ^dur ^warmup ^RPS levels
```

| knob | default | notes |
|---|---|---|
| arg 1 `LAT_MS` | 200 | simulated upstream think-time |
| `BODY` | 64 | request body bytes |

**Known asymmetry:** llmbridge is proxy-self-measured (its own request/response-path
stamps); LiteLLM is client-measured (through-LiteLLM minus direct-to-mock), because
LiteLLM cannot self-report. The streaming benchmark does not have this asymmetry.

**Known gap:** `loadgen.cpp` and `fastbackend.cpp` do **not** set `TCP_NODELAY`, so the
harness legs run with Nagle enabled. The gateway's own sockets do set it. Unquantified.


## 3. Streaming benchmarks

### Head-to-head vs LiteLLM: `bench/run_stream_headtohead.sh`

```sh
./bench/run_stream_headtohead.sh 60 20 20 6 "16 64 256 512"
#                                ^tokens ^interval_ms ^dur ^warmup ^levels
```

| knob | default | published | why |
|---|---|---|---|
| arg 1 `TOKENS` | 60 | 60 | deltas per stream |
| arg 2 `INTERVAL` | 20 ms | 20 ms | = 50 tok/s per stream |
| arg 4 `WARMUP` | 6 s | 6 s | generous: LiteLLM's cold start is severe, and a discard round precedes the sweep |
| `IO` | `auto` | `auto` | |
| `PROVIDER` | `fast` | `fast` | `fast` = `faststream` (C++, >100k tok/s). `python` = `mock_provider.py`, which saturates at 45-50k tok/s. **below llmbridge**, so it measures the harness |

### Steady-state latency (the p99 table): no churn

**This is a distinct configuration, not a variant.** `streamgen` opens a new connection
per stream, so a run in which streams *complete* measures connect cost too. Streams are
made long enough that none finishes inside the window:

```sh
build-linux/bin/faststream --port 9601 --tokens 1000 --token-interval-us 100000 --prefill-us 100000 &
build-linux/bin/llmbridge --listen 8088 --upstream 127.0.0.1:9601 --translate anthropic --workers 1 --io uring &
for i in 1 2 3 4; do build-linux/bin/streamgen --port 8088 --streams 1024 --duration 30 --warmup 5 & done
```

Assert `completed_streams=0` in every client log. If it is non-zero, the run includes
churn and is a different measurement.

**Use four client processes, not one.** A single `streamgen` polling 4,096 sockets is
itself the bottleneck: splitting it improved the *no-gateway baseline* 55× (p99 2,649 µs
→ 48 µs) while leaving both gateways unchanged.

### CPU-per-token and saturation: `bench/run_stream_cpu.sh`

```sh
./bench/run_stream_cpu.sh 20 5     # duration, warmup
```

Records temperature, throttle delta and clock per level. **CPU-per-token is only
meaningful below saturation**: once the worker is pegged at 100% of a core, µs-of-CPU per
token is just `budget ÷ tokens delivered`, i.e. the reciprocal of throughput, not an
efficiency measure.


## 3b. Profiling the worker

`perf` is blocked by default on Ubuntu (`perf_event_paranoid=4`). To profile:

```sh
sudo sysctl -w kernel.perf_event_paranoid=1 kernel.kptr_restrict=0
```

`1` (not `2`) is required: at `2` only user space is sampled, and since ~89% of the
worker's CPU is kernel-side the profile would be an opaque `[unknown]` bucket.
`kptr_restrict=0` turns kernel addresses into symbol names.

The worker only starts *after* the direct-baseline phase of `saturate.sh`, so poll for
the pid instead of sleeping a fixed interval.

## 3c. Host software that changes results

| setting | effect | how to check |
|---|---|---|
| **AppArmor** | **~6.3% of worker CPU** on per-send/recv LSM checks | see below. `systemctl stop` does **not** remove it |
| CPU clock | throughput tracks it ~linearly (78k @ 4.0 GHz vs 88k @ 4.5 GHz) | `grep 'cpu MHz' /proc/cpuinfo` |
| `kernel.perf_event_paranoid` | 4 = no profiling at all | see above |

### Disabling AppArmor actually requires a reboot

`sudo systemctl stop apparmor` is **not enough**, verified by profiling: with the
service inactive the hooks still cost 6.37% of the worker's CPU
(`apparmor_socket_recvmsg` 2.37%, `aa_inet_msg_perm` 2.07%,
`apparmor_socket_sendmsg` 1.45%, plus the ip_* hooks). Stopping the service unloads
*profiles*; the LSM itself is built into the kernel and enabled at boot, so its socket
hooks run on every send and recv regardless.

Removing it needs a kernel parameter and a reboot:

```sh
sudo nano /etc/default/grub     # append to GRUB_CMDLINE_LINUX_DEFAULT:  apparmor=0
sudo update-grub && sudo reboot
```

Verify after reboot: the symbols must be absent from a profile of the worker,

```sh
grep -o 'apparmor=0' /proc/cmdline
perf report -i perf.data --stdio --no-children -g none | grep -ci apparmor   # expect 0
```

Revert by removing the token and rebooting. This is a **security tradeoff**, so it is a
benchmarking/measurement step, not a production recommendation. Quantify the cost, then
decide separately whether the exposure is acceptable where it would actually run.

## 3d. A/B runs (regression checks): a different discipline

A regression check is not a publication run. Nobody cares about the absolute number;
the only thing that matters is whether **build A differs from build B**, which makes it
vulnerable to a class of artifact the published runs are not. Everything below exists
because it produced a confident wrong answer at least once.

**Interleave the arms.** `base, cur, base, cur...`, never two blocks. This box's absolute
numbers drift with package temperature over a few minutes, and a block layout converts
that drift straight into a fake delta.

**Gate on temperature before every run.** Refuse to start the next run until the package
is back under a threshold, so no run is measured on a hotter box than its counterpart:

```sh
while [ "$(awk '{printf "%.0f", $1/1000}' /sys/class/thermal/thermal_zone0/temp)" -gt 70 ]; do sleep 2; done
```

**Run the whole thing a second time with the arms swapped.** This is the control that
catches what the gate cannot. A threshold gate still lets the *first* run after a long
idle start genuinely colder (measured: 62 C versus 70 C for every subsequent run), so
whichever arm goes first gets a free advantage. If a delta is real it survives the swap;
if it is position bias it shrinks or inverts.

> Measured example: a streaming A/B showed `cur` +0.30 ms on TTFT, five runs each,
> distributions barely overlapping; it looked like a real regression. Re-run with the
> order reversed, the same gap was **+0.10 ms**, i.e. two thirds of it was the arm that
> happened to run first. Per-token latency, flat in both orderings, was the honest
> signal.

**Report min as well as median.** Thermal noise and scheduler interference only ever
*add* latency, so the minimum across runs is the cleanest estimator of what the code
itself does. A median can be dragged by one spike; the min cannot.

**Stay well under the knee.** Latency percentiles from a saturated open-loop run are
meaningless: the generator queues without bound and you are measuring backlog, not
service time (`p50 == p99 == max` is the tell). Throughput ceilings are measured by
`saturate.sh`; latency deltas are measured at a rate the box holds comfortably, with the
**control confirmed to hold the offered rate** before and after the sweep.

**Verify the process is alive after starting it.** A harness that records a dead run as
`achieved=0` reads like a catastrophic regression. See §"The fourth" above; that is
exactly how an ephemeral-port collision was misread as "the new build crashes".

> What this discipline is worth: without it, a genuine **+40 us p99** regression was
> invisible under thermal noise of 4,000-359,000 us and was reported as "no regression".
> With it, the same regression showed up as six non-overlapping runs, was traced to six
> redundant header scans per request, fixed, and confirmed gone.

## 4. Gateway flags

```
--listen PORT           --upstream IP:PORT | HOST:PORT | http://HOST[:PORT]
--io auto|epoll|uring   --translate none|anthropic|gemini|cohere
--workers N             --upstream-timeout SECONDS
--duration SECONDS      --warmup SECONDS
```

`--upstream` also parses `https://HOST[:PORT]` but refuses to start until the TLS
transport is wired into the gateway loop. Hostnames resolve once at startup
(first A record); all benchmarks use the literal `127.0.0.1:PORT` form, which
bypasses the resolver entirely.

```
```

Compile-time constants that change benchmark results:

| constant | value | note |
|---|---|---|
| `kMaxIdleUpstreams` | 8192 | **Do not lower without re-running `saturate.sh BACKENDS=4`.** At 256 it stopped being a bound and became a reuse killer: 32,210 achieved vs 77,282. git-bisected. |
| `kIdleUpstreamNs` | 30 s | idle pooled-upstream eviction; the real reclaim mechanism |
| `kUrStreamBufCap` | 8 MiB | per-stream output cap (io_uring back-pressure) |
| `kUrBufCount` / `kUrBufSize` | 4096 × 4 KiB | io_uring provided-buffer pool. Measured irrelevant to the streaming tail; an 8× increase changed nothing. |


## 5. Provenance of published numbers

| claim | configuration required |
|---|---|
| ~84k RPS single-thread ceiling | `BACKENDS=4 WORKERS=1 IO=uring BODY=64`, 5s/2s levels, cool host |
| streaming p99 at 4,096 streams | churn-free config above, 4 client processes, C1-capped host |
| llmbridge vs LiteLLM streaming | `60 20 20 6 "16 64 256 512"`, `PROVIDER=fast`, SYN backlog raised, `ListenOverflows` delta 0 |
| per-token cost ~5 µs | `nullrelay` control at 64 streams, C1-capped host |

## Competitor versions (pinned)

The published LiteLLM figures were produced with these exact versions. A plain
`pip install litellm[proxy]` no longer reproduces them: a newer LiteLLM pulls a
FastAPI that removed `get_flat_dependant`, and its proxy then fails to start with
a misleading `No module named 'proxy_server'`.

```sh
python3 -m venv bench/.litellm-venv
bench/.litellm-venv/bin/pip install "litellm[proxy]==1.95.0" \
                                    "fastapi==0.115.14" "sse-starlette==2.1.3"
```

Verify before a run: the proxy must answer `GET /health/liveliness` with 200.

### The pin is a liability as well as a guarantee (2026-09-02)

1.95.0 is what the published figures were measured against, and it stays pinned so
they stay reproducible. It is also **five releases behind**: PyPI is on 1.99.0, and
LiteLLM has announced an optional sidecar that moves the hot path out of Python,
claiming 1,000 QPS without failures and up to 5,000 on 4 CPU and 8 GB. Our published
figure is a ~246 RPS ceiling on one worker. Both cannot be true of the same software.

Read their post carefully before repeating either number. It states a Q1 **goal** of
"sub-millisecond proxy overhead" and reports **no overhead measurement and no version**,
so the only concrete claim in it is throughput.

`bench/rerun_litellm.sh` measures the current release in a separate venv and leaves the
pinned one alone.

### What 1.99.0 actually contains (inspected 2026-09-02)

The install now succeeds unpinned. FastAPI 0.141.1 and litellm 1.99.0 start the proxy
fine, so the `get_flat_dependant` caveat above describes a window that has closed. The
1.95.0 pin stays for reproducibility, not because a newer one cannot run.

The re-architecture ships, under a different name than the post uses. There is no file
in the package mentioning a sidecar; there is a compiled `litellm/rust_bridge/_native.abi3.so`
and 35 modules referencing it. Searching for the blog's vocabulary returns nothing and
is how a first pass concluded the feature was absent.

Three properties of that path decide what has to be re-measured, all read from
`rust_bridge/chat_completions.py` in the installed package:

| property | value | consequence for our claims |
|---|---|---|
| gate | `LITELLM_RUST` in `{1,true,yes,on}`, or `litellm_params["rust"] is True`; default empty | off unless asked for, so the default arm is still the Python path a user gets |
| providers | `frozenset({"anthropic", "bedrock"})` | covers our head-to-head exactly, which uses the anthropic provider |
| streaming | `if stream: return False` | **the streaming numbers need no re-measurement**: this path serves no streamed request |

So the streaming results, including 99.93-100% delivery against LiteLLM's 4% at 512
concurrent, are unaffected by the Rust work regardless of how the non-streamed arm comes
out. The non-streamed comparison needs both arms, default and `LITELLM_RUST=1`, or it
measures a configuration the vendor would fairly object to.

### First fit measurement of 1.99.0 (2026-09-02, provisional)

A second run on a quiet host (load 0.72, no IDEs) put **llmbridge's own p99 at 0.085 ms
against the published 0.080 ms**, a 6% control drift, so the comparison is readable.
Single run, not the median of three, and the host still carried a desktop session, so
these are provisional pending a cold-boot median.

At 100 RPS, the only unsaturated point:

| | llmbridge p99 | LiteLLM added p50 | LiteLLM added p99 | ratio on p99 |
|---|---|---|---|---|
| 1.95.0, published, cold, median of 3 | 0.080 ms | 4.490 ms | **87.060 ms** | ~1,090x |
| 1.99.0 default (Python path) | 0.085 ms | 4.690 ms | **30.450 ms** | **~360x** |
| 1.99.0 with `LITELLM_RUST=1` | 0.080 ms | 4.410 ms | **23.690 ms** | **~300x** |

**The median did not move; the tail did.** Added p50 is 4.49, 4.69 and 4.41 ms across all
three, statistically one number. The p99 fell from 87 ms to 24-30 ms, so the work landed
on tail latency.

**Consequence for the public claim: "~1,000x versus LiteLLM" is no longer defensible.**
Measured like for like at 100 RPS it is now roughly 360x against the default configuration
and 300x against the fastest one the release offers.

**The throughput claim in their post is not reproduced.** Single-worker saturation measured
217 to 235 req/s, against ~246 published for 1.95.0, so the ceiling is unchanged inside
run-to-run noise. Their post claims 1,000 QPS without failures and up to 5,000 on 4 CPU and
8 GB, which is a multi-worker configuration; our harness runs one worker, as the published
baseline did.

The Rust path was verified to actually serve the request, instead of being assumed from
the environment variable: with `LITELLM_RUST=1` the response carries `x-litellm-rust: true`,
and without it the header is absent. Check that header before attributing a number to it.

### Independent corroboration, and a first Bifrost number (2026-09-02)

A third-party benchmark published 2026-06-26 by the author of GoModel measures four
gateways on an AWS c7i.large against an instant mock, 8,000 requests at concurrency 10,
two trials in randomised order, throughput from a separate saturation sweep. Harness:
github.com/ENTERPILOT/ai-gateway-reproducible-benchmark.

| gateway | runtime | p50 added | p99 added | sustained | peak RAM |
|---|---|---|---|---|---|
| GoModel | Go | 1.8 ms | 6.9 ms | 4,900 req/s | 37 MB |
| Bifrost | Go | 2.5 ms | 18.3 ms | 3,100 req/s | 143 MB |
| Portkey | Node | 9.7 ms | 30.5 ms | 950 req/s | 112 MB |
| LiteLLM | Python | 30.6 ms | 39.3 ms | 324 req/s | 2.3 GB |

**It corroborates today's correction.** They measure 39.3 ms added p99 on different
hardware with a different harness; we measured 30.45 ms. Both sit near 30 ms, and our
published 87 ms is the outlier, which is what a version improvement looks like from two
directions at once.

**It is also our first Bifrost measurement, and it contradicts their marketing.**
Bifrost claims roughly 11 microseconds of overhead at 5,000 RPS; a third party measures
p50 2.5 ms and p99 18.3 ms. Treat neither as settled and run the harness ourselves.

### Reconciling ~84k with ~30k in the third-party harness (2026-09-02)

The ENTERPILOT harness measures one llmbridge worker at roughly 30,000 req/s, against
our published ~84k single-thread ceiling. Both are correct. They differ by four factors,
three of them measured on one box in one session, and none of them is a defect.

| | measured | note |
|---|---|---|
| our loadgen to our fastbackend, direct | 114,278 | the backend is not the limit |
| our loadgen to llmbridge to our fastbackend | 69,308 | one backend, matches the ~65k this file already documents |
| our loadgen to their Go mock, direct | 79,053 | their mock alone |
| our loadgen to llmbridge to their Go mock | 41,074 | swapping only the backend costs 41% |
| the same, containerised, default seccomp | 32,785 | containerisation costs 20% |
| the same, seccomp and apparmor off | 38,528 | 18% of that is the syscall filter |
| the same, plus `--cpuset-cpus` pinning | 41,375 | matches native; container overhead fully recovered |

**Factor 1, backend count.** The published 84k needs `BACKENDS=4`; at one backend this
file already says the mock becomes the ceiling at ~65k, and today's run measured 69,308.
The third-party harness runs a single mock process. A third of the gap was never a
mystery, it is in the table above under `BACKENDS`.

**Factor 2, the backend itself.** Same gateway, same generator, only the mock swapped:
69,308 to 41,074. Their Go mock returns **439 bytes against our fastbackend's 228**, and
a byte-forwarding proxy's cost scales with bytes. Their mock is not saturating (79,053
direct), so this is the gateway doing more work per request. Payload size is the leading
explanation and not an isolated one: the two mocks differ in implementation as well as
response size, and nothing here varied payload alone.

**Factor 3, containerisation, and it is recoverable.** A container costs 20%, of which
**18 points are seccomp**: the filter is evaluated on every syscall, and this workload is
89% kernel time, so a per-syscall tax lands right on it. The remaining ~7% is the
scheduler migrating the worker between cores. `--security-opt seccomp=unconfined` plus
`--cpuset-cpus` restores native throughput exactly.

### The filters were not a tax, they disabled io_uring (2026-09-02)

Docker's default seccomp profile **blocks the io_uring syscalls**, so llmbridge inside a
default container silently selects the epoll fallback. Read off its own startup line:

```
default seccomp:     backend requested=auto active=epoll
seccomp unconfined:  backend requested=auto active=io_uring
```

Everything that harness has measured for llmbridge was therefore **epoll**, not the
backend this project defaults to on Ubuntu 24.04. The memory jump on the unconfined run,
15.1 to 32.3 MB, is the io_uring rings, and it was the tell: a syscall filter cannot cost
memory. An earlier note here called the 18% a per-syscall tax. It is not; it is a
different I/O backend, and the correction matters because the two have different
explanations and different fixes.

Measured effect, single worker, same box, `--force` passes with a desktop session
running, so read the ratio and not the absolutes:

| | epoll (default seccomp) | io_uring (unconfined) |
|---|---|---|
| peak sustained | 29,723 | 33,881 |
| no-gateway baseline that day | 66,846 | 60,813 |
| share of the path's ceiling | 44.5% | **55.7%** |
| memory, peak | 15.1 MB | 32.3 MB |
| knee | c=16 | c=32 |

**This is a deployment fact, not only a benchmark fact.** Anyone running llmbridge in a
container with default seccomp gets epoll. That belongs in the deployment documentation,
and the gateway should arguably say so louder than an INFO line, since `--io auto`
downgrading to epoll is invisible to an operator who did not think to look.

**The harness is good, and this is not a criticism of it.** Five trials in randomised
order, throughput swept separately from latency, per-dialect warmup, a no-gateway
baseline recorded, reproducible from a public repository, and its author states his
scope honestly. The seccomp cost is Docker's default, inherited by any containerised
benchmark, not a design error.

### The full nine-arm run, and what its streaming column measures (2026-09-03)

One run, one floor, one worker, seccomp off for every arm so llmbridge runs io_uring,
five trials at concurrency 10. Chat non-streaming:

| gateway | cores | added p50 | above floor | peak rps | memory | startup | rps/CPU% |
|---|---|---|---|---|---|---|---|
| tcprelay, the floor | 2.17 | 0.25 | 0 | 44,233 | 6.5 MB | 0.30 s | 158.5 |
| llmbridge | 0.99 | 0.25 | 0.000 | 38,236 | 32.0 MB | 0.29 s | 335.9 |
| llmbridge-anthropic | 1.04 | 0.34 | 0.090 | 30,670 | 31.9 MB | 0.30 s | 246.3 |
| gomodel | 6.01 | 0.65 | 0.400 | 16,380 | 68.6 MB | 0.54 s | 22.7 |
| bifrost | 7.86 | 0.95 | 0.700 | 10,049 | 495.6 MB | 11.41 s | 10.9 |
| portkey | 1.20 | 7.71 | 7.460 | 1,250 | 187.6 MB | 0.98 s | 10.1 |
| litellm | 8.05 | 10.98 | 10.730 | 876 | 9,528 MB | 39.30 s | 0.9 |
| tensorzero | 0.16 | 41.92 | 41.670 | 6,537 | 126.2 MB | 0.51 s | 16.6 |
| omniroute | 1.07 | 171.70 | 171.450 | 58 | 931.4 MB | 5.95 s | 0.5 |

Non-streaming lands **at the floor**, 0.000 ms above a byte pipe that parses nothing, on
0.99 cores against its 2.17.

**The cores column is not optional.** GoModel and Bifrost are not multi-threaded by
accident of Docker: the Go runtime takes `GOMAXPROCS` = every core. Measured directly,
outside the harness, GoModel consumed **6.11 cores non-streaming and 4.42 streaming**.
Quoting throughput without cores flatters them and understates us by roughly an order of
magnitude.

### The streaming column measures connection churn, not streaming

Streaming looks like our weakest row (0.74 ms above the floor against GoModel's 0.42).
Before optimising anything against it, know what it is. Syscalls counted under strace
over 1,000 requests, one worker, io_uring:

| | `io_uring_enter` | `setsockopt` | `shutdown` | `close` |
|---|---|---|---|---|
| non-streaming | 1,634 | 11 | 9 | 20 |
| streaming | 2,261 | 1,007 | 1,005 | 1,016 |

**But the churn is not the cost here.** Accounting for the 137.8 us gap over 34 events:
roughly 10 us of extra syscalls and handshake, ~1 us of scans measured in isolation, and
**~125 us, about 3.7 us per event, still unattributed**.

### Why this does not contradict the 40k-stream claim

The two benchmarks measure opposite things and neither is wrong.

**The mock does not pace its tokens.** It flushes 35 SSE events back to back with no
sleep, so a stream completes in microseconds and the test is an event-throughput and
connection-churn test at concurrency 10. Our own streaming benchmark holds thousands of
concurrent streams at a realistic ~20 ms inter-token interval and measures time to first
token and token delivery. At 512 concurrent streams that is ~25,600 tokens/s; this
harness pushes ~159,000 events/s through a single core. Six times the event rate on a
sixth of the cores, and a connection teardown on every one.

Per core, streaming: llmbridge **5,531 req/s/core** (5,227 on 0.94 cores) against
GoModel's **1,505** (6,650 on 4.42). It buys 27% more absolute throughput with 4.7x the
cores.

### What the per-chunk scans actually cost, measured in isolation

Measured directly, 187-byte chunk, at 4.19 GHz:

| | ns/call |
|---|---|
| `sse_carries_thinking`, three misses | 216.2 |
| `sse_carries_first_token`, hits | 65.3 |
| the two-`find` usage gate | 13.0 |
| append + trim to exactly 2 KiB, the old way | 19.8 |
| append + amortised trim, the new way | 5.8 |

About **11 us per stream before the fixes and 1 us after**, against 180 us of CPU per
stream. So the scans were ~6% of it and the fixes are worth ~1.5%. They remain correct
(an unbounded scan that cannot succeed; a memmove quadratic in chunks) but they are not
where streaming cost lives.

## Inbound TLS arm (added 2026-08-12)

`streamgen` gained `--tls --ca FILE`, the only client in this tree that can drive
a `--listen-tls` gateway. Certificate and hostname verification are on and there
is no way to turn them off: a benchmark client that skips verification is one
somebody copies into production.

**The comparison has to be gateway-against-gateway, not gateway-against-control.**
The usual streaming benchmark differences the gateway arm against a no-gateway
control, and that control cannot exist here: with no gateway there is nobody to
terminate the client's TLS, and pointing the client at a TLS mock instead would
measure the mock's TLS stack. The well-formed question is what the inbound
transform costs, so both arms are the same binary against the same mock, one with
`--listen-tls` and one without:

```sh
# same gateway, same mock, TLS on the listener as the only variable
./build-tls/bin/faststream --port 9401 &
./build-tls/bin/llmbridge --listen 9410 --upstream 127.0.0.1:9401 &
./build-tls/bin/llmbridge --listen 9411 --upstream 127.0.0.1:9401 \
    --listen-tls --tls-cert CERT --tls-key KEY &

./build-tls/bin/streamgen --port 9410 --streams 512 --duration 60 --warmup 20 --label plain
./build-tls/bin/streamgen --port 9411 --streams 512 --duration 60 --warmup 20 \
    --tls --ca CERT --label tls
```

Cold-booted host, median of three, per section 3 of this document. Nothing from a
warm or busy box is quotable, and the load generator now pays TLS costs of its
own, so it must be watched for becoming the bottleneck before the gateway does.
