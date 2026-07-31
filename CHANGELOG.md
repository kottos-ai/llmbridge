# Changelog

All notable changes to `llmbridge` are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html) — with one
pre-1.0 caveat: **the API is unstable until v1.0.0, so breaking changes may land in
minor (0.x) releases.** Breaking changes are always called out explicitly below.

## [Unreleased]

Next up: tool-call streaming, **Anthropic-in mode** (clients that speak the Anthropic
API, fronting an OpenAI-compatible upstream), and Gemini / Cohere streaming.

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
  responses. Pooled upstream connections clear the request buffer on release.

### Known gaps

- **Buffers are not scrubbed.** `std::string::clear()` leaves the old bytes in the
  allocation until overwritten, so a credential can persist in freed heap. Not reachable
  through normal operation (string APIs respect `size()`); it would require a separate
  memory-disclosure bug or a core dump. Scrubbing every request buffer would put a
  `memset` on the hot path, so it is documented rather than done.
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
