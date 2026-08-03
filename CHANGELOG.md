# Changelog

All notable changes to `llmbridge` are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html) — with one
pre-1.0 caveat: **the API is unstable until v1.0.0, so breaking changes may land in
minor (0.x) releases.** Breaking changes are always called out explicitly below.

## [Unreleased]

Next up: **Anthropic-in mode** (clients that speak the Anthropic API, fronting an
OpenAI-compatible upstream), Gemini / Cohere streaming, vision / image inputs, and
`cache_control`.

## [0.8.1] — 2026-08-03

A security sweep of the public HTTP surface. **Fixes only — no new functionality**,
hence a PATCH. No public API change: the one signature that moved
(`http::parse_response`) lives in `net/`, which is internal to the reference gateway
and is not among the installed headers (only `provider/` is).

### Compatibility note

This release **rejects requests that previous versions accepted**, deliberately.
A request whose header block contains a bare CR or LF, an obs-fold continuation
line, whitespace before a colon, a header line with no colon, or a non-numeric
`Content-Length` (`0x1b`, `27abc`) now gets a `400` and is not forwarded. Each of
those was a framing-desync primitive — see below. Well-formed HTTP/1.1 is
unaffected, including the legal trailing whitespace in `Content-Length: 27 `.

### Security — HTTP framing sweep

A systematic sweep of the public request/response surface, done by probing rather
than reading. Eight defects, seven of them in the framer and all with the same
signature: **a malformed header became an invisible header instead of a rejection.**
`parse()` returned `Complete` with `body_len == 0` while the body sat unconsumed.

That is a request-smuggling desync. In passthrough mode (`--translate none`) the
gateway forwards the client's header block verbatim, so an upstream that reads a
length we could not frames a body we never sent — and because the upstream pool is
**shared between clients**, the leftover bytes become the head of another client's
request. Each fix refuses the message; none sanitises and forwards.

Fixed in `parse()` (requests) and `parse_response_head()` (responses):

- **Non-numeric `Content-Length` accepted.** `std::from_chars` stops at the first
  non-digit and still reports success, so `0x1b` parsed as **0** and `27abc` as
  **27**. Now `1*DIGIT` only, per RFC 9112 §8.6. Legal trailing OWS (`"27 "`) is
  still accepted — the fix must not over-reject.
- **Whitespace before the colon.** `Content-Length : 27` matched no `name:` prefix
  test, so the length went unseen. RFC 9112 §5.1 forbids it for this exact reason.
- **Bare CR / bare LF in the header block.** We split on CRLF; a parser that splits
  on a bare CR sees headers we never saw. Measured reaching an upstream.
- **obs-fold continuation lines** (`\r\n Content-Length: 27`) — invisible to a prefix
  matcher, honoured by a folding upstream. RFC 9112 §5.2 requires rejection.
- **Header lines with no colon, or an empty field name.**
- **Responses carrying both `Content-Length` and `Transfer-Encoding: chunked`**, and
  **conflicting duplicate `Content-Length` in a response** — neither was rejected.
  A mis-framed *response* on a pooled connection hands one client another's bytes,
  so response framing is now as strict as request framing.

A test that asserted the first of these as a documented parser quirk
(`TrailingGarbageAfterClNumberIsAccepted`) has been **replaced by one asserting the
rejection**. It was a smuggling primitive, not a quirk. The new `HttpDesync` suite
covers every case above and asserts end-to-end that a refused request reaches the
upstream as **zero bytes** — fail closed, verified at the wire, not merely at the
parser.

### Fixed — quadratic cost on the non-streaming chunked path (availability)

`parse_response()` built a fresh `ChunkDecoder` and re-decoded from byte zero on
every call, and it is called on every read. A body arriving in N reads therefore cost
O(N × body). This is the non-streaming response path for **both backends, TLS or
plaintext** — not a TLS-specific path. Measured on one connection with 64 KiB reads:

| body | before | after |
|---|---|---|
| 1 MB | 1.6 ms | 1.3 ms |
| 2 MB | 4.3 ms | 3.1 ms |
| 4 MB | 13.9 ms | 6.5 ms |
| 8 MB | **79.0 ms** | **14.1 ms** |

**Scope, stated precisely.** A typical 1 KB reply — and an 8 KB one — arrives in a
single read, so the re-decode loop never runs: 0.4 µs before and after. The defect
needs a body spanning several 64 KiB reads, so it does not appear below ~128 KB and
does not cost a millisecond until roughly 500 KB–1 MB. Normal traffic was never
affected, and this was never a regression against the p99 < 1 ms added-latency
target, which is measured at ~1 KB payloads.

The harm is **head-of-line blocking**, not the latency of the request that triggers
it. The loop is single-threaded, so a 79 ms decode delays *every other client on
that worker* by up to 79 ms — one oversized response degrades everyone else's p99.
An unusually large legitimate completion can trigger it; a hostile or compromised
upstream can do it deliberately, at will, up to `kMaxBodyLen` (16 MiB), which is the
threat that justifies the fix. Decode state now lives per connection
(`http::ResponseDecoder`) and is fed only newly-arrived bytes, and is reset between
responses on a pooled connection. The regression test asserts the invariant
deterministically — the decoder's consumed count only moves forward, and never by
more than the bytes that just arrived — rather than timing it, because a timing
assertion flaked on allocator warmth and a flaky security test is one people ignore.

### Fixed — flaky TLS test fixture

The TLS test CA was written to a fixed path in the shared temp dir, so parallel
`ctest -j` processes truncated each other's file, surfacing as a spurious
"no certificate or crl found" in whichever test lost the race. Now unique per
process and per call.

### Verified clean (probed, not assumed)

- **Credentials and the shared pool.** A key never inherited by a client that sent
  none, never trailing a shorter reused request, never echoed as `Authorization`
  upstream, never in an error body or the gateway log.
- **TLS.** No bypass path exists — verification is unconditional, with tests for an
  untrusted chain, a hostname mismatch, and explicitly no plaintext fallback.
- **Availability.** Survives 300 slow-loris connections, 100k-deep nested JSON
  (depth capped at 64), an over-cap `Content-Length` and an over-cap header block —
  all refused with 400, process alive and still serving throughout.

Known gaps are stated in `SECURITY.md`, which now also documents the **inbound
plaintext leg** (client → gateway is HTTP, so the OSS gateway is a loopback sidecar,
not a remote endpoint) and the **absence of a client-side idle timeout**.

## [0.8.0] — 2026-08-03

Streamed tool calls. A `"stream": true` request whose model calls a tool now produces
proper OpenAI `tool_calls` deltas, so an agent loop works over SSE. This **supersedes
the guard added in 0.7.0**, which aborted such streams rather than mis-reporting them.

Verified against the live Anthropic API, and re-verified after every round of audit
fixes below — most recently on all three paths at once (streamed parallel calls,
streamed text with tools declared, non-streaming):
two parallel calls arrived as fragments (`{"city": "P` / `ar` / `is"}`) and
reassembled client-side into valid JSON, with `finish_reason: "tool_calls"` and a
clean `[DONE]`; a text stream with tools declared is unaffected.

### Added

- **`content_block_start` → the opening `tool_calls` delta**, carrying `index`, `id`,
  `type:"function"` and `function.name` with an empty `arguments` — the chunk an
  OpenAI client keys off to begin a new call. The role chunk is emitted first even
  when a stream opens straight into a tool call with no text.
- **`input_json_delta` → `arguments` fragments** under the same index, which the
  client concatenates.

### The index mapping (the part that is easy to get wrong)

Anthropic indexes **every content block**, text included; OpenAI's
`tool_calls[].index` counts **only tool calls**. The two diverge the moment text
precedes a call — Anthropic blocks 1 and 2 must become OpenAI ordinals 0 and 1.
Forwarding Anthropic's index directly would emit `tool_calls[1]` with no `[0]`, which
breaks reassembly in every SDK. A dedicated test (`BlockIndexIsNotTheToolOrdinal`)
pins this.

The map is a bounded vector (256 blocks) holding `-1` for non-tool blocks, so a
hostile index cannot make the translator allocate.

### Arguments forward as raw spans

Anthropic's `partial_json` and OpenAI's `arguments` are both JSON strings whose
*contents* are JSON text, escaped identically — so fragments pass through **verbatim**
rather than being decoded and re-encoded. A round trip through the DOM could alter a
customer's argument bytes; the test asserts fragments reassemble byte-for-byte
including `1.50`, which a re-serialising implementation would normalise to `1.5`.
Control bytes are still neutralised on the way out, as for text.

### Hardening found by pre-merge audit

Seven defects across three review passes, **every one caught by probing edge cases
rather than reading the code** — the reasoning pass before them found none. Two were
introduced by earlier fixes in this same list, which is why the last pass was done as
a cold second reading of `dispatch()` and `finish()` rather than another look by their
author.

- **A malformed `index` stole another call's arguments.** `detail::to_ll()` wraps
  `std::from_chars`, which on overflow or garbage leaves its output **untouched** — so
  `"index": 99999999999999999999` came back as `0` and its argument fragments were
  attached to whichever tool call occupied block 0. In an agent loop, arguments on the
  wrong tool means the wrong action. A strict parser now rejects overflow, garbage and
  trailing characters; the event is ignored.
- **A truncated tool call fabricated a clean ending.** Arguments cut mid-JSON still
  produced `[DONE]`, so a client concatenated unparseable JSON inside a stream that
  looked complete — the exact "corrupt framing fabricated a clean ending" failure
  0.3.0 fixed for text, reintroduced by streaming tool calls. Truncated prose is still
  readable; truncated arguments are garbage a client may dispatch. `finish()` now
  returns `false`, which the gateway already maps to `Failed`.
- **Unnamed tools were emitted.** The non-streaming translator drops a tool with no
  name ("unusable without a name"); streaming emitted `"name":""`, so the same
  upstream produced a usable response one way and an undispatchable call the other.
  Now consistent.
- **`finish()` reported failure on an already-complete stream** — a regression
  introduced by the truncation guard above. It checked the open-tool flag *before*
  `_done`, so an upstream sending `message_stop` without a preceding `message_delta`
  got `[DONE]` from `dispatch()` and then a failure from `finish()`, making the
  gateway count an error and close abruptly on a correct response. Order fixed, and
  `message_stop` now clears the flag too, so the invariant holds whichever terminator
  arrives.
- **The tool-ordinal counter was unbounded.** A reopened block index could increment
  it without limit; signed overflow is UB. Capped at 256 — no legitimate response has
  more tool calls than that.
- **`finish_reason: "stop"` was reported after emitting tool calls** — the worst bug in
  the release, because it fails *silently*. Every OpenAI SDK branches on
  `finish_reason == "tool_calls"` to decide whether to dispatch, so a client receiving
  `"stop"` treats the call as a plain answer and **never runs the tool**: no error, no
  exception, just a tool that quietly does not fire. Reachable whenever the upstream
  sends `message_stop` without a preceding `message_delta`. The default is now
  `"tool_calls"` once any call has been emitted, and a text-only stream still defaults
  to `"stop"` (pinned by its own test so the new default cannot leak).
- **A foreign `data: [DONE]` vouched for a truncated call.** `[DONE]` is an OpenAI-ism —
  Anthropic ends with `message_stop` — and the handler jumped straight to the sentinel.
  That gave a truncated tool call a clean ending *and* skipped the finish chunk, so the
  stream ended with `finish_reason: null` and a client had no way to know the message
  had ended. It now routes through the same path (a complete stream still gets its
  finish chunk) but does **not** clear the open-tool flag: a foreign terminator cannot
  vouch that arguments are whole.

**Fuzz coverage.** The SSE corpus contained **no tool events**, so
`content_block_start` and `input_json_delta` were never fuzzed. Two seeds added
(parallel calls behind a leading text block; hostile indices with escaped `id`/`name`).
2.87M executions clean afterwards — which matters most for invariant (2),
fragmentation-invariance, since this release added cross-event state and the fuzzer's
own comment calls that "the property most likely to break as state grows".

### Tests

**21 new** in the translator, replacing 0.7.0's four guard tests: opening chunk shape and role ordering;
fragments reassemble exactly; **block index ≠ tool ordinal**; interleaved fragments
route to their own call without cross-contamination; `finish_reason:"tool_calls"` now
has calls behind it — the regression this feature existed to fix; a fragment for a
block whose `content_block_start` was never seen is **ignored rather than guessed at**
(attaching a customer's arguments to the wrong call is worse than dropping them); an
absurd block index does not allocate; plain text streams unaffected.

From the audit: malformed indices (overflow, negative-overflow, `1e5`, `0x10`) are
rejected rather than aliased to block 0; a malformed index opens no call; every
emitted chunk stays valid JSON under hostile escaping (escaped quotes, trailing
backslashes, `\uXXXX`, tabs, and a hostile `id`/`name`); tool chunks carry
`usage:null` when `include_usage` is set; unnamed tools are dropped; a truncated call
refuses a clean ending while a completed one still gets `[DONE]`; `message_stop`
without `message_delta` still ends cleanly; the ordinal counter is bounded.

**6 new gateway tests**, both backends × chunk sizes {7, 64, 4096}: a streamed tool
call survives the gateway pump and reassembles, with ordinals 0/1 despite Anthropic
using blocks 1/2. Small chunk sizes matter because tool events are longer than text
events and so more likely to straddle a boundary mid-JSON. Before this, the string
`input_json_delta` appeared **zero** times in the gateway suite — the pump path was
verified only by hand against the live API.

Suites: **866/866** with TLS, **840/840** default, **866/866** under ASan+UBSan.

### Known gaps

- **The end-to-end streaming benchmark has not been re-run** on a quiet box. A
  microbenchmark of the translator's per-event path shows **+4.5 ns/event (<1%)**
  against the pre-change build, but that does not cover the gateway pump. Absolute
  streaming figures in `BENCHMARKS.md` are unchanged and were not re-measured.
- **An empty tool `id` is still forwarded.** The non-streaming translator does not
  check it either, so both paths agree; making them both reject it belongs in a change
  that touches the two together.
- Streaming remains OpenAI ⇄ Anthropic only — Gemini and Cohere have no streaming
  translator, tool calls or otherwise.


## [0.7.0] — 2026-08-03

Tool calling, non-streaming. An OpenAI-dialect client can declare tools, receive a
tool call, execute it and return the result — the full agent loop — against an
Anthropic upstream. Verified end to end against the live API: Claude called
`get_weather` twice in parallel and, given the results, answered *"The current
weather in Paris is 18°C with light rain."*

### Streaming tool calls fail cleanly (rendering them is next)

> **Superseded by 0.8.0**, which renders streamed tool calls properly. The guard
> below describes 0.7.0 only; tool calls no longer require a non-streaming request.

Tool calls require a **non-streaming** request. Rendering Anthropic's streamed
`tool_use` / `input_json_delta` as OpenAI `tool_calls` deltas is the next change.

Until then a streamed tool call **aborts the stream** — no `[DONE]`, the same signal
this translator already uses for a corrupt body — rather than completing with a
misleading result.

That guard is the point. Before it, the SSE path handled `text_delta` only, so the
`tool_use` block and its argument fragments were **silently dropped while
`stop_reason: tool_use` still mapped through to `finish_reason: "tool_calls"`**. The
client was told *"I called a tool"* and handed no `tool_calls` array: a response that
looks valid, is not, and would send an agent loop hunting for a call it never
received. Failing is strictly better than that, and it is what a client can actually
detect.

### Added

- **Tool declarations** — OpenAI `tools[].function{name,description,parameters}` →
  Anthropic `{name,description,input_schema}`. The schema is forwarded as a **raw
  byte span**, never rebuilt: a customer's JSON Schema is arbitrary, and
  re-serialising it from the DOM could change number formatting (`1.50`), escape
  forms or key order. Tested by asserting the exact source text appears in the
  output.
- **`tool_choice`** — `"auto"` → `{"type":"auto"}`, `"required"` → `{"type":"any"}`,
  `{"type":"function","function":{"name":…}}` → `{"type":"tool","name":…}`, and
  `"none"` → the tools block is omitted entirely, which is how Anthropic expresses
  it.
- **The call itself** — OpenAI carries arguments as a JSON **string**, Anthropic as
  a JSON **object**. Crossing that boundary is a real conversion in both directions,
  which is why `json.hpp` grew `append_escaped_string()` and `unescape_string()`.
- **Tool results** — OpenAI `role:"tool"` + `tool_call_id` → an Anthropic **user**
  turn containing `tool_result` blocks. Consecutive tool messages merge into one
  turn: a parallel call is semantically one turn of results. (Measured, not assumed —
  the live API accepts consecutive same-role turns and returns 200; an earlier code
  comment claimed otherwise and was wrong.)
- **Responses** — `tool_use` blocks → OpenAI `tool_calls[]`, with `content` set to
  **`null` rather than `""`** on a pure tool call, because SDKs branch on null and an
  empty string reads as "the model answered nothing".
- `json.hpp`: `Value::sv` now carries the **raw span for objects and arrays**
  (brackets included), which is what makes byte-for-byte schema forwarding possible.
- **Guard: a streamed tool call fails the stream** rather than dropping the call.
  Detected at `content_block_start` (so it triggers even for a zero-argument tool)
  and at `input_json_delta`, then routed through the same sticky-failure path as a
  cap overflow. Text-only streams are untouched.
- `Stats::connect` histogram and `Connection::ts_wire_ready` stamp — see the
  latency-profile fix below.
- `bench/fastbackend --tools` — a mock serving an Anthropic response with two
  `tool_use` blocks. Without it the regression sweep reports "no change" for edits
  that only touch the tool path; a live check had already shown 19 µs for tool
  responses against 15 µs for plain, a difference the benchmark could not see. Body
  selection is now an `enum` rather than two bools, so `openai+tools` — a state that
  does not exist — is unrepresentable.

### Fixed: the self-reported latency profile counted the wrong things

`Stats::req_path` ran from *request framed* to *bytes on the wire*, so the **TCP
connect and TLS handshake sat inside it** — and inside `added-total`, the profile's
headline. Harmless against a warm pooled mock, badly wrong against a cold real
provider: a live single-request run reported `request-path p50 = 52.66 ms
[overflow!]`, of which ~52.6 ms was the handshake and ~60 µs was the gateway.

The stamps now give three intervals instead of two:

```
ts_req_recvd ──► ts_req_built ──► ts_wire_ready ──► ts_up_sent ──► ts_up_recvd ──► sent
   request        framing +        socket ready      request        provider's
   framed         translate +      (handshake done   fully          first byte
                  auth mapping      if it was cold)  written
```

- `req_path` — framing, translation, auth mapping **plus the `write()` to the
  upstream**
- `connect` — the TCP + TLS handshake **alone**; exactly 0 on a pooled connection
- `added-total` — `req_path` + `resp_path`, unchanged in meaning: everything the
  gateway does

**The first attempt at this split was wrong in the flattering direction, which is
why it is written up.** It treated the whole `ts_req_built → ts_up_sent` interval as
"connect" and excluded it. But a pooled connection performs no handshake, so that
4.4 µs was the `write()` syscall — unambiguously our cost. Measured under identical
warm-pool conditions:

| | before | first (wrong) cut | correct |
|---|---|---|---|
| `added-total` p50 | — | 8,560 ns | **12,700 ns** |
| `request-path` p50 | 1,920 ns | 1,920 ns | **6,140 ns** |
| `connect` p50 | 4,420 ns | 4,420 ns | **20 ns** |

`connect` at 20 ns is a stamp subtraction on a pooled connection — correctly zero.
The rule that caught it: when a number improves, ask what else changed. It had
improved because a real cost stopped being counted.

**No published figure moves.** `BENCHMARKS.md`'s added-latency numbers come from
`loadgen`'s client-observed measurement, not this histogram; the profile is a
diagnostic printed at shutdown. The consequence is only that the profile previously
overstated `added-total` on cold connections and was unaffected when warm — so a
cold-start run quoted from the profile rather than the client measurement would have
been too high.

### Robustness

Malformed tool pieces are **dropped, never guessed at**: a tool with no name, a call
with no `function`, empty or absent `arguments` (→ `{}`). A half-formed tool would
make the provider fail in a way the client cannot read.

Escaping is handled properly rather than approximately: control bytes are
`\u`-escaped (a raw control byte in a JSON string is invalid JSON that some parsers
accept and others reject), `\uXXXX` decodes to UTF-8 including surrogate pairs, and
a **lone surrogate becomes U+FFFD** rather than invalid UTF-8 that would make a
provider reject the whole body.

### Tests

**47 new.** Translator (24): declaration shape, byte-for-byte schema forwarding, all
five `tool_choice` cases, arguments-string ↔ input-object, parallel calls, text+call
in one turn, tool-result turns, consecutive-result merging, the trailing-turn close
(a malformed-JSON regression guard), malformed pieces dropped, `content:null`, no
`tool_calls` key on a plain answer, and a **full round trip** — Anthropic `input` →
OpenAI `arguments` → back to Anthropic `input` — through `"say \"hi\"\nnow"`,
`a/b\c`, `-1.5e3` and `café`.

JSON layer (7): raw spans include their brackets, whitespace and `1.50` preserved,
escape round-trip, control bytes escaped, surrogate pairs, lone surrogate → U+FFFD,
truncated escapes do not read past the end.

Streaming guard (4): a streamed tool call aborts and **never emits
`finish_reason:"tool_calls"` or `[DONE]`**; argument fragments alone also abort; the
failure is sticky, so later well-formed text cannot resurrect the stream; and a
plain text stream still completes normally.

Gateway, both backends (12): round trip; upstream receives `input_schema` and never
`parameters`; **tools and auth headers coexist** (both rebuild the request, so this
catches one clobbering the other); tool-result turns forward with `input` as an
object; **a streaming request with tools declared still streams**; a 60-property
schema survives framing intact.

Suites: **843/843** with TLS, **817/817** default.

### Performance

No regression on normal traffic: interleaved, temperature-gated A/B at 20k RPS
against the pre-tool-calling build gave **identical p99 minimum and +5 µs median**,
with the control holding 20,000 at 120 µs p99. A cold saturation sweep reached
**87,933 RPS** at 90k offered — the top of the canonical 84–87k band.

Tool-response translation itself costs about **+4 µs** (19 µs vs 15 µs for a plain
response), measured live. The bench-harness A/B that would confirm it needs a cold
box: the attempt ran with the machine at its 73 °C thermal floor and produced a
4,540 µs outlier on a *plain* run, so no number is published from it.

## [0.6.0] — 2026-08-01

Per-request observability metadata. A client can now see, for every response, what the
gateway cost versus what the provider cost, **when** the request arrived, **where it
sits in a total order**, and how many tokens it moved. Opt-in (`--timing-headers`,
default off), because adding a header is a visible API change.

### Added

- **`--timing-headers`** — response headers decomposing where the time went. Metadata
  only: durations and one timestamp, never prompt or completion text. "Prompt content is
  never logged" is a public commitment and this path does not touch it.

  ```
  t0 ────────► t1 ────────► t2 ──────────────────► t3 ──────────► t4
  client req    upstream     bytes on the wire      provider       response
  fully framed  request      (connect + TLS done)   first byte     written to client
                BUILT
  ```

  | header | interval | meaning |
  |---|---|---|
  | `x-llmbridge-t0` | — | epoch **nanoseconds** at request arrival |
  | `x-llmbridge-seq` | — | monotonic sequence number — a total order needing no clock |
  | `x-llmbridge-gateway-us` | (t1−t0)+(t4−t3) | **our compute**: framing, translation, auth mapping, re-serialisation |
  | `x-llmbridge-connect-us` | t2−t1 | TCP connect + TLS handshake; ~0 on a pooled connection |
  | `x-llmbridge-upstream-us` | t3−t2 | the provider: network + inference |
  | `x-llmbridge-tokens-in` | — | prompt tokens, **the provider's own count** (non-streaming) |
  | `x-llmbridge-tokens-out` | — | completion tokens, likewise (non-streaming) |

  Streaming cannot report t4 — headers precede the body — so it emits
  `x-llmbridge-upstream-ttfb-us` (time to the provider's first byte) instead of a total
  it does not yet have.

  Measured live against `api.anthropic.com`: **gateway 17–51 µs** against **1.2–2.6 s**
  of provider time — a gateway share of **0.001%** — with `connect-us` falling from
  ~50,000 µs on the cold connection to ~36 µs once pooled, which incidentally makes
  connection reuse visible per request.

- **`x-llmbridge-seq`** — a process-wide monotonic sequence number, `std::atomic<uint64_t>`
  with `fetch_add(relaxed)`. **Not `volatile`**: volatile provides neither atomicity nor
  inter-thread ordering in C++, and workers are `std::thread`s sharing the process, so a
  volatile counter would be a data race that hands two requests the same number. Relaxed
  ordering is sufficient and cheapest — every atomic has a single total modification
  order, so the values are unique and increasing in the order the increments happened;
  we need the counter ordered, not the memory around it.

  **Why it exists:** two requests can share a nanosecond, and clocks on different hosts
  cannot be trusted to sub-millisecond agreement without PTP. `(t0, seq)` is a total
  order that needs neither. This is the sequencer pattern — an exchange defines order by
  arrival at a sequencing point, not by comparing timestamps — and it is the argument
  for the shadow order book **sequencing rather than timestamping**.
- **`x-llmbridge-tokens-in` / `-out`** — the provider's own counts, never estimated,
  asserted by a test that the header value **equals the body value exactly** (one source
  of truth). Extraction is a bounded scan of the last 256 bytes, since `usage` is the
  tail object of the response we build — a multi-KB completion does not pay an extra
  full pass. No match means the headers are omitted rather than guessed.
- **`metrics::wall_ns()`** — an orderable wall clock. `now_ns()` is `steady_clock`:
  correct for intervals, but it has no epoch and cannot order anything. A raw
  `CLOCK_REALTIME` read is not a substitute either, because NTP can **step** it, leaving
  two requests orderable by arrival but not by timestamp — the one property an order
  book cannot lose. So realtime is read **once** at startup and every later stamp is
  that anchor plus a monotonic delta: epoch-meaningful, strictly increasing, immune to
  NTP steps. Trade-off stated at the definition: ppm drift from true wall time over long
  runs, and cross-host joins at sub-millisecond accuracy need PTP, which one gateway
  cannot promise alone.
- New `Connection::ts_req_built` stamp — the end of our request-side work, which is what
  makes the gateway/connect split possible.

### Why `connect-us` is separate

The first implementation folded connection setup into `gateway-us`, and the live API
said **56 ms**. It was measuring truthfully and reporting misleadingly: a cold TCP+TLS
handshake to Anthropic, against 47–63 µs for the same gateway once the connection was
pooled. A customer reading 56 ms as gateway overhead would be right to walk away and
wrong about the software. One number cannot honestly carry both, so there are two.

### Tests

16 new, both backends: headers absent by default; all present when enabled; **t0 is
epoch-scale** (>1.7e18 — a monotonic uptime counter would be ~1e10); **t0 strictly
increasing across five requests**; the JSON body byte-identical with headers on;
streaming emitting TTFB while *not* claiming a gateway total it cannot have; token
headers matching the body exactly; and streaming asserted to carry `seq` but **no**
token headers.

The sequence-number test drives **4 threads × 6 concurrent requests and asserts no
duplicate** — the one failure this must never have, and the one a non-atomic counter
would produce.

### Known gaps

- **Passthrough (`--translate none`) emits no headers at all.** That mode forwards the
  upstream's bytes verbatim by contract; injecting headers would break the byte-exact
  guarantee. Translated and streaming responses carry them.
- **Streaming carries no token counts and no chunk count.** Both are end-of-stream
  facts and headers precede the body; inventing them would be worse than omitting them.
  A streaming client that wants counts sets `stream_options.include_usage` and reads
  the provider's own numbers from the final chunk. A chunk count would have to go
  somewhere other than the response — gateway stats, or the telemetry path.
- Timestamps are per-process. Ordering holds within one gateway; across workers or hosts
  it needs synchronised clocks, which is a shadow-order-book concern rather than a
  gateway one.

## [0.5.2] — 2026-07-31

Performance and API cleanup for the two preceding releases. No behaviour change: the
same requests produce the same bytes. Found by an A/B regression check against the
pre-0.5.0 build, not by profiling a guess.

### Fixed

- **Auth-header scanning walked the client's header block six times per request** —
  four to validate the credential-bearing headers, then up to two more to fetch the
  value it needed. One pass now collects all four into a struct. Measured on a
  thermally-gated interleaved A/B at 20k RPS: this cost **~40 µs at p99 (~8%)** with
  p50 unchanged — the signature of a little extra work on every request. After the
  fix the two builds' p99 distributions interleave (min −20 µs, median +15 µs vs the
  pre-0.5.0 baseline), i.e. no measurable difference. First-occurrence-wins is
  preserved, and validation still covers every credential header the client sent, not
  only the one the target dialect uses.
- **`parse_response` copied the body on the `Content-Length` path**, where the
  previous `parse()` handed out a `string_view` over the receive buffer. 0.5.1
  introduced that copy on the highest-rate path for no reason; the body is already
  contiguous there. It is now a view again. The copy remains only for **chunked**,
  where it is inherent to the encoding — body bytes are interleaved with chunk-size
  lines and cannot be viewed in place.
- **The chunked decode buffer allocated per request.** It is now a single buffer
  reused across requests (safe because the loop is single-threaded and a response is
  framed and consumed entirely within one event). This matters more than it sounds:
  chunked is the path real providers actually use, so that allocation was being paid
  on every non-streaming call.

### Changed

- `http::parse_response()` returns a `ParsedResponse` struct (`status`, `head`,
  `body`, `total_len`, with `complete()` / `failed()`) instead of writing through four
  out-parameters. Call sites drop from five-variable preambles to two lines. `scratch`
  stays a parameter because the caller owns the reusable buffer — that ownership is
  the allocation-avoidance above.
- **Lifetime contract, now explicit in the header:** `body` aliases the receive buffer
  (`Content-Length`) or the scratch buffer (chunked), so it is valid only until either
  is modified — in the gateway, before `rbuf` is erased or the upstream released. The
  ordering is verified by the suite under ASan/UBSan, which is what would catch a
  dangling view.

### Benchmarks

Verified against the pre-0.5.0 build on both paths, interleaved and temperature-gated:
streaming per-token p50 **108 µs vs 109 µs** and p99 **182 µs vs 183 µs** at 64
concurrent streams with 256/256 streams completing; non-streaming p99 restored as
above. No published figure changes.

`bench/BENCHMARK-CONFIG.md` gains **§3d, the A/B discipline** — interleave arms, gate
on temperature, **re-run with the arms swapped**, report min as well as median, stay
under the knee, and verify the process is alive. Every rule is there because its
absence produced a confident wrong answer during this work: thermal noise hid the
40 µs regression above; an ephemeral-port collision read as "the new build crashes";
and first-run-is-colder invented a 0.3 ms TTFT regression that vanished when the arm
order was reversed.

## [0.5.1] — 2026-07-31

Read chunked upstream responses on the non-streaming path. Found by the first live
run against the real Anthropic API, which returned `502` for every non-streaming
request while streaming worked — a gap no mock had ever exercised.

### Fixed

- **Non-streaming responses framed with `Transfer-Encoding: chunked` are now read
  correctly** (previously `502 Bad Gateway`). Real providers return non-streaming
  completions chunked over HTTP/1.1 — a server does this whenever the body length is
  unknown when headers are sent, which is the normal case for a generated completion.
  Anthropic does exactly this; it is invisible over HTTP/2 (native framing, no chunked
  encoding exists there), so a `curl` probe that negotiates h2 will not reveal it.
  The gateway's whole-body path used `http::parse()`, which rejects `Transfer-Encoding`
  outright, so the response could not be framed at all.
- `ChunkDecoder::consumed()` — a chunked message's end is found by *decoding*, not by
  arithmetic on `Content-Length`. Without it the gateway could not tell where the
  message stopped, which on a pooled connection would leave stray bytes to be
  mis-read as the head of the next response.

### Added

- `http::parse_response()` — response-side framing that accepts **both** body
  encodings, deliberately separate from `parse()` so the request path is untouched
  (see "Why the asymmetry is safe"). Idempotent like `parse()`: re-frames from the
  buffer start and returns `NeedMore` until the whole message is present.
- 18 regression tests across both backends × {1, 3, 17} chunks: translated round-trip,
  passthrough re-framing, and **three sequential requests over a pooled connection** —
  a wrong end-of-message offset only shows up on the *second* request, so a single
  request would not have caught it. The test mock can now reply chunked; every mock in
  the suite previously replied with `Content-Length`, which is precisely why this
  survived 767 tests.

### Changed

- A chunked response on the **passthrough** path (`--translate none`) is re-framed to
  the client with `Content-Length` rather than relayed verbatim. We have already
  decoded it, and forwarding framing we did not re-verify would push the problem
  downstream.

### Why the asymmetry is safe

The gateway now **sends** `Content-Length` and **accepts** either encoding in
responses. That is deliberate, and it is the standard posture for a proxy:

| leg | framing | rationale |
|---|---|---|
| client → gateway (request) | `Content-Length` only; `Transfer-Encoding` **rejected** | we are the server, bytes are attacker-controlled, and a TE/CL desync against a TE-honouring upstream is the classic smuggling attack — worse here, since a desync on a **pooled** upstream lets one client's trailing bytes become the head of another client's request |
| gateway → upstream (request) | `Content-Length`, built by us | nothing to desync: we choose the framing |
| upstream → gateway (response) | `Content-Length` **or** chunked | we are the client of a configured, TLS-verified provider; chunked is ordinary HTTP/1.1 and refusing it means refusing real providers |
| gateway → client (response) | `Content-Length` (non-streaming) | the client never sees framing we did not produce |

**Content cannot break the framing.** Both encodings are *length-prefixed*, not
delimiter-scanned: each chunk declares its size and the decoder copies exactly that
many bytes regardless of what they contain. Model-generated text inside the body —
including text engineered by prompt injection to look like `0\r\n\r\n` — is just
bytes inside a chunk whose length the provider already declared. There is no
delimiter for content to forge. (This is also why the request path can safely keep
its stricter rule: the danger there was never content, it was two parties disagreeing
about which *header* defines the length.)

Defence in depth on the response path, all pre-existing and now load-bearing:

- **`Transfer-Encoding` and `Content-Length` both present → refused.** Even from a
  trusted origin that combination is a smuggling signal (a compromised or buggy
  middlebox), so it is rejected rather than resolved by preference.
- **Chunk sizes are bounded** — a size line over 64 bytes or a chunk over `kMaxBodyLen`
  fails the stream; the framer is covered by the `fuzz_http` target in CI.
- **A framing error never pools the connection.** `error_respond()` closes the upstream
  rather than releasing it, so a connection we could not frame cannot be handed to the
  next request.

### Known gaps

- The upstream is trusted to the extent that TLS verification makes it so. A genuinely
  compromised provider could desync responses deliberately; a compromised provider can
  already return arbitrary content to every one of its clients, so this changes little,
  but it is the reason the bounds and the close-on-error rule above exist.
- HTTP/2 to the upstream is still not implemented, so llmbridge always negotiates
  HTTP/1.1 and therefore always meets chunked in practice.

## [0.5.0] — 2026-07-31

Auth-header passthrough. Combined with 0.4.0's TLS, the gateway can now front a real
provider end to end:
`llmbridge --upstream https://api.anthropic.com --translate anthropic`, with the client
sending its ordinary OpenAI-style key.

### Added

- **Credentials cross the dialect boundary.** Translated requests are *rebuilt*, so
  auth must be mapped explicitly: `Authorization: Bearer K` becomes `x-api-key: K`
  (Anthropic, plus `anthropic-version` pinned to `2023-06-01` unless the client sends
  its own) or `x-goog-api-key: K` (Gemini); Cohere receives the Bearer header verbatim;
  a client already speaking the target dialect's auth is forwarded untouched. No
  credential → **no header invented** (the provider's own `401` is the right answer).
  `TranslateMode::None` is unchanged — byte-forwarding already carries every header.
  Streaming shares the forward path, so streamed requests carry auth identically.
- **`Host` header on rebuilt upstream requests** — previously absent entirely. HTTP/1.1
  requires it; the benchmark mocks tolerated the omission, real providers do not.
  Derived from the parsed upstream hostname (falls back to `ip:port`), default ports
  omitted.
- `http::find_header()` — case-insensitive, zero-copy, first-occurrence-wins lookup.
  First-wins is deliberate: a duplicated credential must resolve the same way for us
  and for the upstream.

### Security

- **Whitelist, not echo.** Only credential headers the target dialect understands cross
  a rebuilt request. Arbitrary client headers — cookies, tracing, anything — do not.
  Echoing them would be both a smuggling surface and a privacy leak to a third-party
  provider.
- **Header-injection fix, found by audit before any real key was used.** Credential
  values are now charset-validated (printable ASCII only) and a malformed one fails the
  request with `400` **without contacting the upstream at all**. This closes a real,
  measured hole: `find_header` splits on CRLF, so a **bare CR** survived inside a value
  and reached an upstream as `x-api-key: sk\rX-Smuggled: yes`. A lenient parser treating
  bare CR as a line terminator would have seen an injected header — and because upstream
  connections are **pooled and shared between clients**, that is a cross-client
  request-splitting vector, not merely a self-inflicted malformed request. Regression
  tests cover bare CR, control characters, a second credential header attempting to
  hide behind a clean first one, and oversized values.
- Trailing whitespace is trimmed from credential values (forwarding it verbatim breaks
  real provider auth).
- Credentials are read as a view over the client's request buffer and written straight
  into the upstream bytes — never stored, never logged, never placed in stats or error
  responses.
- **Pooled upstream buffers are scrubbed, not just cleared.** A pooled connection idles
  up to 30 s; `std::string::clear()` leaves the credential bytes in the allocation, so
  release now `explicit_bzero`s the request buffer first (`explicit_bzero`, not `memset`
  — writing to soon-unused memory is a dead store the optimizer may delete). ~100 bytes
  once per request, ≈0.04% of a core at 84k RPS.
- **Cross-client leak tests.** Pooled connections are shared, so "one client's key can
  never ride another client's request" is asserted rather than assumed: a key must appear
  **exactly once** across every request the upstream ever saw (three-client variant too),
  the pool is asserted to have actually been exercised, and no credential appears in any
  response — including the `400` path, the likeliest place for a "helpful" echo.
- **Plaintext warning at startup.** A non-loopback, non-TLS upstream now prints a loud
  warning that forwarded credentials travel unencrypted. Loopback is exempt (mocks,
  benchmarks, sidecar deployments).

### Known gaps

- **Scrubbing is targeted, not exhaustive.** The pooled upstream request buffer is
  scrubbed (the only place a credential outlives its request). Transient buffers — the
  client's `rbuf`, freed allocations after a move — are not: they are overwritten within
  microseconds, and scrubbing every one would put a `memset` on the hot path for no real
  gain. An attacker able to read live process memory would find the in-flight request and
  the TLS session keys regardless.
- **`--translate none` forwards every client header verbatim**, credentials included —
  that is the byte-forward contract, not a leak, but it means the passthrough mode offers
  no header whitelist.
- No gateway-side credential store — the gateway forwards the client's key and holds
  none of its own. Per-client keys, rotation and quota live in the commercial layer.
- Verified only against hermetic mocks (project policy forbids tests hitting live
  provider APIs); no request has yet been made to a real provider through this path.

## [0.4.0] — 2026-07-31

TLS to the upstream. The gateway can now front an `https://` provider endpoint —
`llmbridge --upstream https://host` — on both event-loop backends. **Opt-in at build
time** (`-DLLMBRIDGE_TLS=ON`, needs OpenSSL ≥ 3.0): the default build remains
zero-dependency end to end, and the translator library never links OpenSSL in any
configuration. See DESIGN.md § "TLS to the upstream" for the data-flow diagram.

### Added

- **TLS transport** (`net/tls.hpp`) — OpenSSL driven through **memory BIOs**, not
  `SSL_set_fd`: the event loop keeps the socket and the `SSL` object is a pure byte
  transform, which is the only shape that works on io_uring (multishot recv hands the
  loop bytes that have *already* been read — there is no read left for OpenSSL to
  perform). Same four calls on both backends.
- **Gateway TLS integration**, both backends. Design invariant: `rbuf`/`wbuf` hold
  **plaintext always** — TLS interposes strictly at the socket edge, so HTTP framing,
  the SSE pump, translation, and stale-pool retry-resend are unchanged and unaware.
  io_uring serializes one SEND at a time so the ciphertext buffer stays immutable
  while the kernel reads it; ciphertext produced meanwhile stages in the write BIO.
- **TLS sessions survive the keep-alive pool** — a pooled reuse pays no second
  handshake (asserted by test: N requests, one handshake).
- **`--upstream` now accepts `HOST:PORT`, `http://HOST[:PORT]`, `https://HOST[:PORT]`**
  (`net/upstream.hpp`), with DNS resolution via `getaddrinfo` once at startup (A
  records, resolver order kept for the future failover PR). `IP:PORT` unchanged. The
  parser is a security boundary — the host string feeds the Host header and SNI — so
  it rejects userinfo (`@` URL-confusion), non-LDH characters (header injection),
  base paths (would be silently dropped), IPv6 literals, and sloppy ports
  (`atoi`-style `"80x"`).
- **Tests**: 10 hermetic transport tests (handshake under byte-at-a-time
  fragmentation, close_notify vs truncation, garbage-is-fatal, multi-record payloads,
  untrusted-cert and wrong-hostname rejection) + 16 end-to-end gateway tests on both
  backends (round trip, pooled-reuse handshake count, SSE translation through TLS,
  16 concurrent interleaved sessions, corrupt-record-mid-stream, provider closing
  pooled conns, TCP close mid-handshake, hostname mismatch → 502 with zero requests
  reaching the unverified peer) + 16 upstream-parser tests. CI's sanitizer job now
  builds TLS-on so the path runs under ASan/UBSan every push; the gcc/clang matrix
  stays TLS-off to keep proving the default build is dependency-free.

### Security

- **Certificate verification cannot be disabled** — no insecure flag exists. Chain
  (`SSL_VERIFY_PEER`) and hostname (`SSL_set1_host`) both derive from one argument,
  so SNI and the verified name cannot disagree; a partially-initialized session is
  torn down so ignoring an init failure cannot yield an unverified handshake.
- TLS 1.2 floor; server-initiated renegotiation refused (`SSL_OP_NO_RENEGOTIATION`);
  `ERR_clear_error()` before every SSL operation (a stale thread-local error-queue
  entry misclassifies a benign `WANT_READ` as fatal); span lengths clamped to
  `INT_MAX` rather than cast.
- **A fatal TLS record error mid-stream aborts the client on both backends** — found
  by review on epoll, where corruption previously fell into the clean-EOF path and a
  corrupted stream could be finalized with a well-formed `[DONE]`.

### Known gaps

- No OCSP/CRL revocation checking (usual for non-browser TLS clients; documented).
- A provider EOF without `close_notify` is treated as a normal end of a
  close-delimited stream — providers rarely send it, and strict truncation detection
  would break real streams.
- Re-resolution on DNS TTL expiry: a long-lived gateway pins the startup A record
  (congruent with connection pooling; revisit with the failover PR).
- Auth headers are **not yet forwarded** — calling a real provider still fails auth;
  that is the next change.

## [0.3.0] — 2026-07-29

Streaming (SSE). `llmbridge` now translates a live Anthropic event stream into
OpenAI `chat.completion.chunk`s token-by-token, end to end through the gateway, on
both event-loop backends.

### Added

- **SSE translator** (`provider/sse.hpp`) — `AnthropicToOpenAiSse`, an incremental,
  stateful `feed()`/`finish()` pump: raw upstream bytes in, OpenAI chunks out. Holds
  cross-chunk state (id/model/`created`, role-emitted, finish reason) and buffers
  partial events, so a network read splitting an event mid-JSON is handled correctly.
- **Streaming response framing** (`net/http.hpp`) — `parse_response_head()` (detects
  `text/event-stream` and chunked encoding) and `ChunkDecoder`, an incremental
  chunked-transfer decoder. Applied to the **upstream response path only**; client
  request framing stays Content-Length-only, so the anti-smuggling posture is
  unchanged.
- **Gateway streaming pump** on **both backends** — epoll and io_uring. A
  `"stream": true` request through `--translate anthropic` is forwarded with the flag
  set, and the upstream SSE response is decoded, translated, and written to the client
  incrementally. Client responses are close-delimited (`Connection: close`).
- **Back-pressure** — epoll pauses upstream reads while a slow client drains; io_uring
  uses a bounded output buffer (8 MiB) because pausing a multishot recv would require
  cancel/re-arm. Different mechanisms, same guarantee: a slow client cannot make the
  gateway buffer without bound.
- **`stream_options.include_usage`** — when the client requests it, every chunk carries
  `"usage": null` and a final chunk with empty `choices` plus real token counts is
  emitted before `data: [DONE]`, matching OpenAI's contract. Counts are the provider's
  own (Anthropic's `message_start` / `message_delta`) — never estimated.
- **Upstream idle timeouts** — new `--upstream-timeout SECONDS` flag and
  `Gateway(..., upstream_idle_ns)` parameter (default **120 s**, `0` disables). A
  request with no upstream progress is aborted rather than pinning a client connection
  and two fds indefinitely. Runs on each loop's existing periodic tick.
- **Upstream provider error passthrough** — `provider::upstream_error_to_openai()` maps
  Anthropic / OpenAI-style / Gemini error bodies into the OpenAI error envelope, and the
  gateway relays the upstream's **own status code**.
- **New stats** — `upstream_timeouts` and `stream_pauses`, both surfaced in the
  gateway's stats output.
- **SSE fuzzer** (`fuzz/fuzz_sse.cpp`) plus a seed corpus, wired into CI. It asserts two
  invariants, not just "doesn't crash": output strictness (no bare control bytes) and
  fragmentation-invariance (whole-feed output == chunked-feed output).

### Changed

- **Upstream errors are no longer flattened to `502`.** A provider's `429` (rate limit),
  `529` (overloaded), `400` (context length), or `401` (auth) now reaches the client with
  its real status code, type, and message — so clients can decide whether to back off and
  retry. Previously any non-translatable response became a generic gateway failure.
- **A non-200 upstream never becomes a `200` stream.** Streaming is entered only when the
  upstream returns `200` with `text/event-stream`.
- **`created` is injectable** on the SSE translator (defaults to wall clock), making
  output deterministic for tests and letting the gateway align a stream's timestamp with
  the non-streaming path.
- Shared internals extracted to `provider/src/openai_common.hpp` — `created_now()`,
  `anthropic_finish_reason()`, `to_ll()`, and `append_sanitized()` — so the streaming and
  whole-body paths cannot drift apart.
- `stream_step()` extracted in the gateway: the chunk-decode → translate → detect-end
  transform is shared by both backends, which keep only their own idiomatic delivery.

### Fixed

- **Translator cap failures were ignored by the gateway.** `feed()`'s return value was
  unchecked, so a stream that tripped the translator's DoS caps became a zombie: the
  gateway kept reading and decoding a hostile body while producing nothing and the client
  hung. Cap failures now abort the stream.
- **Corrupt chunked framing fabricated a clean ending.** A truncated stream emitted a
  normal `finish_reason` + `[DONE]`, so a cut-off answer was indistinguishable from a
  complete one. Corruption and timeouts now close **without** `[DONE]`, which is how SSE
  signals an aborted stream.
- **O(n²) rescan in the SSE parser.** `feed()` restarted its newline search from the
  buffer start on every call, so an upstream dribbling one byte at a time burned quadratic
  CPU. The scan now resumes at the join point — linear regardless of fragmentation.
- **`message_delta` without a `stop_reason`** (Anthropic sends usage-only deltas
  mid-stream) emitted a premature finish chunk.
- Aborted streams are counted as errors rather than served requests.

### Security

- **SSE output is strict.** All passthrough spans (content, `id`, `model`, and the
  upstream error `message`/`type`) have C0 control bytes escaped as `\u00XX` on the way
  out. The hand-rolled JSON parser is deliberately lenient and accepts raw control bytes
  inside strings; relaying them verbatim would emit invalid JSON and could desynchronise a
  strict client's SSE parser.
- **Bounded buffers on every untrusted path** — the SSE translator caps its line
  (1 MiB) and event (4 MiB) buffers with a sticky failure, `ChunkDecoder` rejects absurd
  chunk sizes and size-lines, and the streaming output buffer is capped.
- **Stalled upstreams can no longer hold resources indefinitely** (see idle timeouts).

### Known gaps

Streaming covers **OpenAI ⇄ Anthropic text** only. Tool-call streaming, the reverse
direction, and Gemini / Cohere streaming are not implemented. There is no cap on the
number of concurrent streams a single client may open.

## [0.2.0] — 2026-07-27

- Structured HTTP error responses: `400` / `502` passed through to the client instead of
  a bare connection close.
- Recovery from silently-dead pooled upstream connections (reconnect and retry once).
- Hardened JSON parsing (depth-limited against recursion bombs).
- Build cleanup, optional `-march=native`, and a Clang CI fix for `json::Value`'s
  self-referential special members.

## [0.1.1] — 2026-06-14

- Benchmark chart artifacts stored alongside the website assets.

## [0.1.0] — 2026-06-11

Initial release: the C++20 translator (OpenAI ⇄ Anthropic / Gemini / Cohere,
non-streaming chat completions), the reference gateway proxy (epoll and io_uring),
the benchmark harness, and the test suite.

[Unreleased]: https://github.com/kottosai/llmbridge/compare/v0.8.1...HEAD
[0.8.1]: https://github.com/kottosai/llmbridge/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/kottosai/llmbridge/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/kottosai/llmbridge/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/kottosai/llmbridge/compare/v0.5.2...v0.6.0
[0.5.2]: https://github.com/kottosai/llmbridge/compare/v0.5.1...v0.5.2
[0.5.1]: https://github.com/kottosai/llmbridge/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/kottosai/llmbridge/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/kottosai/llmbridge/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/kottosai/llmbridge/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/kottosai/llmbridge/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/kottosai/llmbridge/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/kottosai/llmbridge/releases/tag/v0.1.0
