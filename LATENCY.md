# Latency accounting in llmbridge

This document defines every latency number the gateway reports, exactly where
each measurement begins and ends, and why the boundaries sit where they do.
It exists because "gateway overhead" is not one number: a request passes
through compute, syscalls, a connection pool, and an event loop, and honest
accounting has to attribute each microsecond to the component that caused it.

For the benchmark methodology (load generation, host configuration, competitor
setup), see [BENCHMARKS.md](BENCHMARKS.md). This document is about what the
numbers *mean*.


## 1. What "added latency" means

Added latency is a subtraction between two worlds:

```
added = (request latency WITH the gateway) − (request latency WITHOUT it)
```

Everything the request would have paid anyway (the provider's queue and
inference time, the network, and the TLS handshake to the provider) appears
in both worlds and cancels. What does not cancel is work that exists only
because the gateway is in the path: parsing the client's request, translating
between API dialects, mapping credentials, re-serialising, and moving the
bytes through one extra hop.

This is why the TCP+TLS handshake to the provider is **never** counted as
gateway overhead: without the gateway, the client's own HTTP stack pays the
same handshake on the same schedule (once per connection, again after idle).
It is reported separately because it is real latency the request
experienced and an operator should see it; it is just not *ours*.

**The inbound handshake is a different argument, and it goes the other way.**
When the gateway terminates TLS for the client (`--listen-tls`), that handshake
exists only because the gateway is in the path. Nothing on the other side of the
subtraction cancels it: without us, the client connects straight to the provider
and pays one handshake, and with us it pays two. So the reasoning above does not
transfer, and applying it would be the convenient mistake.

It is not folded into `added-total` either, because the two are not
commensurable. `added-total` is per request; a handshake is per connection, and
a keep-alive client amortises one handshake over thousands of requests. Booking
it per request would either inflate the first request absurdly or, spread
evenly, invent a cost nobody paid. It gets its own line, `accept(TLS)`, measured
from accept to handshake-done, and an operator reading the profile has to add it
themselves with the connection-reuse ratio they actually run. That is the honest
presentation: the number is real, ours, and not a per-request number.

The published benchmark figures (41–80 µs added p99 from 100 to 5,000 RPS)
are measured externally against a no-gateway control using the same
keep-alive discipline on both arms, so handshakes land on the same side of
the ledger in both worlds. See BENCHMARKS.md for the harness.


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
   │                           │ ▼   (CONNECTION SETUP: ≈0 warm)      │
   │                           │ t2  wire ready: socket can carry     │
   │                           │ │   the request                      │
   │                           │ │                                    │
   │                           │ │  write(): copy the request into    │
   │                           │ │  the kernel's socket buffer        │
   │                           │ ▼   (UPSTREAM WRITE: ~4.4 µs p50)    │
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
which are locals; nothing outlives the request that needs them.

Stamps are named by the **function** that assigns them, in
`gateway/src/gateway.cpp`: epoll first, io_uring second. The gateway is
implemented twice, so every stamp has a site in each loop, and a change to one
that misses the other is the characteristic bug in this file.
`scripts/check_conventions.py` verifies in CI that each named function still
assigns the stamp attributed to it.

| | identifier | storage | stamped when | assigned in (epoll / io_uring) |
|---|---|---|---|---|
| **t0** | `ts_req_recvd` | `Connection` | the client's request is fully framed | `ep_on_client_readable` / `ur_try_forward_buffered` |
| **t1** | `ts_req_built` | `Connection` | the upstream request bytes exist: translation, auth mapping and re-serialisation done, nothing sent | `ep_forward` / `ur_forward` |
| **t2** | `ts_wire_ready` | `Connection` | the socket can carry the request. The **pooled** path sets it equal to t1 (no handshake happened); a **cold plaintext** connection stamps it when the connect completes; a **cold TLS** connection stamps it when the handshake completes, in `tls_feed`, because the wire cannot carry the request before then | `ep_forward`, `ep_on_upstream_writable`, `tls_feed` / `ur_forward`, `ur_on_connect` |
| **t3** | `ts_up_sent` | `Connection` | `write()` has handed the request to the kernel. Several sites (plaintext, TLS-flushed, partial-write completion) all meaning "fully sent" | `ep_forward`, `ep_on_upstream_writable`, `ep_tls_drain_read` / `ur_on_send` |
| **t4** | `ts_up_recvd` | `Connection` | the provider's response arrived. **Non-streaming:** the body is framed. **Streaming:** the response *head* is framed, before any data chunk (see §3; this is not the first token) | `ep_on_upstream_readable` / `ur_on_recv` |
| **t5** | `ts_resp_built` | local | the response is built and the client write is about to begin. A local because it is consumed immediately by the header arithmetic | `ep_on_upstream_readable` / `ur_on_response` |
| **t6** | `ts_resp_sent` | local | the response is fully flushed to the client. Taken where the histograms are recorded | `ep_finish_client` / `ur_finish_client` |

**Two stamps sit outside the scheme, deliberately.** `ts_accepted` is taken when
the client connection is accepted, which is before t0 and on a different clock
of relevance: t0–t6 describe one request, and `ts_accepted` belongs to the
Connection that carries it. It feeds two things and neither is a request
interval: the client setup deadline in `sweep_idle`, and the `accept(TLS)`
histogram (§4), stamped where the inbound handshake completes in `tls_feed`. Do
not add it to the table above; a per-connection stamp in a per-request scheme is
how `connect-us` came to mean two things.

`ts_first_token` is the second, and it is streaming-only. It is stamped when the
translator emits the first content token (`content_started()`), which for a
stream lands somewhere *after* t4: t4 is the response head, this is the first
token, and their gap is the provider's prefill. It is not a mainline step,
because a non-streaming request has no first token at all. It is stamped once, in the shared
`stream_step`, so both backends inherit it without a twin divergence. Its only
consumer is the request sink: it cannot ride a response header, because headers
precede the body and the first token has not arrived when they are written.

Two consequences of that table worth reading off it:

- **t2 has two assignment sites per backend, and that is the whole point.** The
  pooled site writes t2 = t1, which is why `connect-us` and the `connect(TLS)`
  histogram both read exactly 0 on a warm connection. A single site could not
  express "no handshake occurred" without inventing a duration.
- **t3 has four sites on epoll.** Plaintext, TLS-flushed, and two write-completion
  paths all converge on the same meaning; the request is fully in the kernel.
  If you add a fifth send path, it must stamp t3 or the request silently reports
  a zero upstream write.

`gateway.hpp` and `gateway.cpp` use this same t0–t6 labelling in their comments.
They previously used an older compressed t0–t4 scheme that did not break out the
write or the flush, which made `connect-us` readable two different ways depending
on which file you trusted.

Two intervals are the gateway's compute; one is connection setup; one is the
upstream write; one is the provider; and the response write/flush sits at the
edge. Every reported number below is a sum of some subset of these. The
differences between the reporting surfaces are exactly *which* subset, and
the t2/t3 split is the one that matters most: t1→t2 is **not our cost**
(the client would pay the same handshake without us) while t2→t3 **is**
(the write only exists because we are the one sending).

**Where optional TLS sits in this timeline.** On a TLS build
(`-DLLMBRIDGE_TLS=ON`, engaged when the upstream is `https://`), the
*handshake* is inside t1→t2, and the *per-record* crypto rides inside the
spans it serves: encrypting the request happens in t2→t3 with the write
(`tls_push_request` before the flush), and decrypting the response happens
on receive, before t4's framing (`tls_feed` as upstream bytes arrive). No
stamp separates cipher time from the write/receive it belongs to
per-record AES on chat-sized payloads is microseconds, dwarfed by the
once-per-connection handshake, so it is deliberately not broken out. A
plaintext build or a plaintext upstream does none of this.


## 3. Per-request headers (`--timing-headers`)

With `--timing-headers`, every non-streaming response carries the gateway's
own decomposition of that specific request:

| header | definition | contents |
|---|---|---|
| `x-llmbridge-t0` | wall-clock ns at t0 | anchor for ordering against external logs |
| `x-llmbridge-seq` | atomic counter | total order across all workers: two requests can share a nanosecond, and a sequencer cannot lie about order |
| `x-llmbridge-gateway-us` | (t1−t0) + (t5−t4) | **our compute, and nothing else**: framing, translation, auth mapping, re-serialisation |
| `x-llmbridge-connect-us` | (t2−t1) | the handshake **alone**. TCP + TLS on a cold connection (~50–80 ms), and **exactly 0** on a pooled one. Identical span to the `connect(TLS)` histogram, though the header is exact and the histogram quantises; see below. |
| `x-llmbridge-upwrite-us` | (t3−t2) | the `write()` copying the request into the kernel's socket buffer (~4.4 µs p50). Ours, because it exists only because we are the one sending. |
| `x-llmbridge-upstream-us` | (t4−t3) | the provider: network, queue, prefill, generation |
| `x-llmbridge-tokens-in/out` | from the response body | usage, non-streaming only |

### TTFT in the benchmark vs TTFB in the headers, two different instruments

They are easy to conflate and they measure different events:

- **`x-llmbridge-upstream-ttfb-us`** (this section) stamps t4 = **the provider's
  response head is complete**. It is a *first-byte* measure, taken on the
  gateway's clock, and a provider that accepts a stream early can look fast on
  it. Anthropic does not, measured 2026-08-06, its head trails its first token
  by ~1 ms, but that is a per-provider fact, not a protocol guarantee.
- **The streaming benchmark's TTFT** (BENCHMARKS.md §B) is stamped **client-side
  by `streamgen`, on the arrival of the first SSE chunk that actually carries a
  token**: response headers are skipped explicitly (`head_done`), and only a
  `data:` line carrying the provider's emission stamp counts. That is a true
  *first-token* measure and it cannot be gamed by sending headers early.

Rule: quote the benchmark number when the claim is about what a user waits for;
quote the header when decomposing one request's path. Never call the header TTFT.

Streaming responses cannot report t4 in headers (headers precede the body),
so they emit `x-llmbridge-upstream-ttfb-us` instead, and token counts travel
in the stream's final usage chunk (`stream_options.include_usage`), never
invented by the gateway.

**`upstream-ttfb-us` is not time-to-first-token.** It spans t3 → the moment
the provider's response *head* is fully framed. The stamp lands before any
data chunk is pumped, on both backends, verified in the source at both stamp
sites.

**The gateway does stamp a real first-token, for the sink not the headers.**
`ts_first_token` (the `RequestRecord` field of the same name) is taken in
`stream_step` the moment the translator emits its first content token, so a
consumer with a sink installed gets both numbers: t4 for the head and
`ts_first_token` for the token, and their difference is the provider's prefill on
that request. This is the same event `streamgen` times client-side, now available
per request to an in-process integrator. It is deliberately absent from the
response headers, because a stream's headers are written before the first token
exists.

**How far the head precedes the first token is a property of the provider, and
for Anthropic it is ~1 ms.** Measured 2026-08-06 with
`sse_client.py`, which stamps the response head separately from
the first event carrying generated text, 5 samples per arm:

| arm | t_head (median) | first text | gap |
|---|---|---|---|
| `api.anthropic.com` direct | 1032.8 ms | 1033.8 ms | **0.9 ms** (0.8–1.1 over 5) |
| through llmbridge | 968.9 ms | 971.4 ms | **0.2 ms** |

So Anthropic **withholds its response head until it has a token to send**: the
queue-and-prefill wait lands *before* the head, not after it. For this provider
`upstream-ttfb-us` is therefore a good proxy for time-to-first-token: within a
millisecond, against TTFTs measured in hundreds.

Two caveats before leaning on that:

- **It is one provider's behaviour, not a protocol guarantee.** A server that
  emits `200 OK` on stream *acceptance* would put the whole prefill wait after
  the head and make this header read far below TTFT. Re-measure per provider
  before trusting it; the script takes `--direct` for exactly this.
- **It is still not the same number.** In the run above `upstream-ttfb-us`
  read 886.6 ms against a client-observed TTFT of 971.4 ms. The ~85 ms
  difference is not the provider; it is our own TLS handshake to Anthropic on
  a cold connection plus the client→gateway leg, i.e. spans this header
  deliberately excludes (§1). Quote the streaming benchmark in BENCHMARKS.md
  for TTFT; quote this header for decomposing where that TTFT went.

For the same reason a stream's `gateway-us` is request-side only
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
with process uptime. A long-lived gateway's reported `t0` will diverge from
true wall time, silently, with nothing in the header to indicate by how much.

The boundary this draws is deliberate and load-bearing:

- **`t0` is for human-scale correlation**: lining a request up against an
  application log or a provider's dashboard, at millisecond granularity. Fine.
- **`t0` is not an ordering primitive**, and must never become one. That is
  what `seq` is for: a process-wide atomic counter, immune to slew, to clock
  steps, and to two requests landing in the same nanosecond. Anything that
  needs to know which of two requests came first reads `seq`, and this is
  doubly true of the tape for inference, where the whole point is that an
  exchange defines order by arrival at a sequencing point, not by
  comparing timestamps across machines.

If a future version needs wall-clock accuracy instead of wall-clock
*plausibility*, the fix is re-anchoring periodically (or reading
`CLOCK_REALTIME` per request and accepting the cost), not tightening the
tolerance on this one.

Two further properties are deliberate:

- **`gateway-us` ends at t5, before the client write.** It must: the number
  travels *inside* the response, so it cannot include the cost of sending
  itself. It is a **compute** figure, - **Metadata only, by construction.** Durations, one timestamp, token
  counts. No prompt text, no completion text. "prompt content is never
  logged" is a public commitment and this path must never break it.

Headers are per-request **samples**. For distributions, see the histograms.


## 4. The shutdown profile (histograms)

On exit the gateway prints aggregate histograms (after a configurable warm-up
period, `_warmup_ns`, is excluded):

```
added-total    count=...  p50=...  p99=...  p99.9=...  max=...
  request-path   ...
  connect(TLS)   ...
  response-path  ...
first-token    ...          <- only when it has samples, i.e. only if traffic streamed
accept(TLS)    ...          <- only when --listen-tls; see below
```

| line | span | contents |
|---|---|---|
| `request-path` | (t1−t0) + (t3−t2) | our request-side compute **plus** the upstream `write()` (~4.4 µs p50 measured); the handshake between them is excluded |
| `connect(TLS)` | (t2−t1) | handshake only: **no handshake at all** on a pooled connection, and this line existing separately is the point. See the note on resolution below |
| `response-path` | t4 → t6 | translate-back **plus** the client write, through full flush |
| `added-total` | request-path + response-path | everything the gateway did to this request; **both handshakes excluded** |
| `first-token` | t0 → first content token | the client's real wait before anything appears, on a **streamed** request only. Mostly the provider's prefill, and almost none of it ours; see the note below on why it is reported and not folded in |
| `accept(TLS)` | client accept → inbound handshake done | the handshake we terminate for the client. **Printed only when it has samples**, i.e. only under `--listen-tls`; a plaintext listener never terminates one. Per connection, not per request, and unlike `connect(TLS)` it is genuinely ours; see §1 |

`connect(TLS)` here and `x-llmbridge-connect-us` in §3 are **the same span**,
t2→t1, and are computed by the same function. They cannot disagree on the
value, but the header is exact while the histogram quantises, and that
distinction was documented wrongly here until 2026-08-12.

### The handshake histograms use a different range, and why

`connect(TLS)` and `accept(TLS)` are built with **1 µs buckets over 262 ms**,
`first-token` with **100 µs over 26.2 s**; the overhead lines keep the default
20 ns over 2.62 ms. Two failures bracket that choice, and both were live.

The default range is sized for a sub-millisecond overhead claim, but a cold
handshake is 50–80 ms, twenty to thirty times the range. Every cold sample
therefore landed in the overflow region, where `percentile()` returns the running
maximum, so p50, p99 and max printed one clamped number wearing three labels.
Local mocks hid it completely: a loopback handshake fits in 2.62 ms.

Widening the *bucket* far enough to fix that introduces the opposite error.
`percentile()` reports a bucket's **upper** edge, deliberately, so the gateway
never under-reports its own overhead. The cost is that a value of zero reads as
one bucket width. This document previously said `connect(TLS)` reads "exactly 0"
on a pooled connection; it read **20 ns**, and nobody questioned it because 20 ns
reads as zero. At 10 µs buckets it would have read 10 µs, inventing handshake
time that was never paid. 1 µs keeps the artifact below the noise while covering
any real handshake.

`first-token` sits at the far end of the same trade and repeated the first
failure before shipping. It was built with `connect`'s range, and three live
streamed turns printed `p50 = p99 = max = 680.69 ms  [overflow!]`: every sample
past 262 ms, the clamped-maximum artifact again. 100 µs buckets cover 26.2 s,
which is a long agent turn, and quantise a 680 ms sample by 0.015%. The memory
is unchanged, since the bucket count is what costs, not the width.

**So: `max()` is exact, percentiles are quantised upward by up to one bucket, and
a pooled connection shows ≤ 1 µs, not a true zero.** The per-request
header `x-llmbridge-connect-us` is exact and does read 0. There is a test pinning
each half of this.

That was not always true. `connect-us` used to span t1→t3 (handshake **plus**
the upstream write) while this histogram split at t2, so one name carried two
meanings on the same request. The cost of fixing it was one extra header line
per response (`x-llmbridge-upwrite-us`), which is cheap: t2 was already stamped
on every request for the histogram's benefit, so the split needed **no
additional clock reads**, only one more integer on the wire.

Both surfaces now derive from `timing_split()` in `gateway/src/metrics.cpp`,
the single place stamps become intervals. Change that function and both move
together; there is no longer a way to change one alone.

**If you have the old semantics**, recover the retired number by adding the two
headers that replaced it: `connect-us + upwrite-us == the old connect-us`.
There is a test asserting exactly that.

If a histogram prints `[overflow!]`, at least one sample exceeded the
histogram's range and the reported max is clamped. Treat that max as "above
range", not as a measurement.

### What the histograms do not cover

They are **not** a distribution over all served traffic, and the `count=` field
is not the request total. Two classes are excluded, on both backends:

- **Error replies.** A request finishing with `close_after_resp` is counted in
  `_stats.errors` only. Its timing stamps were never taken, so recording it
  would inject garbage. The consequence to keep in mind: during a provider
  outage the failing requests **leave the latency distribution entirely**, so
  the histogram can look its best exactly when the gateway is serving worst.
  Read `errors` alongside it, never the percentiles alone.
- **The response half of a streamed request.** A stream has no meaningful
  single `resp_path`: t4 is the response *head*, so t4→t6 would span the entire
  generation, booking seconds of provider time as gateway overhead. With no
  `resp_path` there is no `added-total` either, since one is defined from the
  other. Both stay empty on streamed traffic and that is what
  keeps the headline number one comparable thing.


So `added-total` stays **empty** on a purely streaming workload while
`requests` is large, and on mixed traffic its `count` < `requests`. Measured
2026-08-24, three streamed requests against the live Anthropic API:

```
requests=3  errors=0  upstream_conns_opened=1  reused=2  retries=0
added-total    count=0  (no samples)
  request-path   count=3  p50=53.26 us  p99=60.84 us  max=113.24 us
  connect(TLS)   count=3  p50=1000 ns   p99=1000 ns   max=57.97 ms
  response-path  count=0  (no samples)
first-token    count=3  p50=509.80 ms  p99=517.60 ms  max=671.79 ms
```

Read that profile the way the claim is phrased: 53 µs of ours sits inside a
510 ms wait. `connect(TLS)` reading one bucket width at p50 with a 58 ms max is
the pooling working, one cold handshake and two reuses, and §4's note on
resolution explains why the reused samples read as a bucket instead of as zero.

`first-token` is a **provider** measurement we happen to be positioned to take,
not a gateway one, and it is printed on its own line at the top level for that
reason: folding it into `added-total` would put the provider's prefill inside a
number whose whole purpose is to be small. It is the only line here that is
mostly not our work, and saying so is the same discipline as excluding the
handshakes.

Per-token cadence still has to be measured externally, which is what the
streaming benchmark in BENCHMARKS.md does; this is time-to-first-token only.

**An empty histogram prints `(no samples)`, never zeros.** It used to print
`p50=0 ns  p99=0 ns`, which reads as a spectacular result instead of as
absent data, and `bench/run_bench.sh` seds that exact line for `p99=`, so a
zero-sample run would have published a fabricated **0 µs** added latency.
`Histogram::print` now short-circuits on `_total == 0`, and the bench regex
finds nothing at all, not a zero.

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

1. **The upstream write**: the syscall copying the request into the kernel's
   socket buffer. A memory copy, not network transit; the kernel ships
   packets on its own time.
2. **The client write and flush**: the mirror image on the response side.
   Not always one syscall: a full socket buffer means partial writes and a
   later completion; on io_uring a write is a submitted operation whose
   completion is reaped on a later loop pass. The stamp lands only when the
   last byte is accepted by the kernel.
3. **Event-loop turnaround**: a single-threaded reactor does not run one
   request continuously. Between "response built" and "send completion
   reaped" there is at least one trip through the loop: submit, kernel
   wakeup, and whatever else is in that event batch.

In trading terms: `gateway-us` is the decision logic; `added-total` is
tick-to-trade. Both are real. Quote `added-total` (or the externally-measured
benchmark equivalent) as "added latency"; quote `gateway-us` when
decomposing where the time goes inside it.


## 5. Warm vs cold connections

The gateway keeps a pool of keep-alive connections to the upstream. Idle
pooled connections are reaped after **30 s** (`kIdleUpstreamNs`): providers
drop idle keep-alives on their own schedule, and a pooled corpse costs a
failed write and a retry to discover.

Consequences you will observe:

- First request, or first after >30 s idle: `connect-us` shows the full
  TCP+TLS handshake (~50–80 ms to a real provider). Every request until the
  next idle gap: warm pool, `connect-us` **exactly 0**. The tens of
  microseconds you still see on a warm request are `upwrite-us`: the write
  syscall, which happens on every request whether the connection was pooled
  or not. Before those two were split, `connect-us` carried the write and so
  never read 0, which made a pooled connection indistinguishable from a very
  fast handshake.
- A non-zero `connect-us` on a warm deployment therefore means the pool
  **missed**; the connection was reaped, dropped by the provider, or never
  established. That makes it a usable operational signal, which it was not
  when a syscall was baked into it.
- What the cold ~50–80 ms is made of: roughly **an even split** between the
  TCP connect (~25–30 ms, one round trip) and the TLS handshake (~30–35 ms,
another round trip plus certificate-chain validation). Measured
  2026-08-06 with `curl -w '%{time_connect} %{time_appconnect}'` against
  `api.anthropic.com`, five fresh connections from the reference dev box:
  TCP 23–50 ms (median ~28), TLS increment 30–36 ms (median ~33). One box,
  one afternoon. Indicative, not a benchmark; the split scales with RTT.
- This is the same behaviour your own HTTP client has without a gateway,
  which is precisely why connect time is reported separately and excluded
  from the added-latency claim (§1).


## 6. Where the floor is

The single-thread ceiling is ~84k requests/s (median of 3). Profiling the
worker under sustained load (`perf record` at 75k RPS, 20,016 samples, i.e.
near but not at that ceiling) attributes ~89% of its CPU to the kernel,
32.7% in the TCP stack alone, and ~6.7% to llmbridge's own code. The residual added latency of a
warm request is dominated by syscalls, socket-buffer copies, and event-loop
wakeups, not by translation compute.

The practical reading: below ~100 µs of added latency, further reduction is
a kernel story (busy-polling, zero-copy I/O, kernel-bypass networking), not
an application story. The gateway's design, zero allocations and no locks
on the hot path, exists to keep the application's share negligible so the
floor *is* the kernel.


## 7. Rules this accounting follows

- **Never fold the handshake into overhead.** Measured live, one cold
  connection put ~56 ms of TCP+TLS setup inside what a single-number report
  would have called "gateway overhead", against 47–63 µs pooled. One number
  cannot honestly carry both.
- **Omit instead of guessing.** A stamp that was never taken produces no header.
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
