# Changelog

All notable changes to `llmbridge` are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html) — with one
pre-1.0 caveat: **the API is unstable until v1.0.0, so breaking changes may land in
minor (0.x) releases.** Breaking changes are always called out explicitly below.

## [Unreleased]

Next up: tool-call streaming, **Anthropic-in mode** (clients that speak the Anthropic
API, fronting an OpenAI-compatible upstream), and Gemini / Cohere streaming.

## [0.6.0] — 2026-07-31

Per-request timing headers. A client can now see, for every response, what the gateway
cost versus what the provider cost — and, in one absolute timestamp, **when** the
request arrived. Opt-in (`--timing-headers`, default off), because adding a header is a
visible API change.

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
  | `x-llmbridge-gateway-us` | (t1−t0)+(t4−t3) | **our compute**: framing, translation, auth mapping, re-serialisation |
  | `x-llmbridge-connect-us` | t2−t1 | TCP connect + TLS handshake; ~0 on a pooled connection |
  | `x-llmbridge-upstream-us` | t3−t2 | the provider: network + inference |

  Streaming cannot report t4 — headers precede the body — so it emits
  `x-llmbridge-upstream-ttfb-us` (time to the provider's first byte) instead of a total
  it does not yet have.

  Measured live against `api.anthropic.com`: **gateway 14–52 µs** against **1.4–4.0 s**
  of provider time, with `connect-us` falling from 52,907 µs on the cold connection to
  ~35 µs once pooled — which incidentally makes connection reuse visible per request.

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

10 new, both backends: headers absent by default; all four present when enabled; **t0 is
epoch-scale** (>1.7e18 — a monotonic uptime counter would be ~1e10); **t0 strictly
increasing across five requests** (the ordering property); the JSON body byte-identical
with headers on; and streaming emitting TTFB while *not* claiming a gateway total it
cannot have.

### Known gaps

- **Passthrough (`--translate none`) emits no timing headers.** That mode forwards the
  upstream's bytes verbatim by contract; injecting headers would break the byte-exact
  guarantee. Translated and streaming responses carry them.
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

[Unreleased]: https://github.com/kottosai/llmbridge/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/kottosai/llmbridge/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/kottosai/llmbridge/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/kottosai/llmbridge/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/kottosai/llmbridge/releases/tag/v0.1.0
