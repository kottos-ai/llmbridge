# DESIGN

How `llmbridge` is built, and — just as important — what it does **not** do yet.
This document is written to be checkable against the source: every claim points at
a module you can read. Where the implementation is narrower than the ambition, it
says so.

## Scope in one paragraph

`llmbridge` is (1) a **dialect translator** — pure `string_view → string` functions
that convert an OpenAI-shaped chat-completion request/response to and from a
provider's native shape — and (2) a **reference proxy** that runs the translator on
a fast, single-threaded, non-blocking event loop. It is the fast, "dumb" core:
one upstream, no routing, no auth, no matching. The intelligent layer (multi-provider
routing, a live provider price/latency book, observability, hosting) is the separate
commercial product and is deliberately **not** in this repository.

## Module layering (bottom-up)

```
net/        sockets + HTTP/1.1 framing + raw io_uring wrapper   (no project deps)
  ├─ http.hpp        header-only, allocation-free request/response framer
  ├─ socket_util     listen/accept/connect helpers, SO_REUSEPORT
  └─ uring.{hpp,cpp} thin raw-syscall io_uring ring wrapper (Linux only)
provider/   dialect translation (OpenAI ⇄ Anthropic / Gemini / Cohere)   (no I/O)
  ├─ json.hpp        hand-rolled, scoped JSON parser + escaping builder
  └─ translate.{hpp,cpp}   string→string translation functions
gateway/    the event-loop proxy that ties net + provider together
app/        the CLI daemon (llmbridge --listen … --upstream … --translate …)
```

`provider/` is completely I/O-free and trivially embeddable/bindable — it is the
"use it as a library" story. `net/http.hpp` is header-only so the framer inlines
into the loop. Nothing above `net/` has a third-party dependency.

### Naming conventions (enforced)

Namespaces mirror the directory, with no exceptions:
`net/…` → `llmbridge::net[::http|::tls|::uring]`, `provider/…` →
`llmbridge::provider[::json]`, `gateway/…` → `llmbridge`.

**Inside `Gateway`, the backend a function belongs to is part of its name:**

| prefix | meaning |
|---|---|
| `ep_` | epoll-only — reachable solely from `run_epoll()` |
| `ur_` | io_uring-only — reachable solely from `run_uring()` |
| *(none)* | genuinely shared by both loops (`sweep_idle`, the `tls_*` pump helpers) |

Two rules make this worth the verbosity:

1. **A call crossing the prefixes is a bug, and `grep 'ur_[a-z_]*(' | grep ep_`
   finds it.** Neither backend's teardown, write-arming or completion handling is
   valid in the other. Exactly one such crossing existed when the convention was
   introduced — `ur_forward()` calling the *epoll* error responder — and it had
   been invisible for as long as the epoll half was unprefixed.
2. **Twins share a verb.** `ep_stream_flush` / `ur_stream_flush`,
   `ep_finish_client` / `ur_finish_client`. Before this, the same role was called
   `stream_flush` on one side and `u_stream_kick` on the other, so a grep for the
   counterpart returned nothing and the pairing had to be rediscovered by reading.

The uring prefix is `ur_`, not `u_`, because `u` is the conventional parameter name
for an *upstream* connection — `void u_tls_kick_send(Connection* u)` used both
meanings of `u` in one signature.

**This is enforced, not merely documented.** `scripts/check_conventions.py` runs as the
first CI job (no compiler, ~90 ms) and fails the build on four things: a call crossing
the prefixes; an **unprefixed** method reachable from only one backend; a header whose
namespace does not mirror its directory; and a stale attribution in LATENCY.md's stamp
table (each timing stamp names the function that assigns it, and that function must
exist and must actually assign it). The table names functions rather than line
numbers on purpose: line references rot on any edit above them, so the doc would
need re-checking on every commit — and a reference that needs re-checking every
commit is one nobody re-checks. Function names move only under a rename, which is
exactly when the table should be revisited anyway. The middle check is the one that earns
its keep — a crossing grep cannot catch a mislabelled method, because the offending name
has no prefix to grep for. That is exactly how `abort_pair` survived the rename that
introduced this convention. Each check is verified against a deliberately reintroduced
instance of the defect it exists to catch.

Elsewhere: `kPascalCase` constants, `_member` privates, `PascalCase` types and enum
values, `snake_case` functions, `ts_*` for timestamps, and `X_to_Y_request` /
`X_to_Y_response` for the translation entry points.

## Threading & I/O model

- **Shared-nothing workers.** Each worker is a single thread owning one event loop
  and its own connections. Workers share a listening socket via `SO_REUSEPORT`
  (`app/main.cpp`); the kernel load-balances accepts. Scaling out = more workers.
- **No locks, no atomics on the request path.** Because state is per-worker and
  per-connection, the hot path has zero synchronization. The only atomics in the
  process are the shutdown flag and a test-only live-object counter.
- **io_uring first, epoll fallback.** On Linux ≥ 5.x (built where `linux/io_uring.h`
  is present) the loop uses a raw-syscall io_uring backend: multishot `accept`/`recv`,
  provided buffer rings, and `SINGLE_ISSUER | DEFER_TASKRUN`. Where io_uring is
  unavailable it falls back to a straightforward `epoll` loop. **Both backends are
  maintained and both are tested** (the gateway test suite is parameterized over
  them).
- **Optimistic writes.** Responses are written straight away and only fall back to
  arming `EPOLLOUT` / a queued send on `EAGAIN`, keeping the common case one syscall.

### Kernel bypass (OpenOnload / DPDK) — considered, deferred

Profiling the saturated worker shows **~89% of its CPU inside the kernel**, the largest
slice being the TCP stack (32.7%). That is precisely what userspace networking exists to
remove — OpenOnload, DPDK with a userspace TCP stack, or AF_XDP — and on packet-heavy
workloads those routinely cut CPU-per-packet several-fold. It is deliberately **not** on
the near-term roadmap, for three reasons:

1. **It is unmeasurable on the current benchmark.** Every run is over `127.0.0.1`, which
   has no NIC. A real-NIC, two-host baseline is a prerequisite for any before/after, and
   the kernel profile may look different once a driver and IRQ path are involved.
2. **It conflicts with zero runtime dependencies.** DPDK or a userspace TCP stack is a
   large dependency plus dedicated poll-mode cores; Onload requires specific NICs. That
   is defensible for a deployment you operate, much less so for a library people drop
   into their own infrastructure.
3. **`--workers N` buys the same throughput for none of the cost.** The machine is ~95%
   idle at saturation, so a second worker nearly doubles throughput today.

Where it *would* pay is a hosted deployment large enough for CPU-per-request to be a
cost-of-goods line — fewer instances per token served. Note also that this is a
**throughput/efficiency** lever, not a latency one: the gateway adds ~80 us p99 against
upstream model latencies measured in hundreds of milliseconds.

### io_uring lifetime handling

The sharp edge of io_uring is object lifetime: a `Connection` can have kernel ops in
flight after you'd like to free it. The backend tracks an in-flight op count per
connection and defers the free until the count reaches zero (a "doomed" list),
handles multishot CQEs that carry `F_MORE` alongside a terminal result, and re-arms
on transient conditions. This is the most safety-critical code in the repo and is
commented inline in `gateway/src/gateway.cpp`.

## Request lifecycle (proxy)

> Latency accounting for this lifecycle — the seven stamps, what each reported
> number spans, and why connection setup is excluded from "added latency" — is
> defined in [LATENCY.md](LATENCY.md). Change the stamps and that document is
> the thing that must change with them.


```
accept ─▶ frame request (http::parse) ─▶ [optional] translate to upstream dialect
       ─▶ forward over a keep-alive upstream connection from the pool
       ─▶ frame response ─▶ [optional] translate back ─▶ write to client
```

Upstream connections are pooled and reused across requests. The client and upstream
sides are symmetric small state machines.

## Memory model — honest version

- **Zero-copy where it counts.** The JSON DOM holds `string_view`s into the input
  buffer; string values are never decoded or copied — a passthrough field is emitted
  verbatim. The HTTP framer (`http::parse`) allocates **nothing** — it is pure index
  math over the caller's buffer.
- **Allocation-light, not allocation-free.** Be precise: the JSON DOM allocates its
  node vectors, each request builds its translated output in a growable `std::string`,
  and the proxy allocates one `Connection` per accepted socket. These are small, warm,
  per-request allocations — bounded, no bursts — but they are allocations. A
  per-connection slab arena to remove the remaining ones is staged for the multi-loop
  phase; today the honest claim is "no GC, `malloc`-bounded tails," not "zero-alloc."

## Parsing & framing — hardened and fuzzed

Both parsers are hand-rolled and scoped to exactly the shapes llmbridge moves — no
general-purpose JSON/HTTP library in the shipped binary. Because translate mode feeds
**client-controlled bytes** into them, they are hardened against hostile input and
continuously fuzzed:

- **JSON parser** (`provider/json.hpp`): recursion is depth-limited (`kMaxDepth`), so
  a `[[[[…` nesting bomb fails cleanly (`ok=false`) instead of overflowing the stack.
- **HTTP framer** (`net/http.hpp`): header size is capped (`kMaxHeaderLen`), body size
  is capped (`kMaxBodyLen`, blocking a `Content-Length: 9999999999` memory-exhaustion
  trickle), and framing is smuggling-safe — **Content-Length only**; a `Transfer-Encoding`
  header and a *conflicting* duplicate `Content-Length` are both **rejected** (RFC 9112
  §6), which matters because upstream connections are pooled across clients.
- **Fuzzing** (`fuzz/`): libFuzzer targets for `json::parse` and `http::parse` under
  ASan + UBSan. Build with `-DLLMBRIDGE_BUILD_FUZZERS=ON` (Clang). Structural
  invariants are asserted on every input (e.g. `total_len == header_len + body_len`,
  `body_len ≤ kMaxBodyLen`).

## Translation model

Translation is a set of pure functions (`provider/translate.hpp`): `openai_to_<x>_request`
and `<x>_to_openai_response` for `x ∈ {anthropic, gemini, cohere}`, plus passthrough for
OpenAI-compatible providers. They take a request/response body as a `string_view` and
return a `std::string` (empty on parse failure — no exceptions). Coverage is the common
chat path: model, system prompt, multi-turn messages, `max_tokens`/`temperature`/`top_p`;
and on the response, content / finish-reason / usage.

## Error handling

- **No exceptions on the hot path.** Errors flow through return codes / empty results;
  the only `throw` is in constructor-time setup (socket bind, ring init).
- **Known limitation:** on a malformed request or an upstream failure, the proxy today
  **closes the connection** rather than emitting a `400`/`502` response — so a client
  currently sees a TCP reset, not a status code. Structured error responses are a
  Phase-B item. (The framer's `Error` status is what triggers the close; it is honored
  in both backends.)

## TLS to the upstream (`-DLLMBRIDGE_TLS=ON`)

Off by default. The default build stays **zero-dependency** end to end; enabling TLS
links OpenSSL (≥3.0) — the one sanctioned runtime dependency, confined to the gateway.
The translator library (`provider/`) never links it, in any configuration. CI proves
both states: the gcc/clang matrix builds TLS-off, the sanitizer job TLS-on.

### Why memory BIOs, not `SSL_set_fd`

`SSL_set_fd` assumes whoever owns the socket also makes the syscalls. On the io_uring
backend we don't: multishot recv hands the loop bytes that have **already been read**
into kernel-provided buffers — there is no read left for OpenSSL to perform. So the
loop keeps the socket and the `SSL` object is a pure byte transform behind a pair of
memory BIOs, identical on both backends. The cost is one memcpy per direction; the
crypto itself (AES-GCM with AES-NI) is noise next to the handshake RTTs.

### Data flow

```
                     PLAINTEXT ONLY                    │            CIPHERTEXT ONLY
                                                       │
 client ──► rbuf ──► translate ──► u->wbuf ────────────┤
 (plain)                            (request,          │
                                     kept intact       ▼
                                     for retry)   write_plaintext()
                                       woff ─────►┌─────────┐   pull_ciphertext()
                                    (plaintext    │   SSL   │──────► u->tls_out ──► socket
                                     fed so far)  │ session │                        send
                                                  │  + BIOs │   feed_ciphertext()
 client ◄── translate/SSE ◄── u->rbuf ◄───────────│         │◄────── recv bytes ◄── socket
  pump         pump            read_plaintext()   └─────────┘
                                                       │
                                                       │  Session survives the keep-alive
                                                       │  pool: a reused conn pays NO
                                                       │  second handshake.
```

### The invariant everything hangs off

**`Connection::rbuf` and `Connection::wbuf` hold plaintext, always.** TLS interposes
strictly at the socket edge. Three things fall out of this for free:

1. **Nothing downstream knows TLS exists** — HTTP framing, the SSE pump, translation,
   and the response parse all read `rbuf` exactly as before.
2. **Stale-pool retry works unchanged** — `wbuf` is never consumed by encryption
   (`woff` counts plaintext *fed to the session*, not bytes destroyed), so a retry
   re-pushes the identical request through a brand-new session.
3. **The two backends share all TLS logic** — only the flush/kick differs: epoll
   writes `tls_out` inline and arms `EPOLLOUT` on a partial; io_uring serializes one
   SEND at a time (`send_inflight`), because a SEND SQE points into `tls_out` and the
   buffer must stay immutable while the kernel reads it. Ciphertext produced meanwhile
   stages inside the SSL write BIO until the send completes.

### Security posture (decided, not defaulted)

- **Verification cannot be disabled.** `SSL_VERIFY_PEER` plus `SSL_set1_host` — chain
  *and* hostname, both derived from one argument so they cannot disagree. A failed
  `init_client` tears the session down (fail closed: a caller ignoring the return
  cannot handshake unverified). SNI and the verified name are set together.
- TLS 1.2 floor, server-initiated renegotiation refused (`SSL_OP_NO_RENEGOTIATION`),
  `ERR_clear_error()` before every operation (a stale thread-local queue entry
  misclassifies a benign `WANT_READ` as fatal), int-length clamping on all spans.
- A **fatal record error mid-stream aborts the client** on both backends — a corrupted
  stream must never be finalized with a clean `[DONE]`.
- **Not done, stated:** no OCSP/CRL revocation checking (usual for non-browser
  clients); a provider EOF without `close_notify` is treated as normal end-of-stream
  (providers rarely send it; strict truncation detection would break real streams).

### Tests

Two layers. `net/tests/tls_test.cpp` proves the transport hermetically (handshake
through byte-at-a-time fragmentation, close_notify vs truncation, garbage-is-fatal,
multi-record payloads, untrusted-cert and wrong-hostname rejection). The end-to-end
layer (`gateway/tests/gateway_tls_test.cpp`, both backends) proves the loop wiring:
round trip, pooled reuse pays one handshake for N requests, SSE translation through
TLS, 16 concurrent interleaved sessions, corrupt-record-mid-stream aborts without
`[DONE]`, provider closing pooled conns, provider dropping the TCP connection
mid-handshake, and wrong-hostname surfacing as a client 502 with **zero** requests
reaching the unverified peer.

## Benchmark methodology

The benchmark is designed to be honest first and impressive second:

- **Equal work.** Both `llmbridge` and the LiteLLM comparison do the *full* OpenAI⇄Anthropic
  translation against the **same 200 ms mock backend** (`bench/fastbackend.cpp`,
  `bench/mock_provider.py`).
- **Coordinated-omission-corrected, open-loop load** (`bench/loadgen.cpp`, wrk2
  methodology): requests are scheduled on a fixed timeline, so a stall is counted
  against every request it delayed — not hidden. Warmup is gated out; the histogram
  reports conservative bucket-upper-bound percentiles and flags overflow.
- **Latency accounting** excludes the upstream wait and inter-packet network time, so
  the headline number is *proxy-added* latency, not end-to-end LLM latency.
- **Caveats, stated up front:** a single co-located dev box (i7-9750H), single
  worker/thread each, plain HTTP (no TLS/WAN). The ~84k RPS single-thread ceiling is
  set by **the worker's own CPU on its single thread**. llmbridge is single-threaded, so
  one core is its hard ceiling and at saturation that thread is 87-92% busy — out of
  headroom. (The machine reports ~95% idle only because one busy core is ~8% of 12 logical
  CPUs; reaching the rest needs `--workers N`.) Splitting *that thread's own* CPU time:
  **89% kernel, 6.7% llmbridge**, the largest kernel slice being the TCP stack at 32.7%. An earlier revision attributed it to the
  loopback packet path; that was wrong. Two consequences: optimising our code can win at
  most ~7%, and the number is **thermally dependent** (87k cold, 82k at 85 °C on this
  laptop). A separate-host run is the next credibility upgrade.

Reproduce: see [`bench/BENCHMARK-CONFIG.md`](bench/BENCHMARK-CONFIG.md) — the host
configuration (`BACKENDS=4`, governor, sysctls) changes the result as much as the code.

## What this repo does NOT do (yet)

Stated plainly so there are no surprises:

- **Streaming (SSE) is supported** for OpenAI ⇄ Anthropic only — Gemini/Cohere streaming
  is Phase B. A streamed client response is close-delimited.
- **No TLS, no provider auth, no per-provider URL routing.** Translate mode targets a
  local/mock/already-proxied upstream — it can't call `api.anthropic.com` directly yet
  (Phase C). This is also why the benchmark runs against a mock.
- **No tool calling, vision, `cache_control`, or Bedrock** yet (Phase B).
- **No language bindings** yet (Python/Go/Rust planned).
- **No routing, matching, pricing, or observability** — by design; that's the separate
  commercial layer, not open source.

## Roadmap

- **Phase B:** tool calling, vision, `cache_control`, streaming for Gemini/Cohere,
  **Anthropic-in mode** (client speaks Anthropic, upstream OpenAI-compatible — the
  harder direction: Anthropic's richer event envelope must be synthesised from OpenAI's
  flatter chunks), language bindings. (Streaming SSE and structured HTTP error responses have landed:
  the transport/dialect step is shared by both backends, upstream provider errors are
  relayed with their own status code, and an upstream idle timeout bounds stalls.)
- **Phase C:** TLS, provider auth + per-provider endpoint routing, separate-host benchmark.
