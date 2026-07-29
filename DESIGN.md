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

### io_uring lifetime handling

The sharp edge of io_uring is object lifetime: a `Connection` can have kernel ops in
flight after you'd like to free it. The backend tracks an in-flight op count per
connection and defers the free until the count reaches zero (a "doomed" list),
handles multishot CQEs that carry `F_MORE` alongside a terminal result, and re-arms
on transient conditions. This is the most safety-critical code in the repo and is
commented inline in `gateway/src/gateway.cpp`.

## Request lifecycle (proxy)

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
  worker/thread each, plain HTTP (no TLS/WAN). The ~90k RPS single-thread ceiling is
  the **loopback's packet-processing limit on this box, not the CPU** (the proxy uses
  ~1 core at saturation) — a separate-host run is the next credibility upgrade.

Reproduce: `./bench/run_headtohead.sh` and `./bench/saturate.sh`.

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
  language bindings. (Streaming SSE and structured HTTP error responses have landed:
  the transport/dialect step is shared by both backends, upstream provider errors are
  relayed with their own status code, and an upstream idle timeout bounds stalls.)
- **Phase C:** TLS, provider auth + per-provider endpoint routing, separate-host benchmark.
