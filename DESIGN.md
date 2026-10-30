# DESIGN

How `llmbridge` is built, and (just as important) what it does **not** do yet.
This document is written to be checkable against the source: every claim points at
a module you can read. Where the implementation is narrower than the ambition, it
says so.

## Scope in one paragraph

`llmbridge` is (1) a **dialect translator**: pure `string_view → string` functions
that convert an OpenAI-shaped chat-completion request/response to and from a
provider's native shape, and (2) a **reference proxy** that runs the translator on
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
  └─ translate.hpp   string→string translation functions, one .cpp per venue
gateway/    the event-loop proxy that ties net + provider together
  ├─ facade.cpp         Gateway, the public class: forwards to the Engine
  ├─ engine.{hpp,cpp}   detail::Engine: construction, teardown, run(), warm buffers
  ├─ resolve.cpp        next address after a failed connect; names resolved off the loop
  ├─ core/              the one request lifecycle, by concern: client, upstream,
  │                     stream, transport (TLS), sink, sweep, pool; io.hpp is IoOps,
  │                     conn.hpp the two connection kinds, req.hpp a request,
  │                     limits.hpp constants
  ├─ io/                the backends: EpollIo and UringIo, each its loop and syscalls
  └─ request, response, scan, stream   the helpers, by concern (src/ only)
app/        the CLI daemon (llmbridge --listen ... --upstream ... --upstream-dialect ...)
```

`provider/` is completely I/O-free and trivially embeddable/bindable; it is the
"use it as a library" story. `net/http.hpp` is header-only so the framer inlines
into the loop. Nothing above `net/` has a third-party dependency.

### Naming conventions (enforced)

Namespaces mirror the directory, with no exceptions:
`net/...` → `llmbridge::net[::http|::tls|::uring]`, `provider/...` →
`llmbridge::provider[::json]`, `gateway/...` → `llmbridge`, with the gateway's
internals in `llmbridge::detail`.

**The backend boundary is a directory, not a prefix.** The request lifecycle is
written once, in `gateway/src/core/`, as methods of `detail::Engine`; each backend is
a `final` class in `gateway/src/io/` (`EpollIo`, `UringIo`) that owns its loop and its
syscalls and reaches the lifecycle through the methods it calls back. The lifecycle
reaches a backend only through `IoOps` (`core/io.hpp`): `send`, `connect`, `set_read`,
`close`, and `IoCaps` for the one behaviour they deliberately do not share. Calling
one backend from the other is a compile error, so the rules that matter are about
what crosses the boundary:

| rule | why |
|---|---|
| `core/` includes no backend header: `<sys/epoll.h>`, `<linux/io_uring.h>`, `net/uring.hpp`, `io/*` | a backend header in core is how backend code leaks back into the shared path |
| `core/` makes no socket call: `::read`, `::write`, `::send`, `::accept`, `::close`, `epoll_ctl` | the shared path cannot depend on how a backend completes I/O |
| `io/epoll*` never mentions io_uring, `io/uring*` never mentions epoll, comments included | a backend reasoned about in the other's terms is how a fix lands on the wrong side |

This replaced the `ep_`/`ur_` prefixes, which existed because the lifecycle used to be
written twice inside one class: a call crossing them was a bug, and a checker walked
the call graph to find one. With one lifecycle and two classes the compiler enforces
the separation, and the checker enforces the boundary instead.

**This is enforced, not merely documented.** `scripts/check_conventions.py` runs as the
first CI job (no compiler, under a second) and fails the build on: a boundary rule
above; a header whose namespace does not mirror its directory; a stale attribution in
LATENCY.md's stamp table (each timing stamp names the function that assigns it, as
`Class::method` or a name only one class defines, and that function must exist and
must actually assign it); and an identifier whose casing breaks the rules below (a
constant that is not `kPascalCase`, or worse is `ALL_CAPS`, or a type that is not
`PascalCase`). The table names functions instead of line numbers on purpose: line
references rot on any edit above them, so the doc would need re-checking on every
commit, and a reference that needs re-checking every commit is one nobody re-checks.
Before it checks the tree, the checker runs each boundary rule against a planted
violation and fails if any goes unnoticed, so a rule that stops matching cannot report
a clean tree forever.

Elsewhere: `kPascalCase` constants, `_member` privates, `PascalCase` types and enum
values, `snake_case` functions, `ts_*` for timestamps, and `X_to_Y_request` /
`X_to_Y_response` for the translation entry points. The constant and type forms are
machine-checked; the rest are followed by hand.

**`ALL_CAPS` is for preprocessor macros and nothing else**, and that one is worth a
sentence of its own. A C++ constant obeys scope, so it does not need the shouting
that warns a reader about a macro. Giving it ALL_CAPS instead creates a real
collision: macros have no namespace, so a `constexpr int ERROR` breaks against any
system header that defines `ERROR`, and `min`, `max` and `DEBUG` are the same story.
Reserving the shape for macros makes that impossible, and not merely unlikely. The
checker rejects an ALL_CAPS constant and names the `kPascalCase` form to use.

**Comment density is capped**: comment-only lines (the license header included) per
code line, per source file under `gateway/`, `net/`, `provider/` and `app/`, must stay
under 0.8, with a warning from 0.7. Long rationale belongs in this file or in
`GATEWAY-INTERNALS.md`, where it is read once, not beside code that is read on every
change; a header keeps its contracts and points here for the reasons.

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

### Kernel bypass (OpenOnload / DPDK): considered, deferred

Profiling the saturated worker shows **~89% of its CPU inside the kernel**, the largest
slice being the TCP stack (32.7%). That is precisely what userspace networking exists to
remove (OpenOnload, DPDK with a userspace TCP stack, or AF_XDP) and on packet-heavy
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
cost-of-goods line: fewer instances per token served. Note also that this is a
**throughput/efficiency** lever, not a latency one: the gateway adds ~80 us p99 against
upstream model latencies measured in hundreds of milliseconds.

### io_uring lifetime handling

The sharp edge of io_uring is object lifetime: a connection can have kernel ops in
flight after you'd like to free it. The backend tracks an in-flight op count per
connection and defers the free until the count reaches zero (a "doomed" list),
handles multishot CQEs that carry `F_MORE` alongside a terminal result, and re-arms
on transient conditions. This is the most safety-critical code in the repo and is
commented inline in `gateway/src/io/uring_io.cpp`.

## Request lifecycle (proxy)

> For the mechanism behind the shape, see
> [GATEWAY-INTERNALS.md](GATEWAY-INTERNALS.md): which buffer holds what on each
> connection, who owns each piece of mutable state, when a connection may be
> freed on each backend, where the credential is scrubbed, and how pooling and
> the two TLS legs fit together.

> Latency accounting for this lifecycle: the seven stamps, what each reported
> number spans, and why connection setup is excluded from "added latency", is
> defined in [LATENCY.md](LATENCY.md). Change the stamps and that document is
> the thing that must change with them.


```
accept ─▶ frame request (http::parse) ─▶ [optional] translate to upstream dialect
       ─▶ forward over a keep-alive upstream connection from the pool
       ─▶ frame response ─▶ [optional] translate back ─▶ write to client
```

Upstream connections are pooled and reused across requests. The client and upstream
sides are symmetric small state machines.

A byte-forwarded body of 16 KiB or more that is still arriving when its head is framed
goes to the venue as it arrives (`--stream-body`, on by default), so the venue does not
wait out the client's upload. It holds a venue connection until its last byte, so it is
bounded three ways: at most `--stream-body-max` at once per worker, past which a body is
held as before; an upload deadline earned at `--body-min-rate` past `--body-grace`, with
`--body-timeout` on top if set, past which the client gets a 408 and the venue a reset;
and an idle rule that tells a client that stopped sending from a venue that stopped
reading. GATEWAY-INTERNALS.md 12.

## Memory model, honest version

- **Zero-copy where it counts.** The JSON DOM holds `string_view`s into the input
  buffer; string values are never decoded or copied: a passthrough field is emitted
  verbatim. The HTTP framer (`http::parse`) allocates **nothing**; it is pure index
  math over the caller's buffer.
- **Allocation-light, not allocation-free.** Be precise: the JSON DOM allocates its
  node vectors, each request builds its translated output in a growable `std::string`,
  and the proxy allocates one `ClientConn` per accepted socket. These are small, warm,
  per-request allocations (bounded, no bursts) but they are allocations. They are
  not small once a request is: an agent context of 10 MB written into a fresh string
  faulted in 2.6 ms of pages per request on the live tape, which is why the upstream
  request is now built in buffers the gateway keeps (v0.54.2), and why the
  per-connection slab arena to remove the remaining ones has a measured reason and not
  only a principle. It is staged for the multi-loop phase; today the honest claim is
  "no GC, `malloc`-bounded tails," not "zero-alloc."

## Parsing & framing: hardened and fuzzed

Both parsers are hand-rolled and scoped to exactly the shapes llmbridge moves, so no
general-purpose JSON/HTTP library in the shipped binary. Because translate mode feeds
**client-controlled bytes** into them, they are hardened against hostile input and
continuously fuzzed:

- **JSON parser** (`provider/json.hpp`): recursion is depth-limited (`kMaxDepth`), so
  a `[[[[...` nesting bomb fails cleanly (`ok=false`) instead of overflowing the stack.
  Since v0.69.0 it also refuses, or stops paying for:
  - **Anything after the root value but whitespace.** `{}}]},{...}` used to parse as
    `{}`, so every caller splicing a parsed value had to re-check the tail by hand.
  - **A repeated key, at any depth** (`Keys::Unique`, the default). `find()` is
    first-wins and most upstream parsers are last-wins, so `{"model":"cheap",
    "model":"x"}` passed a policy on one copy and reached the provider with the other.
    Keys are compared unescaped, so `"m\u006fdel"` repeats `model`. Cost: at each
    object's close, one pass sets a bit per key (from its length and end bytes) in a
    64-bit word; only an object whose bits collide, or whose subtree held a backslash,
    runs the exact check (pairwise to 16 keys, sorted past that, unescaped only if a
    key has a backslash). Tracking that state per key during the descent measured
    slower: it stays live across the recursion. `Keys::Any` keeps the old reading for
    a caller that needs it; the config loader uses it only to word its error.
  - **More than `kMaxNodes` (2^20) values.** A 16 MiB body of `0,0,0,...` is 8 M
    values; at 64 B a `Value` (80 B a `Member`) that was ~1 GB peak per parse and a
    two-second loop stall. Real bodies run 27 B per value at their densest (an
    agent-tool-heavy summary; chat history runs 150+), so a full 16 MiB (`kMaxBodyLen`)
    legitimate body is ~620 K values: 2^20 leaves 1.7x headroom and bounds the worst
    parse near 64 MB of nodes. Measured again at v0.90.0 on 10 MB bodies
    (`protocol_micro --large`): a chat transcript is 50 K values, a turn of base64
    images 57. Only a body made of nothing but deep tool schemas comes near, at 13 B
    a value (790 K at 10 MB), and no request carries 10 MB of schemas, so the cap stays.
  - **An allocation per parse.** A dropped document's arena, emptied of blocks, is
    parked in a per-thread slot and taken by the next parse, so a parse no longer
    allocates the arena and its block list. Allocations per translated request went
    16 to 12, per 20-delta stream 59 to 7, per further delta 2 to 0
    (`gateway_alloc_test`). Since v0.90.0 there are four slots, taken last in first
    out, so a translator that parses a nested document inside the body's (a tool
    call's arguments) gets its arena back too.
  - **Keeping hostile scratch.** The per-thread scratch stacks are emptied after each
    parse and freed outright when one exceeds the retain budget, the same budget as
    `BlockPool`, so one wide document no longer pins its peak for the thread's life.
    The budget is 4 MiB by default and `--retain-mb` (`json::set_retain_bytes`) up to
    the 16 MiB body cap: a deployment of large bodies keeps a worker's buffers warm,
    and a 10 MB translated transcript then allocates nothing after warm-up (10 MB of
    nothing but tool schemas, whose document is three times the body, still does).
    The cost is up to that much resident memory per worker, arena and scratch each,
    held whether or not the next body is large: 12 MB measured after 10 MB of schemas.
  - **Large bodies.** Since v0.91.0 a string's plain run is found sixteen bytes a
    step (`string_stop`: SSE2 on x86-64, an eight-byte word scan elsewhere), so a
    long text or base64 string costs a few instructions per sixteen bytes, not a
    branch per byte. A request body parsed for translation keeps a tool's
    `parameters` opaque: the schema is checked exactly as a built value is, repeated
    keys at any depth included, but no node is built for it, since every translator
    forwards it byte for byte. Text, tool schemas and `cache_control` already went
    out as spans of the client's body, never decoded and re-encoded. Measured on 10
    MB bodies, see BENCHMARKS.md "Large request bodies".
- **HTTP framer** (`net/http.hpp`): header size is capped (`kMaxHeaderLen`), body size
  is capped (`kMaxBodyLen`, blocking a `Content-Length: 9999999999` memory-exhaustion
  trickle), and framing is smuggling-safe. **Content-Length only**; a `Transfer-Encoding`
  header and a *conflicting* duplicate `Content-Length` are both **rejected** (RFC 9112
  §6), which matters because upstream connections are pooled across clients.
  - **One header walk.** `walk_headers` splits the head once, checks each line's
    ending as it goes, and hands every line to the caller already classified as a
    `Field`, so `parse_request` and `parse_response_head` share one strict reader.
    It refuses a head that another parser could split differently, because on a
    pooled upstream a framing disagreement hands one client another client's bytes:
    - a bare CR or bare LF anywhere, start line included (`X-A: 1\rContent-Length: 27`
      hid a length from us that a lenient upstream honoured);
    - obs-fold, a line starting with SP or HTAB (RFC 9112 §5.2);
    - whitespace before the colon (`Content-Length : 27`, RFC 9112 §5.1);
    - a line with no colon, or an empty name.
  - **Content-Length is `1*DIGIT`** after trimming OWS. `std::from_chars` alone reads
    `0x1b` as 0 and `27abc` as 27; the first framed a 27-byte body as empty.
  - **The request line is `method SP target SP HTTP/1.x`** (RFC 9112 §3): single
    spaces, a token method, a target with no whitespace or control byte, and 1.0 or
    1.1, checked by the gateway with `request_line_ok` (not by `parse_request`, which
    tools also use to frame responses). Reading only the version after the last space
    forwarded lines an upstream could split another way.
  - **The status line is `HTTP/1.x SP 3DIGIT`**, then SP and a reason phrase or the
    end of the line; the reason phrase is optional because servers omit it. Bytes left
    in front of a reply (`JUNKHTTP/1.1 200 OK`) are refused, not absorbed, and
    `release_upstream` also clears the buffer: this is the second lock. A
    status of `2000` or `200x` no longer reads as 200.
  - **Keep-alive.** `Connection` is a comma-separated token list on both legs: any
    `close` token closes, and `keep-alive` keeps an HTTP/1.0 message open, which
    otherwise closes. An HTTP/1.0 upstream reply that said nothing used to be pooled,
    and every reuse became a stale-connection retry.
  - **Quota and `Retry-After`.** A rate-limit family counts as exhausted only when its
    remaining value is a well-formed 0, since naming the wrong quota sends an
    operator to raise a limit that was not the one refusing. `Retry-After` is read in
    seconds only; the HTTP-date form reads as absent.
  - **`find_header`** is the lenient lookup for code outside the framer. The name
    matches exactly, in any case, with or without its colon, so `x-tenant` never
    answers from an `x-tenant-spoof:` line (the bare-prefix version did, waiting for
    its first authorisation caller). First match wins, as the gateway rebuilds the
    upstream request from an allowlist, so a duplicate never travels. It stops at a
    blank line, so a credential written into the body is never found. It splits on
    CRLF only, so a bare CR survives inside a value: anything re-emitted must pass
    `header_value_safe` first.
  - **One walk of a request head.** `parse_request` takes a visitor, and the gateway's
    (`HeadReader`) reads the credentials (`Authorization`, `x-api-key`, `api-key`,
    `x-goog-api-key`, `anthropic-version`, and `anthropic-beta` with a count of its
    lines; the first of each, a stripped one unseen),
    up to eight `x-kottos-key-*` lines a policy could name, stripped or not ("Naming
    the key for a venue"), `Expect` and every `Connection` value in the framing walk,
    as offsets into the head. The translated request is written from them straight
    into the scratch; the Bearer scheme matches in any case.
  - **What a byte-forward drops.** The hop-by-hop headers (`Connection`, `Keep-Alive`,
    `Proxy-Connection`, `TE`, `Trailer`, `Upgrade`) and any header a `Connection` line
    names belong to the client's hop; forwarded, `Connection: close` kept the venue's
    connection out of the pool and `Upgrade` invited a 101 the gateway cannot relay.
    `Host`, `Content-Length`, `Expect` and `Accept-Encoding` are dropped too, every
    `x-kottos-key-*` header in any case, and with a key named the client's
    `Authorization`, `x-api-key`, `api-key` and `x-goog-api-key` ("Naming the key for
    a venue"); an HTTP/1.0 request line goes as HTTP/1.1. Whether the venue's
    connection is pooled is the venue's response alone. A head with more than four
    `Connection` lines is refused rather than forwarded with a listed header kept.
  - **What a relayed response keeps** (`net/http_write.hpp`). A reply the gateway
    re-frames (a translated provider error, a chunked byte-forward, a stream's head)
    keeps the upstream's `Retry-After`, `retry-after-ms`, rate-limit headers
    (`x-ratelimit-*`, `anthropic-ratelimit-*`, `ratelimit*`) and request ids
    (`x-request-id`, `request-id`, `x-amzn-requestid`), verbatim; a byte-forwarded
    body also keeps its `Content-Type` and `Content-Encoding`. Nothing else crosses.
    One status table names every code; a code it does not list keeps its number and
    takes its class's phrase, so a policy's 422 is a 422, never a 502.
- **Response framing** (`parse_response`). The request path frames by Content-Length
  only, because there the bytes are the client's and a TE/CL disagreement with the
  upstream is the classic desync. On the response path we are the client of a
  configured provider, and chunked is normal: Anthropic sends non-streamed
  completions chunked over HTTP/1.1 (invisible over h2, so a curl probe will not
  show it). So the response framer accepts both, and refuses:
  - both framings at once (RFC 9112 §6.3), whichever is right;
  - a 1xx: framing `100 Continue` as the reply orphans the real response on a pooled
    connection, where it becomes the next client's bytes (a byte-forward also
    drops the client's `Expect`);
  - neither framing header on a response that has a body: read-until-close cannot be
    pooled. A streamed response is diverted on its head before this, so
    close-delimited SSE is unaffected.

  `ResponseHead::body` names how the body ends (RFC 9112 §6.3): `None` for a reply
  to HEAD and for 1xx, 204 and 304 whatever their headers say, then `Chunked`,
  `Length`, or `UntilClose`. A 304 with its resource's Content-Length, or a HEAD
  reply with its GET's, waited for bytes that never come until the idle timeout.
  Byte-forward relays any method, so the gateway passes `sent_head` to the framer;
  a translated request is always a POST. A chunk size line is `1*HEXDIG`, optional
  whitespace, then a chunk extension (accepted, ignored) or CRLF; `0x10` read as the
  last chunk and swallowed the data as trailers.

  A Content-Length body is a view into the receive buffer; a chunked body is decoded
  into the per-connection `ResponseDecoder`, which is fed only the bytes that arrived
  since the last call. Re-decoding from byte zero on every read cost O(reads × body):
  8 MB in 64 KiB reads took 79 ms of single-threaded loop time (1 MB 1.6 ms, 2 MB
  4.3 ms, 4 MB 13.9 ms), head-of-line blocking every other client on that worker. A
  typical 1 KB reply arrives in one read and never noticed.
- **Fuzzing** (`fuzz/`): libFuzzer targets for `json::parse` and `http::parse` under
  ASan + UBSan. Build with `-DLLMBRIDGE_BUILD_FUZZERS=ON` (Clang). Structural
  invariants are asserted on every input (e.g. `total_len == header_len + body_len`,
  `body_len ≤ kMaxBodyLen`). `fuzz_http_diff` runs every input through the framer and
  through `fuzz/http_legacy.hpp`, a frozen copy of the v0.70.0 framer from before the
  header walker. The framer may refuse more than the old one, never less, and what
  both accept frames the same, except the deliberate changes above: keep-alive, and
  bodyless responses ending at the head. It is kept for one release.

## Translation model

Translation is a set of pure functions (`provider/translate.hpp`): `openai_to_<x>_request`
and `<x>_to_openai_response` for `x ∈ {anthropic, gemini, cohere}`, plus passthrough for
OpenAI-compatible providers. They take a request/response body as a `string_view` and
return a `std::string` (empty on parse failure, no exceptions). Coverage is the common
chat path: model, system prompt, multi-turn messages, `max_tokens`/`temperature`/`top_p`;
and on the response, content / finish-reason / usage.

Every request translator reads `messages` through one walk (`provider/src/messages.hpp`)
that calls the dialect's hook for each message: system (and `developer`), user and
assistant turns, assistant `tool_calls`, and `tool` results. A dialect without a hook
for a construct refuses the request instead of dropping it, so Gemini and Cohere refuse
tools, tool calls and tool results, and every dialect refuses an unknown role. Anthropic's
parameters are a table (`temperature` limited to Anthropic's 0 to 1, `top_p`, `stop` to
`stop_sequences`, `stream`); `max_completion_tokens` wins over `max_tokens`; `tool_choice:
"none"` becomes `{"type":"none"}` and keeps the tools, because a history holding
`tool_use` blocks is refused without them; and `parallel_tool_calls: false` becomes
`disable_parallel_tool_use` inside `tool_choice`. There is no default model.

Coverage per target: Anthropic Messages (system extraction, content blocks,
`stop_reason`), Gemini `generateContent` (`contents`/`parts`, role `model`,
`generationConfig`) and Cohere Chat v2 (`messages`, `top_p` to `p`). OpenAI-compatible
providers (Groq, Together, Fireworks, DeepInfra, Mistral, Perplexity, xAI, OpenRouter,
Cerebras, vLLM, ...) need no body translation: the gateway byte-forwards them and only
rewrites auth and the endpoint. Streaming deltas, tool calling and `cache_control` are
built for Anthropic; vision is not.

### Body edits and top-level readers

`rewrite_model`, `upsert_string` and `apply_overrides` **splice, never re-serialise.**
The body goes to a venue that already speaks this dialect, so re-emitting it from a
parse would silently drop any field this parser does not model, and providers add
fields faster than we adopt them. Only the value span moves.

- **Top-level keys only.** A message whose content contains `"model"` is text, and
  rewriting inside it would corrupt a prompt.
- **A replacement carrying a quote, a backslash or a control byte is refused.** Those
  would need escaping, a model id never contains them, and guessing at the escaping of
  a string that lands in a request body is how an injection starts.
- **`upsert_string` can insert.** `rewrite_model` only replaces, because a request with
  no `model` is not one this gateway serves; `service_tier` is usually absent and the
  route deciding the tier has to be able to put it there.
- **`apply_overrides` sets `model`, `service_tier`, `speed` and `inference_geo` in one
  parse and one copy.** Absent keys go in together after the opening brace. A body with
  an escaped top-level key is refused when a key is inserted, since `find` matches keys
  as written and the venue would then see two spellings of one. A body streamed on a head
  decision gets the same insertion spliced in as it passes, every key; a member the client
  sent is blanked rather than replaced, since its place has gone by (GATEWAY-INTERNALS.md
  12e).
- **A policy's `anthropic_beta` token is a header, not a body field.** It goes only to a
  venue taking an `x-api-key`. A byte-forward joins it to the client's own
  `anthropic-beta` value in one line, unless the client already sent it; a repeated or
  unsafe client line is refused. A translated request never carried the client's
  `anthropic-beta`, so it gets the token alone.
- **`model_of` refuses an escaped value instead of unescaping it.** It is compared with
  configured names, an escape means it was never one of them, and unescaping would need
  an allocation on a path that has none.

**Bedrock** differs from Anthropic direct in two required ways: no `model` field,
because Bedrock takes the id in the path (`/model/{id}/invoke`), and `anthropic_version`
inside the JSON, where Anthropic wants a header. A body that names no model has no path
to build, so the translator refuses it.

**Upstream errors** become the OpenAI envelope: Anthropic's
`{"type":"error","error":{"type":"overloaded_error","message":"..."}}` is sent as
`{"error":{"message":"...","type":"...","code":null}}`, so a rate limit, an overload or
a context-length error reaches the client as an actionable error.

### Gemini clients and Vertex

A Gemini client names its model in the path, `/v1beta/models/{model}:generateContent`,
and whether it streams in the method, so the body has neither. `RequestFacts::model` and
`stream` are the path's, known on the head, and every model a policy picks for such a
request (`Decision::model`, a retry's, a substitution) rewrites that segment and never the
body. A model goes into a request line, so the client's and the policy's must be 1 to 255
bytes of `[A-Za-z0-9._-]`: the client's is a 400 otherwise, the policy's a 500.

`vertex` is Gemini's body at Google Cloud's publisher path. A Gemini API path sent there
is rewritten into `/v1/publishers/google/models/`, a Vertex publisher path sent to a
`gemini` venue into `/v1beta/models/`, and a project-scoped Vertex path goes as written,
its project the client's. The credential is a per-venue choice, not the dialect's:
`x-goog-api-key` by default, `Authorization: Bearer` with `auth_bearer`, and never both.

Refused before any venue: a `key` query parameter, which would ride every log that keeps
a URL; `:streamGenerateContent` without `alt=sse`, which streams one JSON array; and the
Interactions API, not built.

**Gemini replies** say in their objects what other venues say in headers: the model
served (`modelVersion`, which differs from the alias asked for), the id (`responseId`),
the tier (`serviceTier`, or Vertex's `trafficType`), and grounding queries. A stream's
`data:` lines are joined across reads and parsed one at a time, but only a line that can
still say something: every line before the first token, after it only one naming a key
not yet read. The first token is non-empty `text` not marked `"thought": true`, or a
`functionCall`; thinking is a part so marked, which appears only when the client asked
for `includeThoughts`. A stream that fails after its head sends `data: {"error":...}` on a
200, recorded as `stream_error`. A Gemini client is never sent a reply note: the Python
SDK fails a stream on a comment line.

### Signing for Bedrock (`net/sigv4.hpp`)

SigV4 is a defined procedure, not a format to approximate: canonical request, string
to sign, a date-scoped HMAC chain, then one header. Every step has an exact
serialisation, and a mismatch anywhere fails closed with a 403 whose body says nothing
useful, so the implementation is built against AWS's published test vectors and never
against a reading of the prose. It needs SHA-256 and HMAC-SHA256 from the OpenSSL a TLS
build already links; a dependency-free build cannot reach a venue that requires signing.

- **Credentials pass through per request.** llmbridge holds no AWS credential of its
  own: the customer's travels with their request and is gone when it completes, which
  keeps the gateway stateless and a long-lived secret out of any store the request
  path can read. Temporary credentials carry a session token, which most real
  deployments use; omitting it works in a demo and fails at a customer.
- **The bearer value is colon-separated** (`AKID:SECRET[:SESSION_TOKEN]`) because AWS's
  alphabets exclude the colon: an access key id is uppercase alphanumeric, and a secret
  and session token are base64. A value that does not split into two or three non-empty
  parts is malformed and refused.
- **The canonical URI is encoded one more time than the wire.** AWS encodes a non-S3
  path segment twice counting from the raw path, and the request line already holds
  the first pass: a model-id colon travels as `%3A` and is signed as `%253A`. Signing
  the wire form unchanged, or encoding it twice more (`%25253A`), returns 403 with an
  empty body for every versioned model id.
- **One encoder.** `uri_encode` is inline and free of OpenSSL because the gateway builds
  the Bedrock path with it even in a build without TLS; two encoders that must agree
  byte for byte is how a signature silently stops matching a path.

### Dialect resolution (client dialect x venue dialect)

Folding two questions into the venue's dialect field, what the venue speaks and what
to translate, does not work: a single `anthropic` value would mean "translate an OpenAI
client to Anthropic", which assumes an OpenAI client. That assumption is invisible and
wrong for any caller that already speaks Anthropic, whose request would be
mistranslated into an OpenAI response it cannot parse.

The model separates the two axes:

- **client dialect**: the wire format the caller speaks, read per request from the
  method and the full target by exact route (`client_wire`), the query stripped: a
  POST to a path ending in `/v1/messages` or `/v1/messages/count_tokens`, or any method
  under `/v1/messages/batches`, is Anthropic; a POST ending in `:generateContent` or
  `:streamGenerateContent` is Gemini; a POST ending in `/v2/chat` is Cohere. Anything
  else is OpenAI, so `/v1/threads/{id}/messages` is, and an unknown target behaves as
  it did before dialects existed.
- **venue dialect**: the body format the venue speaks, a static property.

The translation to run is a function of the pair (`resolve_translation`, in
`gateway/dialect.hpp`), not of the venue alone. It answers with a `TranslationPlan`
carrying two fields, because one cannot say this: `translate` is whether a translator
runs, `venue` is the shape to translate into. A same-dialect pair byte-forwards, and
there is no dialect value meaning "no translation": spelling it `OpenAI` would claim an
Anthropic passthrough was OpenAI.

| client -> venue | result |
|---|---|
| same dialect (Anthropic->Anthropic, OpenAI->OpenAI) | `translate = false`: byte-forward |
| OpenAI -> Anthropic / Gemini / Cohere | `translate = true`, `venue` names the shape: the built OpenAI-in path |
| Anthropic (or other non-OpenAI) -> a different venue dialect | refused: the reverse translator is not built |

The last row fails closed. It is the Anthropic-in direction (a caller speaking Anthropic
against an OpenAI venue), which needs translators that synthesise structure where the
OpenAI-in ones discard it, and those do not exist yet. Refusing beats mistranslating: the
request never reaches the venue.

**Body dialect is one axis of three, and `dialect.hpp` owns only the first.** A venue also
has an auth scheme (Bearer, `x-api-key`, SigV4, `api-key`) and a URL layout (Bedrock puts
the model in the path, Azure the deployment in the path and the api-version in the query).
`Bedrock` and `Azure` in `UpstreamDialect` bundle those transport quirks over a body dialect
(Anthropic and OpenAI respectively), so they cannot reduce to a plain byte-forward on a
same-dialect match. They stay on their existing OpenAI-client path until a non-OpenAI
client path is built for them, and `resolve_translation` refuses a non-OpenAI client to
either, so the SigV4 or the URL rewrite is never dropped. Bedrock is Anthropic's body
with the model in the path and SigV4 in place of a header swap, so it needs a TLS build
for the signing: selecting it without one fails at startup instead of sending unsigned
bytes. Azure is OpenAI's body with the deployment in the path, `api-version` in the
query and the key in `api-key`; it is not a byte-forward, which would merge two queries.

**The venue table** (`provider/dialect.hpp`, `provider/src/registry.cpp`). Every
per-venue fact is one row of `VenueSpec`, indexed by `provider::Venue`, of which
`UpstreamDialect` is an alias: its one name (flags, config and logs read it), its body
wire, its credential scheme, whether a translated stream is served, whether a
same-wire client byte-forwards, its request and response translators, its path built
from the model (`target`), and its finish reasons. The gateway looks rows up instead
of switching on the venue.

- **Streaming** is a capability. Only Anthropic (its SSE translator), Azure (OpenAI's
  own stream) and a byte-forward serve one; an OpenAI client asking a translated
  Gemini, Cohere or Bedrock venue to stream is refused (400), where Gemini and Cohere
  used to answer one JSON body that an SDK reads as an empty stream.
- **The path names the model** where the venue wants it: Gemini
  `/v1beta/models/{model}:generateContent` (it was always `models/gemini`), Bedrock
  `/model/{model}/invoke`. The model is decoded from its JSON form and sent as one
  percent-encoded segment; a policy's override names it there too. No model is a 400.
- **The body is parsed once.** The gateway parses a translated request at dispatch and
  hands the tree to the venue's translator; a model override travels in `RequestOut`
  instead of being spliced in and parsed a second time.
- **Finish reasons** come from each provider's documented list. One that is the venue's
  own failure (Cohere `ERROR` and `TIMEOUT`, Gemini `MALFORMED_FUNCTION_CALL` and the
  tool-call faults) maps to `Finish::Error`, which is no completion: the gateway answers
  502. A refusal or a safety block is `content_filter`, including a Gemini prompt blocked
  before any candidate (`promptFeedback.blockReason`).

Sequencing: phase one is the resolution above, which makes a same-dialect caller
byte-forward (this is what lets an Anthropic client reach an Anthropic venue) and refuses
the unbuilt pairs. Phase two builds the Anthropic-in translators and splits Bedrock/Azure
transport out of the body-dialect axis.

## Error handling

- **No exceptions on the hot path.** Errors flow through return codes / empty results;
  the only `throw` is in constructor-time setup (socket bind, ring init).
- **Known limitation:** on a malformed request or an upstream failure, the proxy today
  **closes the connection** instead of emitting a `400`/`502` response, so a client
  currently sees a TCP reset, not a status code. Structured error responses are a
  Phase-B item. (The framer's `Error` status is what triggers the close; it is honored
  in both backends.)

## The policy seam (`gateway/policy.hpp`)

One hook, one question: **may this request proceed?** llmbridge answers it for nobody.
It authenticates no client and has no tenant concept, which is a design position, not a
missing feature: a gateway that authenticates clients is multi-tenant infrastructure,
and this is a proxy.

```cpp
class Policy {
  public:
    virtual ~Policy() = default;
    virtual Decision decide(const RequestFacts&) noexcept = 0;
    virtual Retry on_failure(const FailureFacts&) noexcept { return {}; }
    virtual bool wants_prefix_hash() const noexcept { return false; }
    virtual bool decides_on_head() const noexcept { return false; }
    virtual Decision confirm(const RequestFacts& whole, const Decision& early) noexcept { return early; }
    virtual void on_reply(const ReplyFacts&) noexcept {}
};
```

Supplied at `Gateway` construction, non-owning, stored `const` so there is no setter to
race. Called at one site per backend, after framing and before translation, credential
mapping or upstream acquisition, so a refusal reaches no provider. A policy that decides on
the head is called there too, on the head: its refusal reaches no provider either, and a
refusal from its confirm reaches one only as a reset body short of its Content-Length.

Two separate defaults, answering different questions. `Gateway`'s policy pointer is
null in a stock build, so no call is made and the request forwards: absent, not
permissive. `Decision::allow` is false, so a zeroed decision returned from a forgotten
branch in someone's policy refuses instead of forwarding.

`RequestFacts` carries the request line, the headers, the framed body size and the
top-level `model` name, and no route to the body: "metadata only, no prompt text" is a
property of the type instead of a promise. It does carry the client credential, since
`Authorization` is in the head, so nothing derived from it may be logged and the views
die with the call. It exposes no lookup helper; `net::http::find_header(facts.head, name)`
is the safe one.

One more field crosses the seam, and only on request: `prefix_hash`, a 64-bit identity
of the body's leading bytes (SHA-256, truncated to its leading eight bytes; the field
promises the identity and not the function), computed when the policy returns true
from `wants_prefix_hash()` and 0 otherwise.

### Deciding on the head of a body (`decides_on_head`, `confirm`)

A policy routes on the head and at most the leading 4 KiB, so holding a large body to its
last byte for it costs the request one whole upload. A policy that returns true from
`decides_on_head()` is asked on the head instead, with `RequestFacts::body_whole` false: a
top-level `model` only once it has closed in the bytes so far, the streaming flags false,
the prefix hash as a held body's (the gateway waits for its bytes). It may refuse there,
and the refusal reaches no provider: the client is answered while still uploading, and
the rest of its body is read and dropped. Allowed, the body streams to the venue as it
arrives, or, when the decision needs all of it (a `model` override, `Decision::hold`, a
venue that translates), parks in the gateway unsent until its last byte.

Either way the last byte waits for `confirm(whole, early)`, called once with the whole
body's facts, exactly a held body's. Returning `early` keeps the decision; refusing answers
the client with the policy's status and message, and a venue reaches the request only as
a reset body short of its Content-Length, which it never runs; any other allowed decision
redoes the request, the venue reset and the whole body sent under the new one. So a
provider may see part of a prompt the policy then refuses or moves, never a request it can
run, and what a policy decides on a whole body is what the provider runs. The opt-in is
explicit because a confirm that changes its mind wastes a partial upload, a cost the
gateway records (`RequestRecord::decided_on_head`, `confirm_outcome`) so a deployment can
measure it before turning it on. GATEWAY-INTERNALS.md 12c.

A head decision cannot rewrite a model it has not seen, and the SDKs send `model` after the
messages. `Decision::substitute_model` with `substitute_for` is the rewrite it can make: a
conditional substitution, "if the body's top-level `model` is the plain string
`substitute_for`, send this instead". On a streamed body the gateway splices it in as the
model's bytes pass, holding back only that value until it closes, and declares the
Content-Length itself: the client's, plus what the replacement adds. A body whose model is
something else goes as sent with spaces after its last byte, so the declared length holds
and the JSON stays valid; a shorter replacement is followed by spaces instead. The policy's
confirm sees the client's model and `model_substituted`, and keeping it is no redo. On a
body sent whole the substitution is an ordinary override. GATEWAY-INTERNALS.md 12d.

A tier listing is known on the head, so `service_tier`, `speed` and `inference_geo` need no
model, only a place in the body. On a streamed JSON body the gateway inserts them right
after the opening brace, in the text the held rewrite writes, and declares the client's
length plus theirs. A member of the client's own by the same name goes as spaces, with
the separator before it, so the venue reads the policy's value once and the JSON stays
valid. The scan holds back what follows the brace until the first key (or the close of an
empty object, which takes a space for the comma), and every member, separator first, until
its key shows whether it is one being replaced. No placement costs a redo, and the
confirm's keep is judged by what the venue gets. GATEWAY-INTERNALS.md 12e.

### Selecting a venue

`Decision::upstream_index` picks one entry from the gateway's upstream table. The table
is fixed at construction and every venue carries its own address, TLS setting and
Dialect, so routing Claude to Anthropic and a Llama to an OpenAI-compatible host means
one request is rebuilt and the other forwarded, decided per request.

`Decision::model` exists because the same product is named differently at each venue:
`gpt-4o` at OpenAI, a deployment name at Azure,
`us.anthropic.claude-haiku-4-5-20251001-v1:0` at Bedrock. Without it the caller would
have to know which venue it lands on, which defeats routing.

Two rules make the single-upstream gateway a special case of this one, not a different
thing. An index that is unset (-1) or past the end of the table means the
First upstream, so a policy that only authenticates never learns the table exists (past
the end with a key named is refused: "Naming the key for a venue"). And
**pools are per venue**: a keep-alive connection to one provider is never handed to a
request bound for another, because that would put the request, and its credential, on a
socket to the wrong company. A retry after a stale pooled connection stays on the same
venue for the same reason.

The cost is fragmentation: N workers times M venues pools, each seeing ~1/(N*M) of the
traffic and crossing the idle line far more often, and every crossing costs the next
request a reconnect and a TLS handshake. That is what `--pool-idle` exists to tune, and
its 30 s default was chosen when there was one pool.

### Several venues at once (`Decision::hedge`)

A decision may also name up to three more venues, each a `HedgeLeg` with its own venue,
overrides and key header, and the request goes to all of them at once. The gateway decides
nothing here: which venues, and whether to hedge at all, is the policy's, as every choice
of venue is. What it guarantees is the mechanism. Each leg is rebuilt for its own venue
with its own key and no other, which is why every leg, the decision included, must name a
`credential_header`. The body is held to its last byte, so every leg gets all of it. The
first leg with an answer serves the client: a stream's first token, never its head, or a
whole 2xx reply. Every leg still waiting is then reset, never pooled. A failure waits for
the others, and only the last leg to fail answers the client. Each leg leaves its own sink
record, grouped by `hedge_group`, because each was a real request a venue may bill.
GATEWAY-INTERNALS.md 13.

### Venue addresses

A venue is dialled at `Upstream::ip`. Given a list (`ips`), a failed connect moves it to
the next address; given a name (`host`), the failure also asks for the name again. The
lookup runs on a resolver thread, never the loop, and the loop installs its answer at the
next sweep. One venue's name is looked up at most once a second, and a name that keeps
failing waits twice as long each time, up to half a minute.

A venue may be given by its name alone, with no address yet: an embedder that resolves
at boot need not refuse to start on one name that did not resolve. `run()` asks for it,
the sweep asks again as the backoff allows, traffic or not, and until an answer arrives
the venue is refused ("no upstream (venue unresolved)") like any venue that cannot be
reached, so a policy fails over from it. The stock binary still refuses a name that does
not resolve at startup: with no policy there is nothing to fail over to.

The bare `IP:PORT` form's Host header names the address dialled, so it follows the address
as it moves. Destroying a `Gateway` never waits on a lookup in flight, which can take the
resolver's timeouts while DNS is down: the lookup finishes on its own state, detached.

### When a venue fails

`Policy::on_failure` fires once for every venue that failed while the client has seen
nothing: a refused or unreachable connect, a failed write, EOF or silence before the
response was whole, or a venue whose name has no address yet. It may name another; the
request is then rebuilt for that venue's dialect, since the bytes queued for the failed one
were translated for its API. A first attempt fails over like any later one.

`FailureFacts::retryable` says whether the answer is used. It is false with a one-venue
table, once a response byte is in (the venue may have acted, so a re-send could run the
request twice), and for the third venue: those failures are reported so that a policy
keeping health counts every one, and the gateway gives up whatever it returns.
`FailureFacts::seq` is the request's sequencer, the one `RequestFacts` and `RequestRecord`
carry, so a policy keys its own per-request state, such as the venues already tried, on it.

A venue that did answer, unparseably, never reaches the hook: retrying would mask an
incompatibility as a blip. Neither does a failure of this host (out of descriptors or
memory, or a full io_uring queue or epoll set), whether the connect says so at once or in
its completion: another venue would need the same, and this one is not to blame, so the
client gets a 503 and nothing fails over. No free local port is not one of them: Linux
picks a port per destination, so another venue may still have one. Bounds, none of them
policy: three venues per request, the failed venue may not be renamed, and nothing is
re-sent once a byte reached the client.

**Streaming fails over only before its first byte.** After that the stream is truncated
honestly, because no LLM API can resume mid-stream and a reconnect would replay tokens
the client already has.

### When a venue answers (`on_reply`)

`Policy::on_reply` hears the status each venue answered with, since a venue's own 503 is
relayed to the client and never reaches `on_failure`. `ReplyFacts` carries what
`FailureFacts` does to attribute it: the venue, the status as sent, the attempt (0 on the
first venue, more after a failover), the decision's tag and the request's seq; and whether it was a
stream. A policy that ejects venues counts the replies it chooses (kottos: 502, 503, 504
and 529) beside the failures, and a gateway status is never among them, so the 502 a client
gets after a failure or a reply already counted is not counted twice.

It fires once per venue response on the loop thread, before any of it reaches the client,
and cannot change it: a whole response once it is whole, byte-forwarded or translated, a
venue error a stream request got included; a 200 event stream at its head. A head whose
body cannot be framed (no length and not chunked, as a load balancer's close-delimited error
page often is; a broken chunk; a body over the cap) is still the venue's answer: its status
is reported when that is found, and the client gets the gateway's 502. A response cut
short before it was whole is reported to `on_failure` instead, so each attempt reaches at
most one of the two hooks. Two reach neither, and the client gets the gateway's 502: a
head the gateway refuses (a bad status line, both framings, a length over the cap), whose
status it does not trust, and an interim 1xx, which is not an answer. A reply to a client
already gone is not reported: the gateway closes that venue's connection unread, and the
venue's verdict on a request nobody waits for says nothing it would act on. Nothing is reported without a policy, and the default
does nothing; the cost is one virtual call per response.

A `RequestRecord` field holding the venue's own status was the alternative. It was turned
down because a record is written once the request is over, a stream's minutes after its
head said 200 or not, and only when a sink is installed; an embedder that ejects in its
policy, as kottos does, would have to feed its sink back into it. The hook reaches the
policy where it already counts failures, with the same attribution.

Still not in the seam: the body itself, and anything derived from it beyond the model
name, the two streaming flags and the opt-in prefix hash.

### Naming the key for a venue (`Decision::credential_header`)

A request may carry one provider key per `x-kottos-key-<domain>` header, so one route can
list venues that take different keys. The policy names the header holding the key for
the venue it picked, and a failover names the next venue's in
`Retry::credential_header`. The value goes out as the venue's own key header, by its
`VenueSpec` auth kind: `x-api-key` for Anthropic, `Authorization: Bearer` for
OpenAI-compatible venues and Cohere, `x-goog-api-key` for Gemini, `api-key` for Azure.
A request translated for Anthropic carries the client's `anthropic-version`, or
2023-06-01; a byte-forward keeps the client's head, so its `anthropic-version` goes as
sent, and none is added. The named key is the only credential the venue receives: a
byte-forward drops the client's `Authorization`, `x-api-key`, `x-goog-api-key` and
`api-key`.

The name is not known while the head is framed, so the framing walk records every family
line a policy could name (`key_header_ok`) with its trimmed value, as offsets into the
head, up to eight: a request reaches at most three venues, and there are six kinds. At
dispatch, per attempt, `named_key` matches the policy's name against that table and
reads nothing else. A translated request is still one walk of the head and a
byte-forward two, framing and the rebuild, whether or not a key is named, and nothing is
allocated. A strip-list entry for the named header does not hide it from that read: the
family never leaves anyway.

A named key never falls back to `Authorization`, which may hold another provider's key.
The request gets a 400 with fixed text, before any upstream is contacted, when the named
header is absent or empty, appears more than once in any case, or holds a control or
8-bit byte or more than 8 KiB; when the name is outside the family; when the head carries
more than eight nameable family lines, since a repeat could hide past the table; and
when the venue is Bedrock, which signs with a key pair and has no header for a bearer
key. A decision naming a key with an index past the end of the table gets a 500, before
any upstream is contacted. Unnamed, such an index means the first venue; named, the
first venue would receive a key meant for another provider as its own. -1, the default,
means the first venue either way. A failover that names no header after any earlier
attempt that named one, the decision or a retry, gives up, and the client sees the
failure; one that names a header the client never sent gets the 400.

The family itself never leaves. Every header whose name starts with `x-kottos-key-`, in
any case, is dropped from every byte-forwarded request, with or without a policy: at
dispatch, in the head of a body sent as it arrives, and in a stale connection's retry.
The strip list matches exact names, and a client picks the suffix. A translated request
is rebuilt from a whitelist and never carried one, and a sink cannot capture one.

### Dropping headers before they leave (`upstream.strip_headers`)

A policy decides; it cannot rewrite. So the companion to the seam is a list of header
names removed from every upstream request, matched case-insensitively and exactly. It
applies in two places, and the second is easy to miss: the passthrough copy, and the
credential scan, so a stripped `Authorization` is never promoted onto the provider key.

Empty by default, which keeps passthrough a single memcpy. It exists because
a byte-forward carried the client's bytes verbatim, headers included, which is
the "echo" this repo's own rule forbids: a header meant for this gateway, or an internal
routing header, reached a third-party provider. Translating modes were never affected;
they rebuild from a whitelist already. The key family is dropped whatever the list says
(above).

### A note at the end of a stream (`RequestSink::reply_note`)

A sink sees each request's record, and part of it is worth telling the client: the
request's cost is the case the hook was added for. The record is whole only once the
stream has ended, long after the response head went out, so a header cannot carry it, and
a `data:` event would reach an OpenAI or Anthropic SDK as a chunk to parse. An SSE comment
line is text a parser following the specification ignores, so the note is `: <note>` and
a blank line, after the last event and before the end, and only at a clean end. The
default returns nothing, and with no sink, or one that keeps the default, the client gets
the bytes it got before.

The note is a sink's text inside a venue's stream, so it is checked the way a policy's
message is: dropped whole, never repaired, when it is over `kReplyNoteMax` or holds a CR,
LF or NUL, any of which would end the comment and start an event. It is also withheld
after a byte-forwarded stream that stopped inside an event, which the note would join.
GATEWAY-INTERNALS.md 6d.

## Logging

A logger in a process that sells a latency number is a hazard, not a convenience.
Twenty interesting events per request at ~84k RPS is 1.7M records/second against a
budget of 41-80 us added p99 for the entire request; one `fprintf` costs 1-5 us and
takes a lock. Twenty of those exceed the whole published figure, so the wrong design
here invalidates the benchmark.

What follows from that:

- **Two gates.** A compile-time floor (`-DLLMBRIDGE_LOG_LEVEL=`, default `info`)
  removes lower call sites entirely, so their arguments are never even evaluated and a
  Release build carries no per-request logging code at all. A runtime level then gates
  what remains, as one relaxed atomic load and a predictable branch.
- **No allocation and no iostreams.** Lines are built in a fixed stack buffer and
  truncate; they never grow. `operator<<` would mean locales, virtual dispatch and
  allocation on the event loop; objects instead define a free `log_put` found by ADL,
  which reads the same at the call site and costs none of it.
- **One `write(2)` per line, no mutex**, so concurrent workers interleave whole lines
  instead of fragments.
- **A `Sink` seam.** The built-in sink writes to stderr, which is right for a sidecar.

**Identity is the point.** A log line nobody can attribute is worth nothing, so three
subjects lead every message: `ClientConnection#N`, `UpstreamConnection#N`, `Request#N`.
The instance number comes from a process-wide monotonic counter and is never reused;
the file descriptor is printed too, because it is what `ss`, `lsof` and `strace` show,
but it is a correlator and not an identity: the kernel hands fd 6 to the next
connection the moment the last one closes, and to either side of the proxy.
`Request#N` is the same sequencer value that `x-llmbridge-seq` reports, assigned once
when the request frames, so a log line and a response header cannot disagree.

**Levels carry a rule.** Every reply the gateway writes itself is WARN, a client's 4xx
included: one line per refusal, an authentication refusal naming its peer, a 5xx past
dispatch naming its venue. Timeouts and truncations are WARN. Every configured limit logs at
WARN when hit, including limits handled gracefully, because at the default floor a
DEBUG line does not exist in the binary: grading by consequence would have made epoll
back-pressure invisible in production while the io_uring stream drop stayed visible,
which is the backend divergence this codebase works hardest to avoid.

**No credential at any level.** Header names and lengths, never values. `SECURITY.md`
promises credentials are never logged, and the call sites are where that is kept.

## Configuration (`--config FILE`)

Flags remain the primary interface and `bench/*.sh` drives the daemon with eight of
them, so the file is **additive**: it is read first and every flag overwrites it.
Precedence is not positional, a flag wins wherever it sits, and `--config` twice is an
error instead of one file silently winning.

It exists because the next feature does not fit in flags. Multi-upstream routing needs
an ordered list with per-upstream fields (host, TLS, SNI, dialect, route tag), and flat
flags cannot express that without inventing a mini-language with none of the tooling
and worse errors. The shape is grouped (`listen`, `upstream`, `timeouts`, `runtime`) so
`upstream` can become an array without disturbing the rest.

**One table.** Each setting is one `Option` row in `app/options.hpp`: flag, key, kind,
bounds and choices. `parse_cli`, `parse_config` and `--help` all read it, so the two
inputs cannot drift into two validation rules, which is how the flags came to wrap a
port past 65535 and take a negative timeout the file refused. A key with no row is
refused, which is also what keeps a per-venue `strip_headers` from being accepted and
ignored: the list is gateway-wide, so in an `upstream` array it applies to every venue,
and entries that set it must agree.

Parsed with `provider/json.hpp`, the same hand-rolled parser the translator uses, so
the file adds **no dependency**. That parser is a zero-copy DOM over `string_view`, so
`Settings` copies every value out and owns `std::string`; a field holding a view
would be a use-after-free the moment the buffer went away, and a test clobbers the
input buffer before reading the values back to keep that honest.

**Unknown keys are a startup error.** This is the same discipline as the framing code:
a config parser that skips a misspelled `listen_tls` fails open, and unlike a command
line there is no `ps` output to catch it. Wrong types, bad enum values and
out-of-range numbers are refused the same way, each naming the key. Keys beginning
with `_` are comments, which is what makes strictness affordable in a format that has
none. The file holds paths, never secrets, so it never becomes a credential store on
disk. `app/llmbridge.example.json` documents every key at its default value, and a
test pins those values against the constants so the example cannot drift.

### Upstream URLs (`--upstream`)

`net::parse_upstream` accepts these forms:

| Form | Example | Transport |
|---|---|---|
| `IP:PORT` | `127.0.0.1:9001` | plain HTTP |
| `HOST:PORT` | `mock.internal:9001` | plain HTTP |
| `http://HOST[:PORT]` | default port 80 | plain HTTP |
| `https://HOST[:PORT]` | default port 443 | TLS |
| `https://HOST/BASE` | `https://api.groq.com/openai` | TLS |

The base path is a prefix, not a target: it is joined in front of whatever path the
request would otherwise use, so `/openai` + `/v1/chat/completions` reaches
`/openai/v1/chat/completions`. Several providers serve an OpenAI-compatible API below
the root (Groq at `/openai`, OpenRouter at `/api`, Fireworks at `/inference`). A base
path is rebuilt, never echoed: it lands in a request line, so anything that could split
or retarget that line is refused at parse time, never sanitised (`normalize_base_path`).

Refused, with a reason in `error` instead of a guess:

- **userinfo** (`https://a@b`), the classic URL-confusion trick where the host a person
  reads and the host we connect to differ.
- **a fragment**, anywhere: it never travels on the wire, so a URL carrying one is a
  paste error worth naming.
- **a query on a byte-forwarding venue.** Azure OpenAI needs `?api-version=`, which is
  parsed and kept, because a translating mode builds the whole target itself. A
  byte-forward has a client query to merge with, so the Gateway refuses that pairing at
  startup, where the mode is known; `parse_upstream` only reports what it found.
- **IPv6 literals**: the transport is `sockaddr_in`/`AF_INET` end to end, and
  half-accepting `[::1]:443` would fail later with a worse message.
- **host characters outside `[A-Za-z0-9.-]`**: the host is written into the Host header
  and the TLS SNI field, so a stray CR or LF is a header-injection primitive.

## TLS on both legs (`-DLLMBRIDGE_TLS=ON`)

Off by default. The default build stays **zero-dependency** end to end; enabling TLS
links OpenSSL (≥3.0), the one sanctioned runtime dependency, confined to the gateway.
The translator library (`provider/`) never links it, in any configuration. CI proves
both states: the gcc/clang matrix builds TLS-off, the sanitizer job TLS-on.

### Two sessions in opposite roles, never one pipe

The same build does both legs, and they are independent `Session` objects: the gateway
is the **server** for the client (`--listen-tls --tls-cert --tls-key`) and the **client**
to the provider (`--upstream https://...`). It terminates one and originates the other,
which is what lets it translate dialects and swap credentials in between. Neither leg
implies the other: TLS in with a plaintext upstream is a supported configuration and is
what terminating at the edge in front of a local model looks like.

What differs between the roles is only the handshake state (`SSL_set_accept_state`
against `SSL_set_connect_state`) and what gets verified. The client leg presents a
certificate and verifies nobody, because requiring client certificates is a product
decision we have not taken; the upstream leg verifies the provider's chain **and** its
hostname, with no way to disable it. Those are different decisions, and the code says
so at the site, so nobody "makes them consistent" later.

Deployment consequence, and it is the important one: TLS decides what is on the wire,
never who may connect. `llmbridge` authenticates no client. Either front it with
something that does, or keep the listener on loopback. See SECURITY.md.

### Why memory BIOs, not `SSL_set_fd`

`SSL_set_fd` assumes whoever owns the socket also makes the syscalls. On the io_uring
backend we don't: multishot recv hands the loop bytes that have **already been read**
into kernel-provided buffers; there is no read left for OpenSSL to perform. So the
loop keeps the socket and the `SSL` object is a pure byte transform behind a pair of
memory BIOs, identical on both backends. The cost is one memcpy per direction; the
crypto itself (AES-GCM with AES-NI) is noise next to the handshake RTTs.

The call protocol, identical on both backends:

1. `start_handshake()`, then loop: `pull_ciphertext()` and send; recv and
   `feed_ciphertext()`, until `handshake_done()`.
2. Steady state: `write_plaintext()`, then `pull_ciphertext()` and send; recv,
   `feed_ciphertext()`, then `read_plaintext()`.
3. **Drain `pull_ciphertext()` after any call that advances the state machine.**
   OpenSSL emits protocol bytes (key updates, alerts, close_notify) with no application
   data involved, and forgetting them is the classic memory-BIO stall.

Nothing on the steady-state path allocates beyond OpenSSL's own internals; the BIO pair
is created once per connection.

### Data flow

```
                     PLAINTEXT ONLY                    │            CIPHERTEXT ONLY
                                                       │
 client ──► rbuf ──► translate ──► u->out ─────────────┤
 (plain)                            (request,          │
                                     kept intact       ▼
                                     for retry)   write_plaintext()
                                       sent ─────►┌─────────┐   pull_ciphertext()
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

**`Connection::rbuf` and `Connection::out` hold plaintext, always.** TLS interposes
strictly at the socket edge. Three things fall out of this for free:

1. **Nothing downstream knows TLS exists**. HTTP framing, the SSE pump, translation,
   and the response parse all read `rbuf` exactly as before.
2. **Stale-pool retry works unchanged**. `out` is never consumed by encryption
   (sent means plaintext *fed to the session*, not bytes destroyed), so a retry
   re-pushes the identical request through a brand-new session.
3. **The two backends share all TLS logic**; only the flush/kick differs: epoll
   writes `tls_out` inline and arms `EPOLLOUT` on a partial; io_uring serializes one
   send at a time, because a send SQE points into `tls_out` and the buffer must stay
   immutable while the kernel reads it. `tls_out` is an `OutBuf`, so ciphertext
   produced meanwhile stages behind the pinned front until the send completes.

### Security posture (decided, not defaulted)

- **Verification cannot be disabled.** `SSL_VERIFY_PEER` plus `SSL_set1_host`: chain
  *and* hostname, both derived from one argument so they cannot disagree. A failed
  `init_client` tears the session down (fail closed: a caller ignoring the return
  cannot handshake unverified). SNI and the verified name are set together.
- TLS 1.2 floor, server-initiated renegotiation refused (`SSL_OP_NO_RENEGOTIATION`),
  `ERR_clear_error()` before every operation (a stale thread-local queue entry
  misclassifies a benign `WANT_READ` as fatal), int-length clamping on all spans.
- A **fatal record error mid-stream aborts the client** on both backends. A corrupted
  stream must never be finalized with a clean `[DONE]`.
- **Not done, stated:** no OCSP/CRL revocation checking (usual for non-browser
  clients); a provider EOF without `close_notify` is treated as normal end-of-stream
  (providers rarely send it; strict truncation detection would break real streams).
  A translated Anthropic stream that ends before its `stop_reason` still fails: the
  end is stated in-band, so no finish chunk or `[DONE]` is fabricated.

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

The inbound leg adds its own: handshake split across single-byte writes, plaintext sent
to a TLS listener refused with nothing served, three disconnect tests (mid-handshake,
mid-request, immediately after a request) at twenty iterations each under ASan and
UBSan, a bounded-buffering proof for a handshake that never completes, and a check that
a client which never reads cannot grow the gateway without bound. A libFuzzer target
(`fuzz/fuzz_tls_server.cpp`) drives arbitrary bytes at a server-role `Session`, because
that is the first surface an unauthenticated remote peer reaches. The whole inbound
suite also runs clean under ThreadSanitizer.

## Benchmark methodology

The benchmark is designed to be honest first and impressive second:

- **Equal work.** Both `llmbridge` and the LiteLLM comparison do the *full* OpenAI⇄Anthropic
  translation against the **same 200 ms mock backend** (`bench/fastbackend.cpp`,
  `bench/mock_provider.py`).
- **Coordinated-omission-corrected, open-loop load** (`bench/loadgen.cpp`, wrk2
  methodology): requests are scheduled on a fixed timeline, so a stall is counted
  against every request it delayed, not hidden. Warmup is gated out; the histogram
  reports conservative bucket-upper-bound percentiles and flags overflow.
- **Latency accounting** excludes the upstream wait and inter-packet network time, so
  the headline number is *proxy-added* latency, not end-to-end LLM latency.
- **Caveats, stated up front:** a single co-located dev box (i7-9750H), single
  worker/thread each, plain HTTP (no TLS/WAN). The ~84k RPS single-thread ceiling is
  set by **the worker's own CPU on its single thread**. llmbridge is single-threaded, so
  one core is its hard ceiling and at saturation that thread is 87-92% busy, out of
  headroom. (The machine reports ~95% idle only because one busy core is ~8% of 12 logical
  CPUs; reaching the rest needs `--workers N`.) Splitting *that thread's own* CPU time:
  **89% kernel, 6.7% llmbridge**, the largest kernel slice being the TCP stack at 32.7%.
  Two consequences: optimising our code can win at most ~7%, and the number is
  **thermally dependent** (87k cold, 82k at 85 °C on this laptop).
- **A harness we did not write.** `bench/run_enterpilot.sh` runs the gateway inside the
  public [ENTERPILOT benchmark](https://github.com/ENTERPILOT/ai-gateway-reproducible-benchmark)
  beside eight other gateways, with a socat byte pipe as the in-path floor; see
  BENCHMARKS.md §C.

Reproduce: see [`bench/BENCHMARK-CONFIG.md`](bench/BENCHMARK-CONFIG.md), the host
configuration (`BACKENDS=4`, governor, sysctls) changes the result as much as the code.

## What this repo does not do (yet)

Checked against the tree, not against memory; `CHANGELOG.md` is the source of truth
when this list disagrees with it.

- **Streaming is OpenAI ⇄ Anthropic only.** Gemini and Cohere translate
  non-streaming requests and responses and have no SSE translator, so they do not
  stream here.
- **No Anthropic-in translation.** A client speaking Anthropic to an Anthropic venue
  byte-forwards; a client speaking Anthropic to an OpenAI venue needs
  `OpenAiToAnthropicSse` and its request twin, which are not written. The pair is
  refused, never mistranslated.
- **No Bedrock streaming.** Bedrock streams from a different endpoint in AWS
  event-stream framing, and that binary decoder is not built; a streamed request to a
  Bedrock venue is refused.
- **No vision.** A non-text content part is refused by name. `cache_control` ships.
- **No failover policy.** `Policy::on_failure` and `Policy::on_reply` are the mechanism;
  the default never retries and counts nothing.
- **No language bindings.** Python, Go and Rust are planned.
- **No routing policy, no matching, pricing or observability**, by design; that is the
  separate commercial layer.

## Roadmap

Derived from `git tag` and CHANGELOG.md, which are the source of truth when this
disagrees with them.

- **Next:** Bedrock streaming, and an Anthropic-speaking client to a Bedrock venue.
- **Then:** Anthropic-in translation; streaming for Gemini and Cohere; vision.
- **Later:** Python, Go and Rust bindings.
- **v1.0.0:** API stability, only after six months of public use.
