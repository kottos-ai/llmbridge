# Benchmark configuration reference

Every knob that changes a published number, and which configuration produced which
number. **Read this before running or quoting any benchmark.**

This document exists because configuration — not code — invalidated four separate
results in a single session:

| what was undocumented | what it did |
|---|---|
| `BACKENDS=1` instead of `4` | mock capped throughput at ~65k, looked like a 30% gateway regression |
| C-states left at default | added ~40 µs to every low-load latency figure |
| `tcp_max_syn_backlog=1024` | mock refused connects; **only** penalised the competitor, which does not pool |
| connection churn in the load generator | inverted the epoll-vs-io_uring result (io_uring 32× worse → 1.5× better) |

None of these produce an error. They produce a *plausible wrong number*.

---

## 0. Cold-boot runbook (start here)

Two benchmark families, two boots. **Reboot between them** — this box needs ~30 min of
idle to shed the heat a benchmark generates, and a reboot is faster and more reliable
than waiting.

### Both boots — do this first, every time

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

**Set the governor to `performance`. Do NOT cap idle states.** The two are not
equivalent, and the difference is fairness:

```sh
sudo cpupower frequency-set -g performance     # DO  — helps both sides equally
# sudo cpupower idle-set -D 5                  # DON'T for competitive runs (see below)
```

| setting | effect on llmbridge | effect on LiteLLM | verdict |
|---|---|---|---|
| `performance` governor | throughput 79k -> **85k**; p99 slightly better | capacity unchanged (~246 req/s), latency at 100 RPS **improves** 104 -> 87 ms | **fair — use it** |
| `idle-set -D 5` | latency p99 66 -> 37 us | capacity **247 -> 150 req/s** | **unfair — exclude** |

`performance` helps because `intel_pstate`'s `powersave` ramps frequency reactively, and
with 5s measurement levels that ramp is a meaningful fraction of the run. It does not
heat the machine the way disabling idle states does, so it does not cost the CPU-bound
competitor its turbo headroom.

### Boot 1 — non-streaming

```sh
sudo cpupower frequency-set -g performance
BACKENDS=4 ./bench/saturate.sh 5 2 90000 130000        # throughput ceiling
./bench/run_headtohead.sh 200 15 4 100 250 500 1000 2000 5000   # latency vs LiteLLM
```

**Run saturation first and cold.** Throughput falls as the box heats even within a
session: 86,982 -> 84,928 -> 82,380 across three consecutive runs as the package went
57 -> 79 -> 85 C.

### Boot 2 — streaming

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

### Ephemeral ports — the third harness limit that only bites the competitor

`ip_local_port_range` defaults to `32768 60999` = **28,232 ports**. A streaming
head-to-head puts **32,673 sockets into TIME_WAIT**, which exceeds it, and connects then
fail with `TcpAttemptFails`. As with the SYN backlog, this is **asymmetric**: llmbridge
pools its upstream connections and creates almost none, while a gateway that opens a
fresh connection per request exhausts the range. A run in that state reports the
competitor's connect failures as if they were its latency.

Measured: 20 LiteLLM connect failures *during measurement* with the default range and
`ListenOverflows` at zero — i.e. the accept queue was fine and the ports were not.

```sh
sudo sysctl -w net.ipv4.ip_local_port_range="10000 65535"   # 55,536 ports
```

Three separate host limits have now produced this same failure mode (mock listen backlog,
SYN backlog, ephemeral ports). **Assume there is a fourth**: always verify the competitor
recorded zero connect failures before trusting a head-to-head.

### Why no `idle-set -D 5` for the competitive runs

Capping idle states keeps the machine ~20 C hotter, which costs turbo headroom — and it
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
it is fair to use. The distinction is heat — the governor does not stop cores sleeping,
so it does not drive the package toward TjMax.

---

## 1. Host prerequisites

**The idle-state / governor tuning is per-benchmark, not global — applying it to the
throughput benchmark makes the result WORSE.** See the table below.

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
| **non-streaming latency** | **irrelevant** | llmbridge self-measures from client-request-received onward, so the wakeup is outside the measurement. Verified: 76 us p99 tuned vs 67 us untuned — no improvement. |
| **throughput / saturation** | **do NOT apply** | at saturation the CPU never idles, so C-states are never entered — but the tuning keeps the machine ~20 C hotter at idle, which caps turbo. **Measured: 81.6k mean achieved with defaults versus ~75k with the tuning applied, on the same host in the same session.** |

**The tuning heats the machine.** `idle-set -D 5` prevents cores sleeping and the
`performance` governor pins them at max clock, so this box idles at ~80 C instead of
~60 C. On a thermally-limited laptop that costs turbo headroom: max clock measured
4002 MHz tuned versus 4289-4396 MHz untuned. Restore defaults between runs:

```sh
sudo cpupower idle-set -E && sudo cpupower frequency-set -g powersave
```

**Do NOT use `cpupower idle-set -D 0`.** Disabling C1 as well leaves cores spinning in
POLL; on a laptop that drives the package to TjMax and throttles the clock, making
latency *worse* (measured: 13 µs floor → 17 µs). See
[`LATENCY-TUNING.md`](LATENCY-TUNING.md).

**C-states do not affect saturation throughput** directly — at saturation the CPU never
idles — but disabling them *indirectly lowers* it by keeping the machine hot enough to
cap turbo. That is the opposite of the intuition, and it is measured.

### Verify before/after every run — any of these invalidates a result

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
IDE before measuring.** Numbers taken with it running are usable for A/B comparisons —
both arms see the same load — but they understate absolute throughput.

A histogram overflow means the reported "percentile" is really the maximum — it *looks*
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

---

## 2. Non-streaming benchmarks

### Throughput / saturation — `bench/saturate.sh`

```sh
BACKENDS=4 ./bench/saturate.sh 5 2 90000 130000
#          ^^^^^^^^^^ THE ONE THAT MATTERS
```

| knob | default | published runs use | why it matters |
|---|---|---|---|
| `BACKENDS` | **1** | **4** | mock backend instances (`SO_REUSEPORT`). At 1 the **mock** is the ceiling (~65k), not the gateway. **The published ~84k figure requires `BACKENDS=4`.** |
| `WORKERS` | 1 | 1 | gateway `SO_REUSEPORT` worker threads. The headline is a *single-thread* ceiling; >1 measures something else. |
| `IO` | `uring` | `uring` | event-loop backend |
| `BODY` | 64 | 64 | request body bytes; 1024 and 8192 were also measured historically |
| arg 1 / arg 2 | 6 / 2 | 5 / 2 | seconds per level / warmup. Longer levels soak the CPU thermally. |
| `BIN` | build dir | — | binary directory; used to A/B two builds on one host |

**Gateway process:** `--workers 1 --io uring --translate anthropic`.

### Latency head-to-head vs LiteLLM — `bench/run_headtohead.sh`

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

---

## 3. Streaming benchmarks

### Head-to-head vs LiteLLM — `bench/run_stream_headtohead.sh`

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
| `PROVIDER` | `fast` | `fast` | `fast` = `faststream` (C++, >100k tok/s). `python` = `mock_provider.py`, which saturates at 45-50k tok/s — **below llmbridge**, so it measures the harness |

### Steady-state latency (the p99 table) — no churn

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

### CPU-per-token and saturation — `bench/run_stream_cpu.sh`

```sh
./bench/run_stream_cpu.sh 20 5     # duration, warmup
```

Records temperature, throttle delta and clock per level. **CPU-per-token is only
meaningful below saturation**: once the worker is pegged at 100% of a core, µs-of-CPU per
token is just `budget ÷ tokens delivered`, i.e. the reciprocal of throughput, not an
efficiency measure.

---

## 3b. Profiling the worker

`perf` is blocked by default on Ubuntu (`perf_event_paranoid=4`). To profile:

```sh
sudo sysctl -w kernel.perf_event_paranoid=1 kernel.kptr_restrict=0
```

`1` (not `2`) is required: at `2` only user space is sampled, and since ~89% of the
worker's CPU is kernel-side the profile would be an opaque `[unknown]` bucket.
`kptr_restrict=0` turns kernel addresses into symbol names.

The worker only starts *after* the direct-baseline phase of `saturate.sh`, so poll for
the pid rather than sleeping a fixed interval.

## 3c. Host software that changes results

| setting | effect | how to check |
|---|---|---|
| **AppArmor** | **~6.3% of worker CPU** on per-send/recv LSM checks | see below — `systemctl stop` does **not** remove it |
| CPU clock | throughput tracks it ~linearly (78k @ 4.0 GHz vs 88k @ 4.5 GHz) | `grep 'cpu MHz' /proc/cpuinfo` |
| `kernel.perf_event_paranoid` | 4 = no profiling at all | see above |

### Disabling AppArmor actually requires a reboot

`sudo systemctl stop apparmor` is **not enough** — verified by profiling: with the
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

Verify after reboot — the symbols must be absent from a profile of the worker:

```sh
grep -o 'apparmor=0' /proc/cmdline
perf report -i perf.data --stdio --no-children -g none | grep -ci apparmor   # expect 0
```

Revert by removing the token and rebooting. This is a **security tradeoff**, so it is a
benchmarking/measurement step, not a production recommendation — quantify the cost, then
decide separately whether the exposure is acceptable where it would actually run.

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
| `kStreamBufCap` | 8 MiB | per-stream output cap (io_uring back-pressure) |
| `kBufCount` / `kBufSize` | 4096 × 4 KiB | io_uring provided-buffer pool. Measured irrelevant to the streaming tail — an 8× increase changed nothing. |

---

## 5. Provenance of published numbers

| claim | configuration required |
|---|---|
| ~84k RPS single-thread ceiling | `BACKENDS=4 WORKERS=1 IO=uring BODY=64`, 5s/2s levels, cool host |
| streaming p99 at 4,096 streams | churn-free config above, 4 client processes, C1-capped host |
| llmbridge vs LiteLLM streaming | `60 20 20 6 "16 64 256 512"`, `PROVIDER=fast`, SYN backlog raised, `ListenOverflows` delta 0 |
| per-token cost ~5 µs | `nullrelay` control at 64 streams, C1-capped host |
