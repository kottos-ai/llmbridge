# Latency accounting in llmbridge

This document defines every latency number the gateway reports, exactly where
each measurement begins and ends, and why the boundaries sit where they do.
It exists because "gateway overhead" is not one number: a request passes
through compute, syscalls, a connection pool, and an event loop, and honest
accounting has to attribute each microsecond to the component that caused it.

For the benchmark methodology (load generation, host configuration, competitor
setup), see [BENCHMARKS.md](BENCHMARKS.md). This document is about what the
numbers *mean*.

---

## 1. What "added latency" means

Added latency is a subtraction between two worlds:

```
added = (request latency WITH the gateway) − (request latency WITHOUT it)
```

Everything the request would have paid anyway — the provider's queue and
inference time, the network, and the TLS handshake to the provider — appears
in both worlds and cancels. What does not cancel is work that exists only
because the gateway is in the path: parsing the client's request, translating
between API dialects, mapping credentials, re-serialising, and moving the
bytes through one extra hop.

This is why the TCP+TLS handshake to the provider is **never** counted as
gateway overhead: without the gateway, the client's own HTTP stack pays the
same handshake on the same schedule (once per connection, again after idle).
It is reported — separately — because it is real latency the request
experienced and an operator should see it; it is just not *ours*.

The published benchmark figures (41–80 µs added p99 from 100 to 5,000 RPS)
are measured externally against a no-gateway control using the same
keep-alive discipline on both arms, so handshakes land on the same side of
the ledger in both worlds. See BENCHMARKS.md for the harness.

---

## 2. The request timeline (non-streaming)

Seven stamps bound six intervals. All are taken on the gateway's monotonic
clock (`steady_clock`, via `now_ns()`), so no clock adjustment can make an
interval come out negative; only t0 is *additionally* converted to wall time,
for the one header that has to correlate with the outside world.

```
 client                     gateway                                provider
   │                           │                                      │
   │  request bytes            │                                      │
   ├──────────────────────────►│ t0  client request fully framed      │
   │                           │ │                                    │
   │                           │ │  parse · translate · auth-map ·    │
   │                           │ │  re-serialise      (OUR COMPUTE)   │
   │                           │ ▼                                    │
   │                           │ t1  upstream request BUILT           │
   │                           │ │   (bytes exist in our buffer;      │
   │                           │ │    nothing sent yet)               │
   │                           │ │                                    │
   │                           │ │  pool checkout; on a cold conn:    │
   │                           │ │  TCP connect + TLS handshake       │
   │                           │ ▼   (CONNECTION SETUP — ≈0 warm)     │
   │                           │ t2  wire ready: socket can carry     │
   │                           │ │   the request                      │
   │                           │ │                                    │
   │                           │ │  write(): copy the request into    │
   │                           │ │  the kernel's socket buffer        │
   │                           │ ▼   (UPSTREAM WRITE — ~4.4 µs p50)   │
   │                           │ t3  request handed to the kernel     │
   │                           │ ├─────────────────────────────────►  │
   │                           │ │        network + provider queue    │
   │                           │ │        + prefill + generation      │
   │                           │ ◄─────────────────────────────────┤  │
   │                           │ t4  provider's response received     │
   │                           │ │                                    │
   │                           │ │  translate back · re-serialise     │
   │                           │ │                    (OUR COMPUTE)   │
   │                           │ ▼                                    │
   │  response bytes           │ t5  response built, write begins     │
   │◄──────────────────────────┤                                      │
   │                           │ t6  response fully flushed           │
```

### The stamps in code

The `ts_*` identifiers are the stable names; t0–t6 is shorthand used in comments
and in this document. Every stamp is a `Connection` member except the last two,
which are locals — nothing outlives the request that needs them.

Stamps are named by the **function** that assigns them, in
`gateway/src/gateway.cpp` — epoll first, io_uring second. The gateway is
implemented twice, so every stamp has a site in each loop, and a change to one
that misses the other is the characteristic bug in this file. Function names
rather than line numbers: line numbers rot on every edit above them, and a
reference that has to be re-checked on every commit is one nobody re-checks.
`scripts/check_conventions.py` verifies in CI that each named function still
assigns the stamp attributed to it.

| | identifier | storage | stamped when | assigned in (epoll / io_uring) |
|---|---|---|---|---|
| **t0** | `ts_req_recvd` | `Connection` | the client's request is fully framed | `ep_on_client_readable` / `ur_try_forward_buffered` |
| **t1** | `ts_req_built` | `Connection` | the upstream request bytes exist — translation, auth mapping and re-serialisation done, nothing sent | `ep_forward` / `ur_forward` |
| **t2** | `ts_wire_ready` | `Connection` | the socket can carry the request. Two sites per backend: the **pooled** path sets it equal to t1 (no handshake happened), the **cold** path stamps it when the connect completes | `ep_forward`, `ep_on_upstream_writable` / `ur_forward`, `ur_on_connect` |
| **t3** | `ts_up_sent` | `Connection` | `write()` has handed the request to the kernel. Several sites — plaintext, TLS-flushed, partial-write completion — all meaning "fully sent" | `ep_forward`, `ep_on_upstream_writable`, `ep_tls_drain_read` / `ur_on_send` |
| **t4** | `ts_up_recvd` | `Connection` | the provider's response arrived. **Non-streaming:** the body is framed. **Streaming:** the response *head* is framed, before any data chunk (see §3 — this is not the first token) | `ep_on_upstream_readable` / `ur_on_recv` |
| **t5** | `ts_resp_built` | local | the response is built and the client write is about to begin. A local because it is consumed immediately by the header arithmetic | `ep_on_upstream_readable` / `ur_on_response` |
| **t6** | `ts_resp_sent` | local | the response is fully flushed to the client. Taken where the histograms are recorded | `ep_finish_client` / `ur_finish_client` |

Two consequences of that table worth reading off it:

- **t2 has two assignment sites per backend, and that is the whole point.** The
  pooled site writes t2 = t1, which is why `connect-us` and the `connect(TLS)`
  histogram both read exactly 0 on a warm connection. A single site could not
  express "no handshake occurred" without inventing a duration.
- **t3 has four sites on epoll.** Plaintext, TLS-flushed, and two write-completion
  paths all converge on the same meaning — the request is fully in the kernel.
  If you add a fifth send path, it must stamp t3 or the request silently reports
  a zero upstream write.

`gateway.hpp` and `gateway.cpp` use this same t0–t6 labelling in their comments.
They previously used an older compressed t0–t4 scheme that did not break out the
write or the flush, which made `connect-us` readable two different ways depending
on which file you trusted.

Two intervals are the gateway's compute; one is connection setup; one is the
upstream write; one is the provider; and the response write/flush sits at the
edge. Every reported number below is a sum of some subset of these — the
differences between the reporting surfaces are exactly *which* subset, and
the t2/t3 split is the one that matters most: t1→t2 is **not our cost**
(the client would pay the same handshake without us) while t2→t3 **is**
(the write only exists because we are the one sending).

---

## 3. Per-request headers (`--timing-headers`)

With `--timing-headers`, every non-streaming response carries the gateway's
own decomposition of that specific request:

| header | definition | contents |
|---|---|---|
| `x-llmbridge-t0` | wall-clock ns at t0 | anchor for ordering against external logs |
| `x-llmbridge-seq` | atomic counter | total order across all workers — two requests can share a nanosecond; a sequencer cannot lie about order |
| `x-llmbridge-gateway-us` | (t1−t0) + (t5−t4) | **our compute, and nothing else**: framing, translation, auth mapping, re-serialisation |
| `x-llmbridge-connect-us` | (t2−t1) | the handshake **alone** — TCP + TLS on a cold connection (~50–80 ms), and **exactly 0** on a pooled one. Identical span to the `connect(TLS)` histogram. |
| `x-llmbridge-upwrite-us` | (t3−t2) | the `write()` copying the request into the kernel's socket buffer (~4.4 µs p50). Ours, because it exists only because we are the one sending. |
| `x-llmbridge-upstream-us` | (t4−t3) | the provider: network, queue, prefill, generation |
| `x-llmbridge-tokens-in/out` | from the response body | usage, non-streaming only |

Streaming responses cannot report t4 in headers (headers precede the body),
so they emit `x-llmbridge-upstream-ttfb-us` instead, and token counts travel
in the stream's final usage chunk (`stream_options.include_usage`), never
invented by the gateway.

**`upstream-ttfb-us` is not time-to-first-token.** It spans t3 → the moment
the provider's response *head* is fully framed — stamped before any data
chunk is pumped, on both backends — verified in the source at both stamp
sites.

**How far the head precedes the first token is a property of the provider, and
for Anthropic it is ~1 ms.** Measured 2026-08-06 with
`private/demo/sse_client.py`, which stamps the response head separately from
the first event carrying generated text, 5 samples per arm:

| arm | t_head (median) | first text | gap |
|---|---|---|---|
| `api.anthropic.com` direct | 1032.8 ms | 1033.8 ms | **0.9 ms** (0.8–1.1 over 5) |
| through llmbridge | 968.9 ms | 971.4 ms | **0.2 ms** |

So Anthropic **withholds its response head until it has a token to send**: the
queue-and-prefill wait lands *before* the head, not after it. For this provider
`upstream-ttfb-us` is therefore a good proxy for time-to-first-token — within a
millisecond, against TTFTs measured in hundreds.

Two caveats before leaning on that:

- **It is one provider's behaviour, not a protocol guarantee.** A server that
  emits `200 OK` on stream *acceptance* would put the whole prefill wait after
  the head and make this header read far below TTFT. Re-measure per provider
  before trusting it; the script takes `--direct` for exactly this.
- **It is still not the same number.** In the run above `upstream-ttfb-us`
  read 886.6 ms against a client-observed TTFT of 971.4 ms. The ~85 ms
  difference is not the provider — it is our own TLS handshake to Anthropic on
  a cold connection plus the client→gateway leg, i.e. spans this header
  deliberately excludes (§1). Quote the streaming benchmark in BENCHMARKS.md
  for TTFT; quote this header for decomposing where that TTFT went.

For the same reason a stream's `gateway-us` is request-side only —
(t1−t0), with no (t5−t4) term. Translate-back on a stream is spread across
every chunk for the life of the response, so no single header can carry it.
Do not compare a stream's `gateway-us` against a non-streaming one; they
span different work.

### `x-llmbridge-t0` drifts; `seq` does not

Every duration above is a subtraction of two monotonic stamps. `t0` is the one
exception: it must be expressed in wall time to be joinable against anything
outside this process, so `wall_ns()` captures `(mono0, wall0)` **once** on first
use and reports `wall0 + (mono_ns - mono0)`. One `system_clock` read per
process, not per request.

That conversion assumes the two clocks tick at the same rate after the anchor,
and under NTP slew they do not. The error is unbounded in principle and grows
with process uptime — a long-lived gateway's reported `t0` will diverge from
true wall time, silently, with nothing in the header to indicate by how much.

The boundary this draws is deliberate and load-bearing:

- **`t0` is for human-scale correlation** — lining a request up against an
  application log or a provider's dashboard, at millisecond granularity. Fine.
- **`t0` is NOT an ordering primitive**, and must never become one. That is
  what `seq` is for: a process-wide atomic counter, immune to slew, to clock
  steps, and to two requests landing in the same nanosecond. Anything that
  needs to know which of two requests came first reads `seq` — and this is
  doubly true of the shadow order book, where the whole point is that an
  exchange defines order by arrival at a sequencing point rather than by
  comparing timestamps across machines.

If a future version needs wall-clock accuracy rather than wall-clock
*plausibility*, the fix is re-anchoring periodically (or reading
`CLOCK_REALTIME` per request and accepting the cost), not tightening the
tolerance on this one.

Two further properties are deliberate:

- **`gateway-us` ends at t5, before the client write.** It must: the number
  travels *inside* the response, so it cannot include the cost of sending
  itself. It is a **compute** figure.
- **Metadata only, by construction.** Durations, one timestamp, token
  counts. No prompt text, no completion text — "prompt content is never
  logged" is a public commitment and this path must never break it.

Headers are per-request **samples**. For distributions, see the histograms.

---

## 4. The shutdown profile (histograms)

On exit the gateway prints aggregate histograms (after a configurable warm-up
period, `_warmup_ns`, is excluded):

```
added-total    count=…  p50=…  p99=…  p99.9=…  max=…
  request-path   …
  connect(TLS)   …
  response-path  …
```

| line | span | contents |
|---|---|---|
| `request-path` | (t1−t0) + (t3−t2) | our request-side compute **plus** the upstream `write()` (~4.4 µs p50 measured); the handshake between them is excluded |
| `connect(TLS)` | (t2−t1) | handshake only — exactly 0 on a pooled connection; this line existing separately is the point |
| `response-path` | t4 → t6 | translate-back **plus** the client write, through full flush |
| `added-total` | request-path + response-path | everything the gateway did to this request; **connect excluded** |

`connect(TLS)` here and `x-llmbridge-connect-us` in §3 are **the same span**,
t2→t1, and are computed by the same function. They cannot disagree.

That was not always true. `connect-us` used to span t1→t3 — handshake **plus**
the upstream write — while this histogram split at t2, so one name carried two
meanings on the same request. The cost of fixing it was one extra header line
per response (`x-llmbridge-upwrite-us`), which is cheap: t2 was already stamped
on every request for the histogram's benefit, so the split needed **no
additional clock reads**, only one more integer on the wire.

Both surfaces now derive from `timing_split()` in `gateway/src/metrics.cpp` —
the single place stamps become intervals. Change that function and both move
together; there is no longer a way to change one alone.

**If you have the old semantics**, recover the retired number by adding the two
headers that replaced it: `connect-us + upwrite-us == the old connect-us`.
There is a test asserting exactly that.

If a histogram prints `[overflow!]`, at least one sample exceeded the
histogram's range and the reported max is clamped — treat that max as "above
range", not as a measurement.

### What the histograms do NOT cover

They are **not** a distribution over all served traffic, and the `count=` field
is not the request total. Two classes are excluded, on both backends:

- **Error replies.** A request finishing with `close_after_resp` is counted in
  `_stats.errors` only. Its timing stamps were never taken, so recording it
  would inject garbage. The consequence to keep in mind: during a provider
  outage the failing requests **leave the latency distribution entirely**, so
  the histogram can look its best exactly when the gateway is serving worst.
  Read `errors` alongside it, never the percentiles alone.
- **Streaming requests.** `{ep,ur}_finalize_stream` increments
  `_stats.requests` but records no histogram sample ("latency histograms N/A"
  in both). A stream has no meaningful single `resp_path`: t4 is the response
  *head*, so t4→t6 would span the entire generation — seconds of provider
  time booked as gateway overhead.

So on a purely streaming workload these histograms are **empty** while
`requests` is large, and on mixed traffic `count` < `requests`. Verified
2026-08-06 — three streamed requests against a local SSE mock:

```
requests=3  errors=0  upstream_conns_opened=3  reused=0  retries=0
added-total    count=0  (no samples)
  request-path   count=0  (no samples)
```

That is correct behaviour, not a bug; streaming latency has to be measured
externally, which is what the streaming benchmark in BENCHMARKS.md does.

**An empty histogram prints `(no samples)`, never zeros.** It used to print
`p50=0 ns  p99=0 ns`, which reads as a spectacular result rather than as
absent data — and `bench/run_bench.sh` seds that exact line for `p99=`, so a
zero-sample run would have published a fabricated **0 µs** added latency.
`Histogram::print` now short-circuits on `_total == 0`, and the bench regex
finds nothing rather than a zero.

### Why the histogram reads higher than `gateway-us`

They bound different spans, on purpose:

```
                 t0 ──── t1 ──── t2 ─── t3 ───────── t4 ──── t5 ──── t6
                 │compute│ conn  │write │  provider  │compute│ flush │

gateway-us       ███████                              ███████            (compute only)
added-total      ███████         ██████               ███████ ████████   (everything ours)
connect-us               ███████                                         (handshake only)
connect(TLS)             ███████                                         (SAME span)
upwrite-us                       ██████                                  (the write)
```

`added-total` additionally contains, relative to `gateway-us`:

1. **The upstream write** — the syscall copying the request into the kernel's
   socket buffer. A memory copy, not network transit; the kernel ships
   packets on its own time.
2. **The client write and flush** — the mirror image on the response side.
   Not always one syscall: a full socket buffer means partial writes and a
   later completion; on io_uring a write is a submitted operation whose
   completion is reaped on a later loop pass. The stamp lands only when the
   last byte is accepted by the kernel.
3. **Event-loop turnaround** — a single-threaded reactor does not run one
   request continuously. Between "response built" and "send completion
   reaped" there is at least one trip through the loop: submit, kernel
   wakeup, and whatever else is in that event batch.

In trading terms: `gateway-us` is the decision logic; `added-total` is
tick-to-trade. Both are real. Quote `added-total` (or the externally-measured
benchmark equivalent) as "added latency"; quote `gateway-us` when
decomposing where the time goes inside it.

---

## 5. Warm vs cold connections

The gateway keeps a pool of keep-alive connections to the upstream. Idle
pooled connections are reaped after **30 s** (`kIdleUpstreamNs`): providers
drop idle keep-alives on their own schedule, and a pooled corpse costs a
failed write and a retry to discover.

Consequences you will observe:

- First request, or first after >30 s idle: `connect-us` shows the full
  TCP+TLS handshake (~50–80 ms to a real provider). Every request until the
  next idle gap: warm pool, `connect-us` **exactly 0**. The tens of
  microseconds you still see on a warm request are `upwrite-us` — the write
  syscall, which happens on every request whether the connection was pooled
  or not. Before those two were split, `connect-us` carried the write and so
  never read 0, which made a pooled connection indistinguishable from a very
  fast handshake.
- A non-zero `connect-us` on a warm deployment therefore means the pool
  **missed** — the connection was reaped, dropped by the provider, or never
  established. That makes it a usable operational signal, which it was not
  when a syscall was baked into it.
- This is the same behaviour your own HTTP client has without a gateway —
  which is precisely why connect time is reported separately and excluded
  from the added-latency claim (§1).

---

## 6. Where the floor is

The single-thread ceiling is ~84k requests/s (median of 3). Profiling the
worker under sustained load — `perf record` at 75k RPS, 20,016 samples, i.e.
near but not at that ceiling — attributes ~89% of its CPU to the kernel,
32.7% in the TCP stack alone, and ~6.7% to llmbridge's own code. The residual added latency of a
warm request is dominated by syscalls, socket-buffer copies, and event-loop
wakeups, not by translation compute.

The practical reading: below ~100 µs of added latency, further reduction is
a kernel story (busy-polling, zero-copy I/O, kernel-bypass networking), not
an application story. The gateway's design — zero allocations and no locks
on the hot path — exists to keep the application's share negligible so the
floor *is* the kernel.

---

## 7. Rules this accounting follows

- **Never fold the handshake into overhead.** Measured live, one cold
  connection put ~56 ms of TCP+TLS setup inside what a single-number report
  would have called "gateway overhead", against 47–63 µs pooled. One number
  cannot honestly carry both.
- **Omit rather than lie.** A stamp that was never taken produces no header.
  Usage that cannot be parsed produces no token counts. Streams do not get
  invented totals.
- **Order by sequencer, not by clock.** Cross-request ordering uses the
  atomic `seq`, because timestamps from different cores or hosts are not
  comparable at these scales.
- **Min is structure, spread is queueing.** For any latency distribution
  here, the minimum approximates structural cost; everything above it is
  queueing or scheduling. Optimise the floor with engineering; interpret the
  spread as load.
- **The instrument is part of the experiment.** Percentiles from overflowed
  histograms, comparisons against a collapsed control, and asymmetric
  keep-alive between arms have each produced a plausible wrong number in
  this project's history. The reporting above is shaped by those failures.
