# Changelog

All notable changes to `llmbridge` are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html), with one
pre-1.0 caveat: **the API is unstable until v1.0.0, so breaking changes may land in
minor (0.x) releases.** Breaking changes are always called out explicitly below.


## [0.29.0]. 2026-08-24

### Added

- **Streamed requests appear in the latency profile, and a `first-token` line
  reports the wait they actually cost.** A stream used to record nothing at all, on
  the argument that "added latency" needs one instant the response was built. That
  argument covers the response half and nothing else: request-path and the handshake
  are the same measurements whatever shape the response takes. On the workload this
  gateway is sold for, where nearly everything streams, the profile was measuring
  almost nothing: a real Claude Code session logged seven requests and one sample,
  taken from a rate-limit error.

  `added-total` and `response-path` stay empty on a stream, deliberately, so the
  headline number keeps meaning one comparable thing. `first-token` is printed
  separately and only when it has samples, because it is mostly the provider's
  prefill and not our work; folding it in would put half a second inside a number
  whose purpose is to be microseconds. Live against Anthropic: 53 us of request path
  inside a 510 ms wait.

  It is built with 100 us buckets over 26.2 s. The first attempt reused the
  handshake range and every live sample overflowed it, printing p50 = p99 = max =
  680.69 ms, which is the clamped-maximum artifact LATENCY.md already documents for
  `connect(TLS)`.

  A truncated stream still records nothing, the same rule error replies follow: a
  request that did not finish has no place in a percentile.

## [0.28.0]. 2026-08-24

An agentic client can be measured, which it could not be before. Minor: usage is now
recorded for a dialect that reported none, and one request header stops being
forwarded.

### Fixed

- **Byte-forwarded streams are measured in the Anthropic dialect too.** The usage
  scanner read `prompt_tokens` and `completion_tokens`; Anthropic says `input_tokens`
  and `output_tokens`, and the first-token stamp looked for `"content":"` where
  Anthropic sends `text_delta`.

  Three things this needed beyond adding the field names:

  - **The scan runs as the stream arrives, before the window is trimmed.** Anthropic
    states input and cache tokens in `message_start`, at the very beginning.
  - **`output_tokens` is taken from the last occurrence.** `message_start` carries a
    placeholder `1` and `message_delta` the real total, so the first match reports
    every answer as one token long.
  - **`stream_options.include_usage` no longer gates the capture.** That is an OpenAI
    option Anthropic does not have and this client never sends, so gating on it meant
    no counts at all for the dialect being measured.

- **`Accept-Encoding` is not forwarded, because a compressed body cannot be
  measured.** The work above was verified with curl, which asks for no compression.

  A provider that compresses unasked is now a warning naming the reason, not a dash
  in the tape with nobody knowing why. `ResponseHead::encoded` carries the fact;
  the framer records it and does not act on it, since where a body ends is a
  different question from what it means.

## [0.27.0]. 2026-08-24

Six defects found by an audit of the whole repository, five of them reachable from an
ordinary client request on shipped defaults and two of those cross-client. Minor:
responses this gateway used to accept are now refused, and a chunked response now
carries the status the provider sent.

### Fixed

- **A response with neither `Content-Length` nor `Transfer-Encoding` is refused**
  (`net/http.hpp`). It used to frame as complete with a zero-length body and the
  connection kept alive, which is RFC 9112 rule 8 read backwards: such a body runs
  until the server closes, and the connection is not reusable afterwards. The body
  was dropped and the unframed head relayed to a client on a pooled connection, where
  by the specification the client reads our next response as this one's body.

  `204` and `304` carry no body by definition and still frame. A streamed response is
  diverted to the pump on its head, before this check, so close-delimited SSE is
  unaffected.

- **An interim (1xx) response is refused instead of being served as the answer.**
  `100 Continue` framed as the reply reached the client as the reply itself, and the real
  response was left unread on a connection then returned to the pool. A client could
  provoke it with `Expect: 100-continue`, which byte-forward passed straight through;
  that header is now dropped on the way out, so the request succeeds instead of
  turning into a 502.

- **epoll drops a pooled upstream that receives stray bytes** (`gateway.cpp`), which
  is what the io_uring twin already did. Bytes arriving on an idle pooled connection
  were appended to its read buffer and abandoned, and `ep_acquire_upstream` does not
  clear that buffer, so the next client was served the previous exchange's bytes.
  Chained to the interim-response defect above, a test reproduced client B receiving
  client A's completion verbatim. A one-sided divergence where epoll was the wrong
  side, so the fallback backend carried it and the default did not.

- **A chunked response keeps the status the provider sent.** Byte-forward re-frames a
  chunked body with `Content-Length`, and it wrote the status line as a literal
  `HTTP/1.1 200 OK`: a provider's 429 reached the caller as a completion whose body
  happened to be a rate-limit error, with no way to back off. Length-framed responses
  were always relayed correctly, which is why it survived; only chunkedness triggered
  it. Both backends.

- **Tool-call `arguments` are parsed before they are spliced**
  (`provider/translate.cpp`). The value arrives as a JSON string and leaves as a JSON
  object, so its bytes are written into a body we construct, and they were appended
  raw. An `arguments` of `{}}]},{...}` closed our object and appended members of its
  own, producing a syntactically valid Anthropic body we did not write; a non-JSON
  value produced `"input":not json`, a malformed body we then sent upstream. Now
  parsed, required to be an object, and required to be the whole string: the parser
  stops at the end of the first value, so checking only "is it an object" accepts
  exactly the injection this refuses. `rewrite_model` already refused the same class
  three functions away.

- **A stream cut before its terminator is no longer finished for the client.** The end
  condition was `(chunked && decoder.done()) || at_eof`, so EOF overrode the framing
  even where the framing proves truncation, and a chunked SSE stream cut mid-answer
  was completed with a fabricated `finish_reason: "stop"` and a `[DONE]`. The client
  was told it received a complete answer, and the tape recorded a clean 200.
  `stream_complete` now asks the framing: chunked ends at its terminator, and only a
  close-delimited stream ends at EOF. EOF before the terminator is reported as the
  truncation it is, so the stream is torn down honestly instead of hanging.

- **A pooled upstream is always readable.** `ep_release_upstream` re-arms reads before
  pooling. A slow client pauses its upstream, and `ep_stream_flush`'s `stream_ended`
  branch returns before the resume below it, so a connection could enter the pool with
  its interest mask at 0 and never report readable again: the next client's request
  goes out and is never answered. **Stated plainly: no test reproduces the
  interleaving.** It needs the final flush to be the one that paused. This is an
  invariant at the door of the pool, not a repair for a demonstrated failure.

### Changed

- **A request carrying an image, audio or a file is refused, not quietly stripped.**

  The refusal names the part, because the point of an error is that the reader knows
  what to do: "image content is not supported", and likewise for audio, files, and
  any part type that is not `text`.
  Malformed tool `arguments` say what shape was expected. An array of text parts is
  the multi-part shape we do carry and still works.

  A malformed credential header says so too, without echoing the value: it is a
  credential, and it would land in a JSON body unescaped. What a wrong or unfunded
  key produces is unchanged and was never ours to write: that answer comes from the
  provider, with its own status and its own message, relayed.

  The message reaches the client, which took a second change: `why` went only to the
  log and the client got a canned "malformed request". `{ep,ur}_error_respond` now
  takes a separate `detail` shown only on a 4xx, **passed per call site and never
  automatically**. A reason derived from the caller's own bytes is theirs to see; a
  policy's deny reason is not.

- **`BENCHMARKS.md` says which two tables have no committed data.** The
  concurrent-stream capacity table cited `bench/results/stream-steadystate.csv`, which
  is the 4,096-stream latency run and says so on its own first line. No tracked script
  produces the capacity table's method, and nothing in the repository records its RSS,
  CPU or socket columns. The four-client 4,096 table has no pointer at all. Both now
  say so in place, and the document's "everything below is reproducible" now names the
  exception instead of being false.

- **The README's streaming numbers are re-derived from the committed CSV**: 4% not 3%
  delivered by LiteLLM at 512 streams, 55-131 us not 50-120 us per token, and median
  of 3 runs, not a single run. Stale since the v0.10.0 re-measurement updated
  `BENCHMARKS.md` and the charts and left the README behind. Every deviation had been
  in our favour.

- **The README no longer says Bedrock and Azure are unshipped.** Both ship, with SigV4
  and an upstream query respectively, since 0.23.0 and 0.24.0. `--help` contradicted
  the README on line one.

### Added

- **`scripts/check_docs_data.py`, in CI**: every in-repo path a document points at
  must exist. A benchmark claim is worth what the data under it is worth, and the
  cheapest way for that to rot is a citation that stops resolving. Header shorthand
  (`net/http.hpp` for the file the code includes) resolves by unique suffix, so the
  design docs keep reading the way the code does.

## [0.26.1]. 2026-08-22

Tests only, no shipped code changed. The ThreadSanitizer job is green again, and the
defect it caught now fails in the ordinary build.

### Fixed

- **The model-rewrite tests joined the loop thread too late.** They read the upstream's
  bytes and returned, so the policy object they had lent the gateway was destroyed
  while the loop thread was still running and had already read its model string. TSan
  reported the `delete` against that read.

- **The fixture now enforces that instead of relying on it.** `TestBackend` refuses to
  answer `last_request()` or `all_requests()` while the loop is running, but **only
  when the loop borrows an object the test owns** (a `Policy` or a `RequestSink`).

  It fires without a sanitizer, on both backends, which is the point: this bug was
  invisible outside one CI job and on one backend. Verified by reintroducing the
  defect and watching the check fail.

## [0.26.0]. 2026-08-22

A policy may name the model, not only the venue. Minor: one new field on `Decision`,
and a stock build (no policy) puts the client's bytes on the wire unchanged.

### Added

- **`Decision::model`.** The same product is named differently at every venue:
  `claude-haiku-4-5` at Anthropic, `us.anthropic.claude-haiku-4-5-20251001-v1:0` at
  Bedrock.

  It sits on the `Decision` and **not on the upstream table**, which was the first
  design and was wrong: a venue sells many models, so a per-venue model would have
  forced one venue entry per model and made the table the thing that routes.

  Lifetime is the caller's: the view must stay valid until `decide` returns and the
  request is framed, which happens in that call stack.

- **`provider::rewrite_model`**, which **splices** the new value over the old in the
  client's own bytes instead of re-serialising the request. Everything outside the 
  model string survives byte for byte.

  The rewrite happens **before** translation, so one point serves every dialect: a
  translated venue emits the model the policy chose, and a byte-forwarded one carries
  it through untouched.

  This is what 0.25.0 was a precondition for. The spliced body is a different length
  from the client's, and the `Content-Length` on the wire now describes the bytes
  actually sent. Tested as such, on both backends: the venue receives the new model,
  the rest of the body is unchanged, and the framing matches what was written.

## [0.25.0]. 2026-08-22

`Content-Length` on a byte-forwarded request is stated by the gateway, from the bytes
it actually sends. Minor: nothing observable changes for a body we do not edit, but a
duplicate length is no longer echoed to a venue.

### Changed

- **The forwarded length is derived, never copied.** `request_without` rebuilt the
  header block from the client's, which carried the client's `Content-Length` through
  unexamined. It now drops it and emits its own from the body being forwarded.

  This is a **precondition, not a fix**. Today the two numbers are equal, so the change
  is invisible.

  Unambiguous because `parse_request` refuses `Transfer-Encoding` outright, so every
  byte-forwarded request is length-framed or bodyless. A request that carried no body
  and no length still carries neither.

  One behaviour does change, and it is the case that tells derived from copied apart:
  the parser accepts an **identical** duplicate `Content-Length` and collapses it, and
  the old code forwarded both lines to the venue. Exactly one is now emitted. Two
  lengths reaching an upstream is smuggling-adjacent even when they agree.

## [0.24.0]. 2026-08-22

Azure OpenAI as a venue. Minor: a new `TranslateMode`, and `parse_upstream` now
accepts something it used to refuse.

### Added

- **`--translate azure`.** Azure serves the OpenAI dialect, so **nothing is
  translated**: the body is forwarded exactly as the client wrote it, and an option we
  do not model cannot be dropped on the way through. What differs is everything
  around it. The deployment lives in the path, the `api-version` in the query, and the
  credential goes in an `api-key` header, never `Authorization`. Streams take the
  byte-forward path, so Azure needs nothing beyond what 0.22.0 already fixed.

  A request with no credential is refused before it leaves: Azure rejects an
  unauthenticated call anyway, so spending a round trip to be told so buys nothing.

- **A query on the upstream URL**, parsed and kept:
  `https://x.openai.azure.com/openai/deployments/gpt-4o?api-version=2024-02-01`.

  This was previously refused outright, and the stated reason was that it would have
  to be merged with the client's own query. That reason **does not apply to a mode
  that builds its own request target**, which discards the client's. So the rule moved
  and did not loosen: `parse_upstream` reports what it found, and the `Gateway`
  refuses a query on a byte-forwarding venue **at startup**, where the mode is known
  and the operator is watching, instead of dropping the `api-version` and returning
  404 forever.

  The query is split off before the path is normalised, so no `?` can reach a
  request-line path, and it carries the same charset refusal as the path: it lands in
  a request line, so anything that could split or retarget that line is rejected, never
  sanitised.

### Verified

- **SigV4 signing works against real AWS.** 0.23.0 shipped it with an explicit
  "untested against AWS" caveat, because it had only ever been checked against
  published vectors and a mock. An OpenAI-shaped request through
  `--translate bedrock` to `bedrock-runtime.us-east-1.amazonaws.com` returned **HTTP
  200** with a translated Anthropic response and usage, on
  `us.anthropic.claude-haiku-4-5-20251001-v1:0`.

  That id contains a colon, so it exercised the trap the whole module was built
  around: `%3A` on the wire, `%253A` in the signed canonical URI. A mismatch there is
  a 403 with an empty body, and it was a 200. Region derivation from the hostname and
  the `bedrock` service pin were exercised at the same time.

  A bare model id returns 400 asking for an inference profile. That is Bedrock policy
  about on-demand throughput, not a signing failure, and the gateway relayed it with
  the upstream's own status and message.

- **The Anthropic-compatible Bedrock surfaces are region-scoped and credential-fussy.**
  A long-term Bedrock API key authenticates on `bedrock-mantle.{its region}.api.aws`
  and is rejected by `bedrock-runtime/anthropic`, which wants the short-lived token
  `aws_bedrock_token_generator` mints. Mantle also takes the short model id
  (`anthropic.claude-haiku-4-5`) and 404s on an inference-profile id. So reaching
  Bedrock over plain SSE depends on having a key and an entitlement in the same
  region; SigV4 has neither constraint but cannot stream without an AWS event-stream
  decoder.

### Changed

- **A fragment is refused with its own message.** It never travels on the wire, so a
  URL carrying one is a paste error worth naming; it used to be reported as "query or
  fragment", which named the wrong half for the Azure case.


## [0.23.0]. 2026-08-21

AWS SigV4 request signing, and Bedrock as a venue for non-streamed requests. Minor:
a new `TranslateMode`, a new credential shape, and a new module.

**Untested against AWS.** The signing is verified against AWS's published test
vectors and the gateway wiring against a mock, so the arithmetic and the plumbing are
proven; whether Bedrock accepts the result is not. No request from this code has ever
reached the service. Treat a 403 from a first real attempt as expected work, not as a
surprise, and start the bisect at the canonical request, not at the signature.

### Added

- **`net::sigv4`**, enough of SigV4 to reach Bedrock: canonical request, canonical
  query, the `kDate`/`kRegion`/`kService`/`kSigning` chain, and the `Authorization`
  header. Session tokens are supported, because temporary credentials are what most
  real deployments use and an implementation that omits them works in a demo and
  fails at a customer. Each link of the signing chain is scrubbed as soon as the next
  is derived: the intermediates are as good as the secret to anything reading that
  memory.

  Built against **AWS's own published values**, reproduced independently before being
  written into the tests, so a disagreement means this code is wrong and not that a
  constant was mistyped. That discipline paid immediately: "URI-encode a non-S3 path
  segment twice" counts from the raw path, and the request line already holds the
  first pass, so encoding the wire form twice turns a model id's `%3A` into `%25253A`
  and fails exactly like not encoding it at all.

  TLS builds only: it needs SHA-256 and HMAC-SHA256 from the OpenSSL a TLS build
  already links, so the dependency-free build is untouched and simply cannot reach a
  venue that requires signing.

- **`provider::openai_to_bedrock_request`**, the Messages body as Bedrock takes it:
  no `model` field, because the model id belongs in the path, and `anthropic_version`
  inside the JSON where Anthropic wants a header. It shares one message walk with
  `openai_to_anthropic_request`, since a second copy would be a second place for tool
  results, vision and system-prompt joining to drift; a test asserts the two bodies
  are byte-identical after their respective first fields.

- **`TranslateMode::Bedrock`**, and `--translate bedrock`. The model moves from the
  body into `/model/{id}/invoke`, percent-encoded as one segment, and the response leg
  reuses the Anthropic translator because Bedrock answers in the same envelope.

- **Per-request AWS credentials**, `Authorization: Bearer AKID:SECRET[:TOKEN]`.
  Signing has to happen here: the gateway rewrites the target and the body, so any
  signature a caller pre-computed is void by the time the bytes exist. Passthrough is
  what keeps that secret out of anything the request path could read, and it reuses
  the header the BYOK path already carries, so nothing upstream needs a new mechanism.
  Colon is unambiguous because AWS's alphabets exclude it. A value that does not split
  into two or three non-empty parts is refused, never signed partially, since AWS
  answers a partial credential with the same opaque 403 as a wrong signature.

### Security

- **The region is derived from the endpoint name and checked by shape**, not against a
  list of regions that would go stale. An endpoint carrying no region refuses every
  request; it never signs with a guess. The signing service is pinned to `bedrock`
  and deliberately does not follow the `bedrock-runtime` hostname.
- **A build without OpenSSL refuses Bedrock** instead of sending the customer's secret
  unsigned on a call AWS would reject anyway.

### Not yet

Streaming. `invoke-with-response-stream` returns AWS event-stream binary frames, which
is a second framing decoder and its own release. A streamed request to a Bedrock venue
today builds a request to `/invoke`.


## [0.22.0]. 2026-08-21

Byte-forwarded responses stream. Minor because the default deployment shape behaves
differently: an OpenAI-compatible venue's SSE response now reaches the client as the
provider produces it, and carries token counts it did not before.

### Fixed

- **A streamed response from a venue needing no translation was buffered whole.** The
  streaming pump was entered for the Anthropic path only, so `--translate none` framed
  a `text/event-stream` response as a whole body and delivered it once complete.

  A stream with no translator now forwards its bytes untouched and lets the provider's
  own `[DONE]` end it. Both backends, through the shared `stream_step`, so the fix
  cannot land on one loop.

  **Gemini and Cohere stay on the whole-body path deliberately.** They have no SSE
  translator, so forwarding their events would hand an OpenAI client a dialect it
  cannot read. The gate names the two modes that may stream; it does not exclude the
  two that may not, so a dialect added later is opted in by someone who thought about
  it.

### Added

- **Token counts on a byte-forwarded stream**, including cache-read tokens. Nothing
  parses those events, so the counts come from a bounded tail of the stream scanned
  for the final usage chunk, kept only when the client sent
  `stream_options.include_usage`. Without it a streamed passthrough request reported
  no tokens at all, which is most of a voice or agent workload. All three stay -1 when
  no usage chunk arrives: "not reported", never zero.
- **`Connection::wants_usage` is set on the byte-forward request path too.** It was
  set only where a translator existed, which was harmless while that path could not
  stream.

### Changed

- **`Connection::sse` is now `sse_xlate`.** SSE is a transport both dialects speak, so
  "the SSE object" was the wrong name for an Anthropic-to-OpenAI translator, and it
  became actively misleading once byte-forwarded streams existed: a null `sse` reads
  as "not SSE" for a stream that is every bit as much SSE and simply needs no
  translating. Null now reads as what it means, "forward the provider's events
  untouched".
- **One window where there were two.** The bytes retained from a byte-forwarded
  stream and the bytes searched for a usage block were separate constants, 1024 and
  512, which had to relate and did not: half the retained buffer was dead and the
  relationship was invisible. Both are now `kUsageWindow`, sized by what must fit and
  pinned by a test using a full-size provider usage chunk. This did not correct a
  miscount: measured against a realistic OpenAI usage chunk, the counts sit 267 bytes
  from the end and the old window reached them.

### Note

`Connection::sse_xlate` is a single `AnthropicToOpenAiSse`, not a category. A third dialect
that learns to stream owns its own token accounting; `stream_tokens()` says so at the
one place that decision lands, because falling through to the tail scan would search a
non-OpenAI stream for an OpenAI usage block and quietly report nothing.


## [0.21.0]. 2026-08-21

Time to first token, distinct from time to first byte. Minor because
`RequestRecord` and the SSE translator each gain a field.

### Added

- **`RequestRecord::ts_first_token`**, stamped when a streamed response emits its
  first content token, as opposed to `ts_up_recvd` (t4), which is the response head.
  Their difference is the provider's prefill. It sits outside the t0-t6 scheme by
  design (a non-streamed request has no first token), and it is stamped once in the
  shared `stream_step`, so both the epoll and io_uring backends carry it. 0 on a
  non-streamed request, or a stream that produced no content.
- **`AnthropicToOpenAiSse::content_started()`**, a one-way latch that turns true when
  the first text delta or tool call is emitted, not the role-only opening delta. This
  is the signal the gateway stamps its clock against. It is the same first-token event
  `streamgen` measures client-side, now available per request to an in-process sink.
  It rides no response header: a stream's headers are written before the first token
  exists (see LATENCY.md).


## [0.20.0]. 2026-08-20

Prompt-cache token reporting, minor because the translated usage now carries a field
it did not before.

### Added

- **Cache-read tokens surface in the OpenAI usage.** When an Anthropic response
  reports `usage.cache_read_input_tokens` (the prompt tokens served from cache, at a
  discount), the translator now emits `usage.prompt_tokens_details.cached_tokens` in
  the OpenAI shape, on both the non-streaming response and the streaming usage chunk.
  It is emitted only when the provider reported a non-zero value, so an uncached
  request produces byte-for-byte the usage object it always did. An OpenAI-compatible
  upstream that already sends `prompt_tokens_details.cached_tokens` passes through
  unchanged.
- `RequestRecord::cached_tokens` and `AnthropicToOpenAiSse::cached_tokens()`, so an
  embedder can record cache reads per request. On a gateway header,
  not the installed provider API.

### Fixed

- **The runtime log level now defaults to the compile floor**, so a binary built with
  the debug floor (`-DLLMBRIDGE_LOG_LEVEL=debug`) emits debug without an explicit
  `set_level`. Before, the runtime default was hardcoded to Info regardless of the
  compile floor.

Diagnostics only, patch for the same reason as 0.19.2: the installed `provider` API
is unchanged; the new accessors sit on `net::BufRing`, which llmbridge does not
install.

### Added

- **The io_uring provided-buffer ring fallback now reports why.** `BufRing::init`
  records the failing step (`mmap-ring`, `mmap-bufs`, or `register-pbuf-ring`) and
  the errno, and the gateway prints both when it drops to epoll.
- `BufRing::init_stage()` and `BufRing::init_errno()`, the accessors the log line
  reads. On an uninstalled header, so no effect on the shipped API.


## [0.19.2]. 2026-08-19

Diagnostics only, again, and patch for the same reasons as 0.19.1: nothing new is
reachable and the installed library's API is unchanged.

### Changed

- **A failed inbound TLS handshake logs at DEBUG instead of WARN.** On a public
  listener that is overwhelmingly internet scanners speaking junk at 443, and one
  WARN each buries every line worth reading. An upstream handshake failure, and a
  mid-session failure on either leg, stay at WARN: those are actionable.

### Added

- `Stats::client_tls_handshake_failures`, because silencing a line without counting
  what it stood for is how a customer who cannot handshake produces no evidence at
  all on a production build. The rate is the diagnostic: a steady climb is
  background noise, a step change the moment someone tries to connect is their TLS
  problem. The counter is the inbound leg only, so scanner noise and a broken
  provider stay distinguishable, and a test asserts each half.


## [0.19.1]. 2026-08-18

Diagnostics only. Nothing new is reachable that was not reachable at 0.19.0: the
same inputs are refused, with the same exit code and the same text on stderr. Patch
for that reason, and because the installed library is untouched. `app/main.cpp` is
the binary and ships no header.

### Changed

- Every startup refusal in `llmbridge`'s own `main` goes to stderr and the log now,
  through one `refuse()` helper, so a message cannot drift between the two. stderr
  is for an operator running the binary by hand; the log line is what a journal can
  filter by level and timestamp. **Operators scraping logs will see ERROR lines that
  did not appear before.** They are additional: no existing output changed.
- `refuse()` takes a `std::source_location` defaulting to the call site. An
  `LB_ERROR` inside it stamped every refusal in the binary with the helper's own
  line number, which is worse than none: it looks like a location and is not one.
  Verified by running the binary, since a file-local helper in the translation unit
  holding `main` cannot be linked into a test.
- A multi-address upstream is an `LB_WARN` instead of a bare `fprintf`. It does not
  stop startup, so it reads as the warning it is, beside the plaintext-credential
  warning a few lines below it.
- A base path carrying a query or fragment says so, instead of reporting the
  generic "character outside [A-Za-z0-9-._~:/]". Pasting an Azure OpenAI URL is how
  an operator reaches that rejection, and the generic message named neither the
  character nor the reason.

Parsers are deliberately unchanged: they report through an out-parameter and log
nothing. The caller is what knows whether a failure is a startup refusal or a
rejected config edit, and `kottos-broker --check` needs a parser that can answer a
question without writing to a journal.


## [0.19.0]. 2026-08-18

**A venue may now carry a base path** (`--upstream https://api.groq.com/openai`).
It is a prefix, joined in front of whatever target the request would otherwise use,
so `/openai` + `/v1/chat/completions` reaches `/openai/v1/chat/completions`. It
exists because several providers serve an OpenAI-compatible API below the root and
were unreachable without it: Groq at `/openai` and OpenRouter at `/api` both answer
on their prefixed path and 404 at the root.

Minor. Nothing changes for a venue without a base path, and a test asserts the
target stays byte-identical there.

### Added

- `net::UpstreamSpec::path` and `Upstream::base_path`: a normalized base path,
  empty or `/...` with no trailing slash. `parse_upstream` accepts one only in the
  `http(s)://` form, since `host:9001/x` reads as a path here and as something else
  in half the world's URL parsers.
- The prefix applies on both legs: the client's own target when forwarding bytes,
  and ours (`/v1/messages`, `/v2/chat`, Gemini's `generateContent`) when
  translating. Tested end to end on epoll and io_uring.

### Security

- The base path is spliced into a request line, so it is validated as strictly as a
  host: a whitelist of `[A-Za-z0-9-._~:/]`, and a refusal instead of a strip for
  anything else. Control bytes, spaces, `?`, `#`, `%`, `//`, `/./` and `/../` are all
  rejected at parse time. Percent-encoding is refused precisely because `%2e%2e` and
  `%2f` are how a dot segment or a separator gets past a check that only looks at
  literal characters.
- A request whose own target is not origin-form is **refused with 400 and never
  forwarded**, on a base-path venue. Prefixing an absolute-form target would mean
  deciding what it addressed, and deciding wrongly sends a caller's credential to a
  host the operator never listed. The test asserts nothing reached the upstream, not
  merely that the bytes differed: the shared pool means the next request on that
  connection belongs to someone else.
- Query and fragment stay rejected in every position. Azure OpenAI needs
  `?api-version=`, which means merging our query with the client's; that is a
  separate design question and is not guessed at here.

### Fixed

- A data race in the test harness, found by running TSan over the whole suite:
  `PinnedPolicy::set` wrote a plain `int` from the main thread while the io_uring
  loop read it in `decide()`. Test-only, and it never produced a wrong result, but
  it made TSan red on three route tests. The field is atomic now. TSan is not part
  of `precommit.sh`, which is why it had gone unnoticed since the routing tests
  landed.

### Known gaps

- No query-string support, so Azure OpenAI is still out of reach.
- AWS Bedrock and Google Vertex need this and request signing (SigV4, OAuth2), so
  the base path alone does not reach them.


## [0.18.0]. 2026-08-17

**Byte-forward now rewrites `Host` to name the venue.** It is a reverse proxy on that
path: the request goes to a different origin than the client addressed, and HTTP/1.1
wants `Host` to name the origin being addressed. The translating path has always
emitted the venue's `Host`; byte-forward passed the client's straight through, so a
provider behind a CDN or serving several vhosts saw a name that was never its own.

Minor, and **this changes bytes on the wire** for `--translate none`: a deployment
that relied on the client's `Host` reaching the upstream will see the venue's
instead. That is the fix, not a side effect, but it is called out because pre-1.0
minor releases may change behaviour and this one does.

### Changed

- Byte-forward rebuilds the request with the venue's `Host` (`Upstream::host_hdr`,
  the same value the translating path uses), directly after the request line.
  However many `Host` headers arrive, exactly one goes upstream.
- The empty-strip-list fast path is gone: byte-forward always rebuilds now, because
  `Host` has to be replaced whether or not anything is stripped. The rebuild is the
  same run-copy that stripping already used, measured at +86 to +99 ns and flat in
  body size.
- Three tests asserting byte-identical passthrough now assert the body is intact and
  the Host is the venue's. `EmptyListLeavesTheRequestUntouched` is renamed
  `EmptyListChangesOnlyTheHost`, which is what it now proves.

## [0.17.1]. 2026-08-17

**Undefined behaviour on the sink's most common path.** `sink_capture` copied a
configured-but-ABSENT header with `memcpy(dst, nullptr, 0)`: `find_header` returns a
null view when the header is missing, and memcpy's arguments are declared non-null
even for a zero length. Every request that omitted a captured header did it.

Patch: a fix and the test that was missing, no API change.

### Fixed

- Guard the copy on a non-zero length.  New test
  (`ACaptureConfiguredButAbsentIsEmptyAndNotUndefined`, both backends) that fails
  under `-fsanitize=undefined` against the unguarded version, which the sanitizer
  CI job runs.

## [0.17.0]. 2026-08-16

**An optional per-request metadata sink.** `RequestSink` (gateway/sink.hpp) receives
one `RequestRecord` per completed request on the loop thread: the monotonic stamps,
a wall clock taken at framing, status, serving venue, attempts, the policy tag,
token counts where the provider reported them, and up to two request-header values
the integrator asked to capture. Streams and gateway-generated error replies emit
too, because a record of successes only cannot answer "why was this slow".

Minor: additive API. A build that never calls `set_request_sink` pays one
predicted-false branch per request and nothing else.

### Added

- `RequestSink`, `RequestRecord`, `Gateway::set_request_sink(sink, capture)`. Header
  values are copied at framing, bounded to `kSinkCaptureBytes`, because the request
  buffer is reused long before completion; the record's views die with the call.
- Emitted at every completion on both backends: non-streaming replies, finished and
  truncated streams including SSE translator's token counts, and error replies the
  gateway generated itself (stamps unset, `error_reply` set).
- Tests on both backends: capture round-trip, per-request capture on keep-alive,
  bounded over-long values, failover attribution (the record names the venue that
  Served), stream token counts, and error replies. Mutation-checked: removing the
  framing-time capture or the stream emit fails the suite.

## [0.16.0]. 2026-08-16

**An tag from the decision to the failure.** `Decision::tag` is stored with the
request and handed back verbatim as `FailureFacts::tag`.

Minor: additive API, no behaviour change for existing policies (both fields default
to 0, and a policy that never sets the tag reads 0 at the failure).

### Added

- `Decision::tag` and `FailureFacts::tag` (`uint64_t`, default 0).
- The tag is per request: reset where the failover budget is, overwritten by every
  `decide()`, and stable across a multi-attempt failover chain. Tests cover the round
  trip on both backends, plaintext and TLS, keep-alive replacement between requests,
  chain stability, and the zero default.

## [0.15.1]. 2026-08-16

**One client that never reads could make the gateway hold 228 MB.** It looked like a
flaky test: `ClientThatNeverReadsCannotGrowUsWithoutBound` failed occasionally under
parallel CI load and passed on rerun. It was not flaky. It was an unbounded-growth bug
that the test caught only when the machine was loaded enough to expose it.

Patch: a fix and a measurement correction, no API change.

### Fixed

- **`ep_drain_read` had no byte budget.** It looped until EAGAIN, so against a provider
  that writes as fast as we read, one readable event pulled as much as the loop could
  keep up with. That plaintext became `wbuf`, then ciphertext, then staged memory.
  Bounded to `kEpMaxReadPerEvent` (1 MiB) per event; epoll is level-triggered, so the
  remainder re-notifies and back-pressure engages between events, which is what the
  epoll path is supposed to do.
  The obvious symmetry fix, giving epoll the 8 MiB cap io_uring already had, was tried
  and **rejected on measurement**: it changed nothing. io_uring's cap works because a
  completion hands back at most one 4 KB buffer, so the cap is re-checked every 4 KB.
  An epoll pump checks once per event, after the drain has already happened, so a cap
  bounds nothing that the pull did not already stage. The budget is not a second
  mechanism next to the cap; it is epoll's equivalent of that 4 KB buffer.
- **`Stats::tls_buffered_peak` counted the wrong thing.** It measured `tls_out.size()`,
  which includes the prefix already handed to the kernel, so it grew with total
  throughput, not with backlog. Now the unsent backlog: unwritten ciphertext plus
  the write BIO. This is why the symptom read as an erratic metric.

### Measured

Four builds, 5 runs each, under 12 busy cores, watching process RSS because the counter
itself was one of the suspects:

| epoll build | peak process RSS over 5 runs |
|---|---|
| neither mechanism | 51, 50, **213**, 50, 49 MB |
| 8 MiB cap only | **204**, **115**, **222**, **222**, 48 MB |
| 1 MiB read budget only | 48, 57, 55, 49, 48 MB |
| both | 49, 56, 57, 57, 57 MB |

The cap alone is no better than nothing, and adds nothing on top of the budget: ten
further runs of budget-only against both put every reported peak at or below 1036 KB in
each arm. So the shipped fix is the budget, and epoll carries no stream cap.

The io_uring cap was then put through the same check, and it stays: it is that backend's
only bound. Its multishot recv is armed for the upstream's life and there is no uring
pause path, so a stalled client throttles nothing. It fires once in this test (epoll's
fired zero times), and removing it stages the entire 32 MiB flood: peak RSS 67 MB with
it and 99 MB without, 5 runs each, no overlap.

The test's bound is tightened from 12 MiB to 4 MiB, and it now also asserts the peak is
non-zero so a broken flood cannot make the bound vacuous. It remains a smoke test: it
catches the unbounded version 2 runs in 6 under load, 1 in 6 idle, because the bug needs
the loop to fall behind. The RSS A/B is the evidence, and the test says so.

## [0.15.0]. 2026-08-15

**Several upstreams, chosen per request.** The gateway held one upstream, resolved at
startup, so the policy seam could decide only whether a request proceeded and never
where it went. It now holds an ordered table and `Decision::upstream_index` selects
from it. llmbridge still chooses nothing itself: picking a venue needs measurements it
does not collect, so a stock build always uses the first entry.

Minor: new functionality, and a behaviour change in the config file, called out below.
The C++ API is additive: the single-upstream constructor still exists and delegates.

### Added

- **`Gateway::Upstream`** and a constructor taking `std::vector<Upstream>`. Each venue
  carries its own address, TLS setting, SNI host and dialect, which is what makes
  cross-venue routing possible at all: one request is rebuilt for Anthropic while the
  next is forwarded to an OpenAI-compatible host, decided per request.
- **`Decision::upstream_index`**, promised in 0.14.0's "known gaps" and deliberately
  withheld until the table existed, because a field the gateway ignored would have been
  a lie. Unset (-1) or out of range means the first upstream, so a policy that only
  authenticates never learns the table exists.
- **`upstream` may be an array** in the config file. The object form is unchanged and
  means exactly one entry, which is the additive migration DESIGN.md promised when
  `--config` shipped ahead of the table.

### Changed

- **One keep-alive pool per venue.** A connection to one provider is never handed to a
  request bound for another; that would put the request, and its credential, on a
  socket to the wrong company. Retries after a stale pooled connection stay on the same
  venue for the same reason.
- **Pool fragmentation is now N workers times M venues.** Each pool sees ~1/(N*M) of
  the traffic and crosses the idle line far more often, and every crossing costs the
  next request a reconnect and a TLS handshake. `--pool-idle` defaults to 30 s, chosen
  when there was one pool; measure before trusting it with a table.
- **Startup logs the table**, one line per venue with its dialect, replacing the single
  `upstream=` line.

- **`Policy::on_failure(FailureFacts) -> Retry`**, so a caller can react when a venue
  does not answer: a refused connect, a failed write, an EOF before the response, an
  idle timeout. Twelve sites across both backends. **The default never retries**, so a
  stock build, and any policy that ignores failures, answers 502 exactly as before.
- **A failover rebuilds the request** for the new venue, never resends it: the bytes
  queued for the venue that failed were translated for its API, so resending them would
  put an Anthropic Messages body on an OpenAI-compatible host. The client's original
  request is kept only when a policy is installed and the table has more than one entry,
  so a stock or single-upstream build copies nothing.
- **`Stats::upstream_failovers`**, apart from `upstream_retries`: one says a provider
  dropped an idle keep-alive, the other says a provider is not answering.

### Not included, and deliberately

- **No failover policy.** llmbridge forms no opinion about which venue is healthy,
  because health is measured and it measures nothing. Ordering, ejection thresholds and
  cooldown belong to the caller.
- **A venue that did answer, with something unparseable, does not reach the hook.**
  Retrying elsewhere would mask a real incompatibility as a transient blip.
- **Streaming fails over only before its first byte reaches the client.** After that the
  stream is truncated honestly: no LLM API can resume mid-stream, so a reconnect would
  replay tokens the client already has. Three further bounds, none of them policy: three
  venues per request, the failed venue may not be renamed, and nothing is re-sent once
  any byte has gone out.

### Also

- **A GCC + TLS build leg**, in CI and in the pre-commit script. `gateway_tls_test.cpp`
  is compiled only when TLS is on, and every TLS build here used clang, so that file
  had no GCC coverage at all. Three errors were sitting in it: two dangling elses (an
  `ASSERT_*` expands to an if/else, so an unbraced body under `if` is ambiguous) and an
  ignored `warn_unused_result` write. Clang accepts all three; GCC refuses them. Fixed,
  and the leg is what stops them coming back.

### Verified

Eight routing tests on both backends, and five mutations of the routing logic all
caught: the policy's index ignored, pools shared across venues, release into the wrong
pool, an out-of-range index left unclamped, and a retry rerouted to another venue.

That last one survived three attempts. The first two tests reached the retry path but
could not observe where the retried connection landed, because the mock closed every
connection and the contaminated one was evicted before anything could draw it. Catching
it needed a mock that kills only its first connection, so the retried one survives to
be pooled, and a following request to the other venue to expose it.

## [0.14.2]. 2026-08-14

**The JSON number scanner accepted things that are not numbers.** It consumed any run
of `[0-9+-.eE]` and called the result a `Number`, so `1e`, `--1`, `1.2.3`, `+1`, `.5`
and `00` all parsed. Found while chasing an uncovered branch in a consumer's config
parser: the branch existed because the parser needed it to.

Patch, and the reason is worth recording because it is arguable. Input earlier
versions accepted is now refused, which is the shape that made **0.9.0 a minor**. The
difference claimed here is that none of it was ever valid JSON, so no correct client
is affected. If that reasoning does not hold, this is 0.15.0.

### Fixed

- **`parse_number` follows RFC 8259 §6**: `-? int frac? exp?`, with a leading zero
  standing alone, at least one digit after `.`, and at least one after `e`/`E`.
- **What this was not.** No injection was possible: the old character set is closed, so
  a number span could never carry a quote, comma or brace, and re-emitting one could
  not break out of the JSON around it. What it did was push validation onto every
  consumer, since `strtod("1e")` fails and a consumer that forgets the check gets 0.
  llmbridge's own config carried that guard; a second consumer's did too, which is how
  this surfaced.
- **User-visible effect**: a request body containing a malformed number is now refused
  with a 400 naming a bad request, where before it was forwarded for the provider to
  reject. The gateway's error is the more useful of the two.

### Verified

31 shapes checked against the RFC grammar, five mutations of the new scanner all
caught, and 90 seconds of libFuzzer on the JSON parser with ASan and UBSan, clean.

## [0.14.1]. 2026-08-14

**Passthrough echoed every client header to the provider.** DESIGN.md and CLAUDE.md
have said "rebuild, do not echo" since the 0.8.1 security sweep, and the translating
path does exactly that. `TranslateMode::None` never did: it copied the client's bytes
verbatim, headers included, so an internal routing header, or a token meant for this
gateway alone, reached a third party unchanged.

Patch: a defect fix. It ships one new config key because that is the only way to name
the headers, and the default (none) leaves every existing deployment byte-identical.

### Fixed

- **`upstream.strip_headers`**, header names removed from every upstream request,
  matched case-insensitively and exactly, so `x-drop` does not also eat `x-dropper`.
  Applied in two places, and the second is the one that matters: the passthrough copy,
  and the credential scan, so a stripped `Authorization` is never promoted onto the
  provider key.

### Cost

Measured with a microbenchmark of the copy itself, seven interleaved rounds, min of
each, on the reference laptop and not a cold host. Treat it as an order of
magnitude, not a published figure:

| body | plain copy | with one header stripped |
|---|---|---|
| 1 KB | 42 ns | 128 ns |
| 4 KB | 70 ns | 159 ns |
| 16 KB | 242 ns | 341 ns |

**About +90 ns per request, flat in body size**, because the extra work is a scan of
the header block and not of the body. Against the 41-80 us added p99 this repo claims,
that is roughly 0.2% of the budget, and only for deployments that set the key. An empty
list keeps the single memcpy and the whole path unchanged.

Kept lines are copied in runs, not line by line, which is worth ~45 ns of that:
one memcpy when nothing matches, two when one header does. Translating modes pay
nothing measurable, since they already rebuild the request and only gain a few string
compares in the credential scan.

## [0.14.0]. 2026-08-13

**The policy seam.** One hook, called once per framed request, answering the question
llmbridge does not answer for itself: may this request proceed? The gateway still
authenticates nobody and meters nobody.


### Added

- **`gateway/policy.hpp`**: `Policy`, `RequestFacts`, `Decision`. Supplied at `Gateway`
  construction, non-owning, `const` member so there is no setter to race. Called at one
  site per backend, after framing and before translation, credential mapping or
  upstream acquisition, so a refusal reaches no provider.
- **Two opposite meanings of "default".** No policy installed, which is every stock
  build, means no call is made and the request is forwarded: absent, not permissive. A
  policy that returns a value-initialised `Decision` gets a refusal, because that shape
  is what a forgotten branch produces.
- **`Stats::policy_denied`**, a subset of `errors` and not a sibling of it.

### Fixed

- **`build_error()` rendered every status except 400 and 504 as `502 Bad Gateway`.**
  Harmless while those were the only codes in the tree, wrong once a policy could pick
  one: a 401 would have gone out as "bad gateway: upstream failure", blaming the
  provider for our own refusal. Now a table (400, 401, 403, 404, 413, 429, 503, 504)
  with the 502 fallback kept. A deny status outside 400-599 still refuses, with 403
  substituted and a WARN.
- **`net::http::find_header` matches header names exactly.** It wanted a trailing
  colon, compared the name as a bare prefix and never lower-cased it, so
  `"Authorization"` matched nothing (an auth check reading every request as
  unauthenticated) and `"x-tenant"` returned a client-sent `x-tenant-spoof:` value. No
  production caller existed, so nothing was exploitable; the policy seam would have been
  the first. Names are now written without a colon, with the colon spelling still
  accepted.

### Known gaps

- `Decision` selects no upstream, and `RequestFacts` exposes no model name. Both arrive
  with the upstream table; a field the gateway ignores would be a lie.

## [0.13.0]. 2026-08-13

**Logging.** The gateway said almost nothing about itself: five `fprintf` lines at
startup, and silence thereafter. Twenty-eight error sites collapsed into three status
codes with no cause, every TLS handshake failure looked identical to a closed socket,
and every configured limit was reached without a word. This release makes a running
process describe itself.

Minor: new functionality. `net/log.hpp` is internal (only `provider/include/provider`
is installed), so the public C++ API is unchanged.

### Added

- **A logger** (`--log-level`, `runtime.log_level`, `-DLLMBRIDGE_LOG_LEVEL`; stderr,
  default `info`), sized so it cannot cost the latency claim. Twenty events per request
  at ~84k RPS is 1.7M records/second against a budget of 41-80 us added p99 for the
  whole request, and one `fprintf` costs 1-5 us under a lock, so a conventional logger
  would invalidate the benchmark. Hence a
  **compile-time floor** that removes lower call sites entirely, so their arguments are
  never evaluated; a runtime level behind one relaxed atomic load; a **fixed stack
  buffer**, no allocation and no iostreams; and one `write(2)` per line, no mutex, so
  concurrent workers interleave whole lines.

- **Three subjects lead every message**, because a line you cannot attribute is a line
  that costs a reader time: `ClientConnection#42(fd=17,cid=3)`,
  `UpstreamConnection#8(fd=7)`, `Request#123`. The instance number is process-unique
  and never reused, unlike the fd, which the kernel recycles the moment a connection
  closes. Objects get a print method through a free `log_put` found by ADL, which is
  how a class becomes loggable without dragging iostreams onto the event loop. Every
  line also names its thread (`worker/2`), not a fifteen-digit `pthread_self`.

- **Per-request tracing at DEBUG**: the request line, which upstream served it and
  whether that upstream came from the pool, and the response status with byte count and
  keep-alive. Connection accept and close carry the live client count; upstream
  acquisition carries the pool depth.

- **A `Sink` seam.** The built-in sink writes to stderr.

- **Error causes.** `*_error_respond` now takes a reason, at all 28 sites: `request
  framing`, `request translate`, `malformed credential`, `no upstream (connect failed)`,
  `upstream connect refused`, `upstream write failed, retry exhausted`, `upstream EOF,
  retry exhausted`, `upstream response head framing`, `upstream response framing`,
  `response translate`, `upstream idle timeout`. The level is derived from the code and
  never chosen per site: 4xx is the client's fault and is DEBUG, 5xx is ours or the
  provider's and is WARN. Backwards, and you either drown in 4xx noise at scale or miss
  an outage.

- **TLS handshake failures.** `Session::last_error()` was consumed in zero places in
  shipped code, so a failure produced a closed socket and no diagnostic. The three most
  likely first-deployment failures are now distinguishable at a glance: plaintext sent
  to a TLS listener (`http request`), a client that does not trust the certificate
  (`tlsv1 alert unknown ca`), and success. The strings are OpenSSL reason codes and
  carry no key material.

- **Every parameterised boundary says when it is hit**, tagged `CAP` or `TIMEOUT` so
  one grep finds them all, and each carries the measured value against the limit so
  "barely over" is distinguishable from "wildly over": the upstream pool cap, the
  io_uring 8 MiB stream cap, epoll back-pressure (with its matching resume, since a log
  that opens an episode and never closes it reads as still stuck), io_uring
  provided-buffer exhaustion, the three timeouts, and the stale-pooled-connection retry.

  **All of these are WARN**, including the ones handled gracefully. Grading by
  consequence was wrong: at the default `info` floor a DEBUG line is compiled out, so
  epoll back-pressure would have been invisible in production while the io_uring stream
  drop stayed visible. Same event class, two backends, one log.

### Changed

- The five startup `fprintf` lines are now log lines. Usage and argument errors keep
  `fprintf` deliberately: they happen before the log level is known and their audience
  is a shell, not a log pipeline.
- **One assignment point for the request sequencer.** `x-llmbridge-seq` used to fetch
  its own value; both it and the log lines now read `Connection::req_seq`, assigned once
  when the request frames, so a log line and a response header cannot name different
  requests.

Next up: **Anthropic-in mode** (clients that speak the Anthropic API, fronting an
OpenAI-compatible upstream), Gemini / Cohere streaming, vision / image inputs, and
`cache_control`.

## [0.12.0]. 2026-08-12

### Added

- **`--client-idle SECONDS`**, closing an established client connection that has gone
  quiet. Default three days; `0` disables it. The existing 30 s setup deadline only
  reaps connections that never framed a request, because `ever_framed` latches on the
  first one, so before this a client could send a single request and then hold a file
  descriptor indefinitely. It is a **descriptor bound for an exposed listener, not a
  load-balancing device**: a short window would charge a reconnecting client a fresh
  TCP+TLS handshake to solve a problem the client cannot observe. Requests and streams
  in flight are never reaped, and `stats.client_idle_timeouts` counts what was.
- **A logger**, replacing every `fprintf` that described the running process
  (`--log-level`, `runtime.log_level`, default `info`; stderr).

  Sized so it cannot cost the latency claim. Roughly twenty interesting events per
  request at ~84k RPS is 1.7M records/second against a budget of 41-80 us added p99
  for the whole request, and one `fprintf` costs 1-5 us and takes a lock, so a
  conventional logger would invalidate the benchmark.
  Hence: a **compile-time floor** (`-DLLMBRIDGE_LOG_LEVEL=`, default `info`) that
  removes lower call sites entirely so their arguments are never even evaluated; a
  runtime level behind one relaxed atomic load; a **fixed stack buffer** with no
  allocation and no iostreams; and one `write(2)` per line so concurrent workers
  interleave whole lines with no mutex.

  Every line names its **thread** (`worker/2`, not a 15-digit `pthread_self`), and
  objects carry a stable identity: a connection is `Connection#42(fd=17,client,cid=3)`
  for its whole life, via a free `log_put` found by ADL, which is how a class gets a
  print method without dragging iostreams onto the event loop. Connection lifecycle,
  live client count on change, and upstream pool reuse are `DEBUG`, so a lab build
  (`-DLLMBRIDGE_LOG_LEVEL=debug`) shows them and a Release build does not carry them.

  **No credential is logged at any level**, and the header states the rule the call
  sites must follow: a header's name and length, never its value.

  There is a `Sink` seam. The built-in one writes to stderr.

- **`--config FILE`**, an optional JSON configuration file. Fourteen flags is
  uncomfortable and the fifteenth is impossible: multi-upstream routing needs an
  ordered list with per-upstream fields, which flat flags cannot express without
  inventing a mini-language. The shape is grouped (`listen`, `upstream`, `timeouts`,
  `runtime`) so `upstream` can become an array later without disturbing the rest.
  Every flag still works and **the CLI overrides the file**, so nothing that drives
  the daemon today has to change. Annotated example in `app/llmbridge.example.json`.

  **Unknown keys are a startup error, never ignored.** A config parser that skips a
  misspelled `listen_tls` fails open in exactly the shape fixed in 0.11.0, and a file
  has no `ps` output to catch it. Wrong types, bad enum values and out-of-range
  numbers are refused the same way, each naming the offending key. Keys beginning
  with `_` are comments, since JSON has none and the file is edited by hand.

- **`--pool-idle SECONDS`**, how long a pooled upstream may sit unused before it is
  closed. Previously a hardcoded 30 s. That is a reasonable compromise for one upstream
  and one worker, and the wrong number as soon as either multiplies: every worker keeps
  its own pool, so offered traffic divides across them, each pool crosses the idle line
  more often, and every crossing costs the next request a full reconnect and handshake.
  Default unchanged at 30 s; `0` disables reaping.

### Changed

- **The three TLS startup refusals now name both surfaces.** They named only flags,
  such as `--tls-cert/--tls-key given without --listen-tls`, which an operator using a
  config file never passed. They now name the config keys as well.

### Fixed

- **The installed package reported the wrong version.** `project(llmbridge VERSION ...)`
  was left at `0.10.1` when 0.11.0 was tagged, and that value feeds
  `write_basic_package_version_file`, so `find_package(llmbridge 0.11 REQUIRED)` against
  a 0.11.0 install would have failed. The consumer job in CI calls `find_package` with
  no version, which is why nothing caught it; `scripts/check_conventions.py` now fails
  the build when the CMake version and the newest CHANGELOG entry disagree.

## [0.11.0]. 2026-08-12

**Inbound TLS.** The gateway now terminates the client's TLS as well as originating
its own to the provider, so it can be a remote endpoint instead of only a loopback
sidecar. Minor, because this is new functionality; nothing existing changed shape.

### Added

- **`--listen-tls --tls-cert PATH --tls-key PATH`.** TLS 1.2 floor, ALPN advertising
  `http/1.1` only and failing the handshake on no overlap, renegotiation disabled.
  **One listener, one mode**: `--listen-tls` makes the single listener TLS-only, so
  "am I exposed in the clear?" is answered by reading the command line.
- **Startup validation, each with its own error**: a private key readable beyond its
  owner, a key that does not match the certificate, an already-expired certificate, an
  unreadable path, and either TLS flag given without the other.
- **`accept(TLS)` histogram**, the inbound handshake from accept to handshake-done.
  It completes *before* t0, so no other number in `LATENCY.md` could see it. It is
  deliberately **not** folded into `added-total`: a handshake is per connection and
  `added-total` is per request, so spreading it either way invents a cost or inflates
  one request. Unlike the upstream handshake, this one exists only because the gateway
  is in the path, so it **is** ours; `LATENCY.md` section 1 explains the asymmetry.
- **`streamgen --tls --ca FILE`** in the benchmark harness, the only client in the
  tree that can drive a TLS listener. Verification is not disableable.
- **A libFuzzer target for the inbound handshake** (`fuzz_tls_server`), wired into CI.
  It is the first surface an unauthenticated remote peer reaches.

### Fixed

- **A non-TLS build silently ignored `--listen-tls`**: it bound the listener, served
  **plaintext**, did not check that the certificate paths existed, and warned about
  nothing. The default build has no TLS, so an operator following the inbound-TLS
  instructions without `-DLLMBRIDGE_TLS=ON` would have put every client's API key on a
  routable address believing the listener was encrypted. The mirror guard for the
  upstream leg already existed; this direction, the one that fails open, had none.
  `main.cpp` is linked by no unit test, so every CLI guard in the file was uncovered;
  four `ctest` cases now assert the exact refusal messages.
- **A pooled upstream could be released with its request half-sent.** The upstream's
  recv is armed before the send, so a provider that answers early (a 413 or 401 on the
  headers of a large body) delivers a complete response while our request is still
  going out. Releasing that connection scrubbed and re-used a buffer the transport had
  not finished with, and left a truncated request on the wire for the next client to
  inherit. Reproduced on **both** backends, where the next client's request was
  silently dropped. Such a connection is now closed instead of pooled
  (`stats.upstream_unsent` counts it).
- **Both handshake histograms overflowed their range.** They used the default 20 ns
  buckets over 2.62 ms, sized for the sub-millisecond overhead claim, while a cold
  handshake is 50-80 ms: every cold sample landed in overflow, where `percentile()`
  returns the running maximum, so p50, p99 and max printed one clamped number wearing
  three labels. Local mocks hid it entirely. Now 1 us buckets over 262 ms.
- **`connect(TLS)` never read "exactly 0" on a pooled connection**, as `LATENCY.md`
  claimed; `percentile()` reports a bucket's upper edge, so it read 20 ns. The header
  `x-llmbridge-connect-us` is the exact surface and does read 0. Documented, with the
  quantisation stated, and both halves pinned by tests.
- **A response head with a malformed status line was accepted.** `parse_response` now
  requires `HTTP/1.0` or `HTTP/1.1` followed by a space, which also closed a test that
  had been passing vacuously.

### Known gaps

- **No client authentication.** Anything that can reach the listener can use it with
  its own key. There is no per-client token, quota or rate limit, which is why a
  remote deployment needs an authenticating layer in front of it.
- **No mutual TLS**; client certificates are not requested or verified.
- **Certificate renewal requires a restart**, dropping in-flight requests and streams.
  There is no reload on signal.

## [0.10.1]. 2026-08-09

### Fixed

- **The TLS handshake was billed as the upstream write.** On a cold TLS
  connection `t2` was stamped at TCP connect, before the handshake, while `t3`
  waits for the request ciphertext to flush. The handshake therefore landed in
  `x-llmbridge-upwrite-us` and `x-llmbridge-connect-us` reported TCP alone: a
  live run measured `upwrite-us` at 32-43 ms cold against 34 us warm. `t2` is now
  stamped in `tls_feed()` when the handshake completes, on both backends; pooled
  reuse still reports exactly 0. Published benchmark numbers are unaffected, as
  they run plaintext on pooled connections, and the error *overstated* our own
  added latency. Patch: no new functionality, and the headers now mean what
  `LATENCY.md` always said they meant.

### Added

- Four timing-attribution tests in `gateway_tls_test.cpp`, covering both event
  loops on both the streaming and non-streaming paths. They stall the mock
  provider before `SSL_accept` and assert the stall lands in `connect-us`; each
  fails against 0.10.0.

## [0.10.0]. 2026-08-06

Latency accounting made consistent: `LATENCY.md` was re-derived against the source and
the mismatches fixed in the code. Minor because one response header changes meaning.

### Warning: Breaking

- **`x-llmbridge-connect-us` is now the handshake alone (t2−t1), not handshake + upstream
  write (t3−t1).** It reads exactly **0** on a pooled connection, where it previously read
  a few microseconds. The write moved to the new `x-llmbridge-upwrite-us`; adding the two
  reproduces the old value. It shared a name with the `connect(TLS)` histogram but not a
  span, so one request reported two different "connect" numbers; both now derive from one
  function, `timing_split()`.

### Added

- **`x-llmbridge-upwrite-us`**: the `write()` into the kernel's socket buffer (~4.4 µs
  p50), previously hidden inside `connect-us`. No extra clock reads.

### Fixed

- **Empty latency histograms print `(no samples)`, not zeros.** `p50=0 ns p99=0 ns` reads
  as a perfect result instead of absent data, and `bench/run_bench.sh` scrapes that line,
a zero-sample run could have published a fabricated 0 µs. Streaming hits this every
  run: streams count in `requests` but are never recorded in the histograms.

### Documentation

- **`LATENCY.md`** linked from README/DESIGN/BENCHMARKS and re-derived. Corrections: the
  scheme is seven stamps / six intervals (a stale line said four/three); `upstream-ttfb-us`
  bounds the provider's response *head*, not its first token (measured live: ~1 ms apart
  for Anthropic, a per-provider fact, not a guarantee). §2 gains a stamp table mapping
  t0–t6 to identifiers and the functions that assign them.
- **`BENCHMARKS.md`**: streaming delivery is **99.94–100%**, not "100%"; the shortfall is
  a load-generator boundary artifact (tripling the window cut the deficit 1,225 → 195,
  where a loss rate would have tripled it). Dropped a stale claim that the gateway could
  not terminate TLS to `api.anthropic.com` (shipped in 0.4.0).

### Internal

- `t5` → `ts_resp_built` and io_uring's `ts` → `ts_resp_sent`, matching the `ts_*` scheme
  and the epoll twin. `check_conventions.py` gains a fourth check: LATENCY.md's stamp
  table must name functions that exist and actually assign the stamp.

## [0.9.0]. 2026-08-04

A naming audit, a JSON strictness fix, and the test suite that found it. Minor because
two things break: an installed header moves namespace, and input earlier versions
accepted is now refused.

### Warning: Breaking

- **`llmbridge::json` → `llmbridge::provider::json`.** The only public break. `provider/`
  is the installed header set. Add `provider::`, or alias it in one line.
  (`llmbridge::http` → `llmbridge::net::http` likewise, but `net/` is not installed.)
- **Malformed JSON is now refused, not forwarded.** A string containing a raw control
  character (`U+0000`–`U+001F`) or an invalid escape (`\q`, `\uZZZZ`) fails the parse.
  Well-formed JSON is unaffected, including every RFC 8259 escape, `\u` sequences,
  surrogate pairs and raw UTF-8.

### Fixed

- **The gateway could return `200 OK` with a body no strict JSON parser accepts.** The
  DOM keeps raw string spans for zero-copy passthrough, so control bytes the parser
  accepted were copied verbatim into the client's response. Measured: a provider answer
  containing a raw newline reached the client inside a 200 that Python's `json.loads`
  and so the OpenAI SDK, rejects. Now a `502` with a valid error envelope. A test that
  asserted the old behaviour has been rewritten; the comment claiming "our parser is
  lenient" was true when written and is not now.
- **A cross-backend call the old naming hid.** `ur_forward()` called the *epoll* error
  responder on the malformed-credential path. Harmless in practice, but only by three
  accidents (`peer` still null, a small body completing inline, an already-deferred
  close), any of which a future change could remove.
- **Flaky TLS test fixtures, both copies.** The test CA was written to a fixed temp path,
  so parallel `ctest -j` processes truncated each other's file. The same copy-pasted bug
  existed in `gateway_tls_test.cpp` and `net/tests/tls_test.cpp`, and the first fix landed
  on only one: the exact failure mode this release's audit was about.

### Changed: naming, and it is enforced now

`Gateway` implements the whole request lifecycle twice, at 60–81% similarity for the ten
largest of fourteen twin pairs. The io_uring half was prefixed `u_`; the epoll half was
unprefixed, so the epoll implementation silently occupied the namespace that should mean
"shared". Now `ep_*` / `ur_*` / unprefixed-means-shared, with twins sharing a verb so the
counterpart is greppable. `parse()` → `parse_request()`, and three identical status enums
collapse into one `FrameStatus`.

`scripts/check_conventions.py` runs as the first CI job (~90 ms, no compiler) and fails on
a call crossing the prefixes, an *unprefixed* method reachable from only one backend, or a
namespace not mirroring its directory. The middle check earns its keep: a crossing grep
cannot catch a mislabelled method, which is how `abort_pair` survived the rename that
introduced the convention. Each check is verified by reintroducing the defect it catches.

Diffing all fourteen twins found no unintentional one-sided logic. One divergence is
deliberate and worth knowing: under a slow client epoll applies back-pressure while
io_uring drops the stream past an 8 MiB cap.

### Added: `gateway_corpus_test`

1000 requests over 100 clients, streamed and non-streaming, on both backends. Every
request carries a different question and the mock provider answers by **looking it up**,
so a pooled-connection desync handing client A client B's bytes fails the test by name
a canned-response test is structurally blind to that.

The corpus is 1000 recorded Claude answers (`scripts/gen_qa_corpus.py`; the test itself
never touches the network), typed by what each stresses and curated from 2000 so every
rare character class survives at 100%. It carries real newlines, quotes, backslashes,
astral-plane characters and answers large enough to cross the io_uring provided-buffer
boundary. Negative controls deliberately break each property and assert the harness
notices; that is what exposed the control-character defect above.

### Known: self-reported added latency is not comparable across backends under load

`Histogram::percentile()` returns the running max once the target falls past its 2.62 ms
range, and the io_uring stamps bracket a *submitted* send and its completion, so they also
contain queueing. Across a 1→500 client sweep io_uring's reported median climbs
(43 µs → 1689 µs → unresolvable) while epoll's stays flat (45 → 79 µs). The honest
per-request figure on both is ~45–80 µs, and it does not degrade with concurrency. This
does not affect the published ~80 µs claim, measured at 100 RPS where nothing queues.

### Note on older entries

Entries below refer to the **old** names (`http::parse`, `u_on_recv`, `llmbridge::json`).
They record what the code was called at the time, not a current API reference.

## [0.8.1]. 2026-08-03

A security sweep of the public HTTP surface. **Fixes only, no new functionality**,
hence a patch. No public API change: the one signature that moved
(`http::parse_response`) lives in `net/`, which is internal to the reference gateway
and is not among the installed headers (only `provider/` is).

### Compatibility note

This release **rejects requests that previous versions accepted**, deliberately.
A request whose header block contains a bare CR or LF, an obs-fold continuation
line, whitespace before a colon, a header line with no colon, or a non-numeric
`Content-Length` (`0x1b`, `27abc`) now gets a `400` and is not forwarded. Each of
those was a framing-desync primitive; see below. Well-formed HTTP/1.1 is
unaffected, including the legal trailing whitespace in `Content-Length: 27 `.

### Security: HTTP framing sweep

A systematic sweep of the public request/response surface, done by probing rather
than reading. Eight defects, seven of them in the framer and all with the same
signature: **a malformed header became an invisible header instead of a rejection.**
`parse()` returned `Complete` with `body_len == 0` while the body sat unconsumed.

That is a request-smuggling desync. In passthrough mode (`--translate none`) the
gateway forwards the client's header block verbatim, so an upstream that reads a
length we could not frames a body we never sent, and because the upstream pool is
**shared between clients**, the leftover bytes become the head of another client's
request. Each fix refuses the message; none sanitises and forwards.

Fixed in `parse()` (requests) and `parse_response_head()` (responses):

- **Non-numeric `Content-Length` accepted.** `std::from_chars` stops at the first
  non-digit and still reports success, so `0x1b` parsed as **0** and `27abc` as
  **27**. Now `1*DIGIT` only, per RFC 9112 §8.6. Legal trailing OWS (`"27 "`) is
  still accepted; the fix must not over-reject.
- **Whitespace before the colon.** `Content-Length : 27` matched no `name:` prefix
  test, so the length went unseen. RFC 9112 §5.1 forbids it for this exact reason.
- **Bare CR / bare LF in the header block.** We split on CRLF; a parser that splits
  on a bare CR sees headers we never saw. Measured reaching an upstream.
- **obs-fold continuation lines** (`\r\n Content-Length: 27`), invisible to a prefix
  matcher, honoured by a folding upstream. RFC 9112 §5.2 requires rejection.
- **Header lines with no colon, or an empty field name.**
- **Responses carrying both `Content-Length` and `Transfer-Encoding: chunked`**, and
  **conflicting duplicate `Content-Length` in a response**: neither was rejected.
  A mis-framed *response* on a pooled connection hands one client another's bytes,
  so response framing is now as strict as request framing.

A test that asserted the first of these as a documented parser quirk
(`TrailingGarbageAfterClNumberIsAccepted`) has been **replaced by one asserting the
rejection**. It was a smuggling primitive, not a quirk. The new `HttpDesync` suite
covers every case above and asserts end-to-end that a refused request reaches the
upstream as **zero bytes**: fail closed, verified at the wire, not merely at the
parser.

### Fixed: quadratic cost on the non-streaming chunked path (availability)

`parse_response()` built a fresh `ChunkDecoder` and re-decoded from byte zero on
every call, and it is called on every read. A body arriving in N reads therefore cost
O(N × body). This is the non-streaming response path for **both backends, TLS or
plaintext**, not a TLS-specific path. Measured on one connection with 64 KiB reads:

| body | before | after |
|---|---|---|
| 1 MB | 1.6 ms | 1.3 ms |
| 2 MB | 4.3 ms | 3.1 ms |
| 4 MB | 13.9 ms | 6.5 ms |
| 8 MB | **79.0 ms** | **14.1 ms** |

**Scope, stated precisely.** A typical 1 KB reply (and an 8 KB one) arrives in a
single read, so the re-decode loop never runs: 0.4 µs before and after. The defect
needs a body spanning several 64 KiB reads, so it does not appear below ~128 KB and
does not cost a millisecond until roughly 500 KB–1 MB. Normal traffic was never
affected, and this was never a regression against the p99 < 1 ms added-latency
target, which is measured at ~1 KB payloads.

The harm is **head-of-line blocking**, not the latency of the request that triggers
it. The loop is single-threaded, so a 79 ms decode delays *every other client on
that worker* by up to 79 ms: one oversized response degrades everyone else's p99.
An unusually large legitimate completion can trigger it; a hostile or compromised
upstream can do it deliberately, at will, up to `kMaxBodyLen` (16 MiB), which is the
threat that justifies the fix. Decode state now lives per connection
(`http::ResponseDecoder`) and is fed only newly-arrived bytes, and is reset between
responses on a pooled connection. The regression test asserts the invariant
deterministically: the decoder's consumed count only moves forward, and never by
more than the bytes that just arrived, instead of timing it, because a timing
assertion flaked on allocator warmth and a flaky security test is one people ignore.

### Fixed: flaky TLS test fixture

The TLS test CA was written to a fixed path in the shared temp dir, so parallel
`ctest -j` processes truncated each other's file, surfacing as a spurious
"no certificate or crl found" in whichever test lost the race. Now unique per
process and per call.

### Verified clean (probed, not assumed)

- **Credentials and the shared pool.** A key never inherited by a client that sent
  none, never trailing a shorter reused request, never echoed as `Authorization`
  upstream, never in an error body or the gateway log.
- **TLS.** No bypass path exists: verification is unconditional, with tests for an
  untrusted chain, a hostname mismatch, and explicitly no plaintext fallback.
- **Availability.** Survives 300 slow-loris connections, 100k-deep nested JSON
  (depth capped at 64), an over-cap `Content-Length` and an over-cap header block
  all refused with 400, process alive and still serving throughout.

Known gaps are stated in `SECURITY.md`, which now also documents the **inbound
plaintext leg** (client → gateway is HTTP, so the OSS gateway is a loopback sidecar,
not a remote endpoint) and the **absence of a client-side idle timeout**.

## [0.8.0]. 2026-08-03

Streamed tool calls. A `"stream": true` request whose model calls a tool now produces
proper OpenAI `tool_calls` deltas, so an agent loop works over SSE. This **supersedes
the guard added in 0.7.0**, which aborted such streams instead of mis-reporting them.

Verified against the live Anthropic API, and re-verified after every round of audit
fixes below, most recently on all three paths at once (streamed parallel calls,
streamed text with tools declared, non-streaming):
two parallel calls arrived as fragments (`{"city": "P` / `ar` / `is"}`) and
reassembled client-side into valid JSON, with `finish_reason: "tool_calls"` and a
clean `[DONE]`; a text stream with tools declared is unaffected.

### Added

- **`content_block_start` → the opening `tool_calls` delta**, carrying `index`, `id`,
  `type:"function"` and `function.name` with an empty `arguments`: the chunk an
  OpenAI client keys off to begin a new call. The role chunk is emitted first even
  when a stream opens straight into a tool call with no text.
- **`input_json_delta` → `arguments` fragments** under the same index, which the
  client concatenates.

### The index mapping (the part that is easy to get wrong)

Anthropic indexes **every content block**, text included; OpenAI's
`tool_calls[].index` counts **only tool calls**. The two diverge the moment text
precedes a call. Anthropic blocks 1 and 2 must become OpenAI ordinals 0 and 1.
Forwarding Anthropic's index directly would emit `tool_calls[1]` with no `[0]`, which
breaks reassembly in every SDK. A dedicated test (`BlockIndexIsNotTheToolOrdinal`)
pins this.

The map is a bounded vector (256 blocks) holding `-1` for non-tool blocks, so a
hostile index cannot make the translator allocate.

### Arguments forward as raw spans

Anthropic's `partial_json` and OpenAI's `arguments` are both JSON strings whose
*contents* are JSON text, escaped identically, so fragments pass through **verbatim**
instead of being decoded and re-encoded. A round trip through the DOM could alter a
customer's argument bytes; the test asserts fragments reassemble byte-for-byte
including `1.50`, which a re-serialising implementation would normalize to `1.5`.
Control bytes are still neutralised on the way out, as for text.

### Hardening found by pre-merge audit

Seven defects across three review passes, **every one caught by probing edge cases
instead of reading the code**, the reasoning pass before them found none. Two were
introduced by earlier fixes in this same list, which is why the last pass was done as
a cold second reading of `dispatch()` and `finish()` instead of another look by their
author.

- **A malformed `index` stole another call's arguments.** `detail::to_ll()` wraps
  `std::from_chars`, which on overflow or garbage leaves its output **untouched**, so
  `"index": 99999999999999999999` came back as `0` and its argument fragments were
  attached to whichever tool call occupied block 0. In an agent loop, arguments on the
  wrong tool means the wrong action. A strict parser now rejects overflow, garbage and
  trailing characters; the event is ignored.
- **A truncated tool call fabricated a clean ending.** Arguments cut mid-JSON still
  produced `[DONE]`, so a client concatenated unparseable JSON inside a stream that
  looked complete: the exact "corrupt framing fabricated a clean ending" failure
  0.3.0 fixed for text, reintroduced by streaming tool calls. Truncated prose is still
  readable; truncated arguments are garbage a client may dispatch. `finish()` now
  returns `false`, which the gateway already maps to `Failed`.
- **Unnamed tools were emitted.** The non-streaming translator drops a tool with no
  name ("unusable without a name"); streaming emitted `"name":""`, so the same
  upstream produced a usable response one way and an undispatchable call the other.
  Now consistent.
- **`finish()` reported failure on an already-complete stream**, a regression
  introduced by the truncation guard above. It checked the open-tool flag *before*
  `_done`, so an upstream sending `message_stop` without a preceding `message_delta`
  got `[DONE]` from `dispatch()` and then a failure from `finish()`, making the
  gateway count an error and close abruptly on a correct response. Order fixed, and
  `message_stop` now clears the flag too, so the invariant holds whichever terminator
  arrives.
- **The tool-ordinal counter was unbounded.** A reopened block index could increment
  it without limit; signed overflow is UB. Capped at 256; no legitimate response has
  more tool calls than that.
- **`finish_reason: "stop"` was reported after emitting tool calls**, the worst bug in
  the release, because it fails *silently*. Every OpenAI SDK branches on
  `finish_reason == "tool_calls"` to decide whether to dispatch, so a client receiving
  `"stop"` treats the call as a plain answer and **never runs the tool**: no error, no
  exception, just a tool that quietly does not fire. Reachable whenever the upstream
  sends `message_stop` without a preceding `message_delta`. The default is now
  `"tool_calls"` once any call has been emitted, and a text-only stream still defaults
  to `"stop"` (pinned by its own test so the new default cannot leak).
- **A foreign `data: [DONE]` vouched for a truncated call.** `[DONE]` is an OpenAI-ism
  Anthropic ends with `message_stop`, and the handler jumped straight to the sentinel.
  That gave a truncated tool call a clean ending *and* skipped the finish chunk, so the
  stream ended with `finish_reason: null` and a client had no way to know the message
  had ended. It now routes through the same path (a complete stream still gets its
  finish chunk) but does **not** clear the open-tool flag: a foreign terminator cannot
  vouch that arguments are whole.

**Fuzz coverage.** The SSE corpus contained **no tool events**, so
`content_block_start` and `input_json_delta` were never fuzzed. Two seeds added
(parallel calls behind a leading text block; hostile indices with escaped `id`/`name`).
2.87M executions clean afterwards, which matters most for invariant (2),
fragmentation-invariance, since this release added cross-event state and the fuzzer's
own comment calls that "the property most likely to break as state grows".

### Tests

**21 new** in the translator, replacing 0.7.0's four guard tests: opening chunk shape and role ordering;
fragments reassemble exactly; **block index ≠ tool ordinal**; interleaved fragments
route to their own call without cross-contamination; `finish_reason:"tool_calls"` now
has calls behind it: the regression this feature existed to fix; a fragment for a
block whose `content_block_start` was never seen is **ignored instead of guessed at**
(attaching a customer's arguments to the wrong call is worse than dropping them); an
absurd block index does not allocate; plain text streams unaffected.

From the audit: malformed indices (overflow, negative-overflow, `1e5`, `0x10`) are
rejected instead of aliased to block 0; a malformed index opens no call; every
emitted chunk stays valid JSON under hostile escaping (escaped quotes, trailing
backslashes, `\uXXXX`, tabs, and a hostile `id`/`name`); tool chunks carry
`usage:null` when `include_usage` is set; unnamed tools are dropped; a truncated call
refuses a clean ending while a completed one still gets `[DONE]`; `message_stop`
without `message_delta` still ends cleanly; the ordinal counter is bounded.

**6 new gateway tests**, both backends × chunk sizes {7, 64, 4096}: a streamed tool
call survives the gateway pump and reassembles, with ordinals 0/1 despite Anthropic
using blocks 1/2. Small chunk sizes matter because tool events are longer than text
events and so more likely to straddle a boundary mid-JSON. Before this, the string
`input_json_delta` appeared **zero** times in the gateway suite: the pump path was
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
- Streaming remains OpenAI ⇄ Anthropic only. Gemini and Cohere have no streaming
  translator, tool calls or otherwise.


## [0.7.0]. 2026-08-03

Tool calling, non-streaming. An OpenAI-dialect client can declare tools, receive a
tool call, execute it and return the result (the full agent loop) against an
Anthropic upstream. Verified end to end against the live API: Claude called
`get_weather` twice in parallel and, given the results, answered *"The current
weather in Paris is 18°C with light rain."*

### Streaming tool calls fail cleanly (rendering them is next)

> **Superseded by 0.8.0**, which renders streamed tool calls properly. The guard
> below describes 0.7.0 only; tool calls no longer require a non-streaming request.

Tool calls require a **non-streaming** request. Rendering Anthropic's streamed
`tool_use` / `input_json_delta` as OpenAI `tool_calls` deltas is the next change.

Until then a streamed tool call **aborts the stream**: no `[DONE]`, the same signal
this translator already uses for a corrupt body, instead of completing with a
misleading result.

That guard is the point. Before it, the SSE path handled `text_delta` only, so the
`tool_use` block and its argument fragments were **silently dropped while
`stop_reason: tool_use` still mapped through to `finish_reason: "tool_calls"`**. The
client was told *"I called a tool"* and handed no `tool_calls` array: a response that
looks valid, is not, and would send an agent loop hunting for a call it never
received. Failing is strictly better than that, and it is what a client can actually
detect.

### Added

- **Tool declarations**. OpenAI `tools[].function{name,description,parameters}` →
  Anthropic `{name,description,input_schema}`. The schema is forwarded as a **raw
  byte span**, never rebuilt: a customer's JSON Schema is arbitrary, and
  re-serialising it from the DOM could change number formatting (`1.50`), escape
  forms or key order. Tested by asserting the exact source text appears in the
  output.
- **`tool_choice`**. `"auto"` → `{"type":"auto"}`, `"required"` → `{"type":"any"}`,
  `{"type":"function","function":{"name":...}}` → `{"type":"tool","name":...}`, and
  `"none"` → the tools block is omitted entirely, which is how Anthropic expresses
  it.
- **The call itself**. OpenAI carries arguments as a JSON **string**, Anthropic as
  a JSON **object**. Crossing that boundary is a real conversion in both directions,
  which is why `json.hpp` grew `append_escaped_string()` and `unescape_string()`.
- **Tool results**. OpenAI `role:"tool"` + `tool_call_id` → an Anthropic **user**
  turn containing `tool_result` blocks. Consecutive tool messages merge into one
  turn: a parallel call is semantically one turn of results. (Measured, not assumed
  the live API accepts consecutive same-role turns and returns 200; an earlier code
  comment claimed otherwise and was wrong.)
- **Responses**. `tool_use` blocks → OpenAI `tool_calls[]`, with `content` set to
  **`null` instead of `""`** on a pure tool call, because SDKs branch on null and an
  empty string reads as "the model answered nothing".
- `json.hpp`: `Value::sv` now carries the **raw span for objects and arrays**
  (brackets included), which is what makes byte-for-byte schema forwarding possible.
- **Guard: a streamed tool call fails the stream** instead of dropping the call.
  Detected at `content_block_start` (so it triggers even for a zero-argument tool)
  and at `input_json_delta`, then routed through the same sticky-failure path as a
  cap overflow. Text-only streams are untouched.
- `Stats::connect` histogram and `Connection::ts_wire_ready` stamp; see the
  latency-profile fix below.
- `bench/fastbackend --tools`: a mock serving an Anthropic response with two
  `tool_use` blocks. Without it the regression sweep reports "no change" for edits
  that only touch the tool path; a live check had already shown 19 µs for tool
  responses against 15 µs for plain, a difference the benchmark could not see. Body
  selection is now an `enum` instead of two bools, so `openai+tools`, a state that
  does not exist, cannot be represented.

### Fixed: the self-reported latency profile counted the wrong things

`Stats::req_path` ran from *request framed* to *bytes on the wire*, so the **TCP
connect and TLS handshake sat inside it**, and inside `added-total`, the profile's
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

- `req_path`: framing, translation, auth mapping **plus the `write()` to the
  upstream**
- `connect`: the TCP + TLS handshake **alone**; exactly 0 on a pooled connection
- `added-total`. `req_path` + `resp_path`, unchanged in meaning: everything the
  gateway does

**The first attempt at this split was wrong in the flattering direction, which is
why it is written up.** It treated the whole `ts_req_built → ts_up_sent` interval as
"connect" and excluded it. But a pooled connection performs no handshake, so that
4.4 µs was the `write()` syscall, unambiguously our cost. Measured under identical
warm-pool conditions:

| | before | first (wrong) cut | correct |
|---|---|---|---|
| `added-total` p50 | n/a | 8,560 ns | **12,700 ns** |
| `request-path` p50 | 1,920 ns | 1,920 ns | **6,140 ns** |
| `connect` p50 | 4,420 ns | 4,420 ns | **20 ns** |

`connect` at 20 ns is a stamp subtraction on a pooled connection, correctly zero.
The rule that caught it: when a number improves, ask what else changed. It had
improved because a real cost stopped being counted.

**No published figure moves.** `BENCHMARKS.md`'s added-latency numbers come from
`loadgen`'s client-observed measurement, not this histogram; the profile is a
diagnostic printed at shutdown. The consequence is only that the profile previously
overstated `added-total` on cold connections and was unaffected when warm, so a
cold-start run quoted from the profile instead of the client measurement would have
been too high.

### Robustness

Malformed tool pieces are **dropped, never guessed at**: a tool with no name, a call
with no `function`, empty or absent `arguments` (→ `{}`). A half-formed tool would
make the provider fail in a way the client cannot read.

Escaping is handled properly instead of approximately: control bytes are
`\u`-escaped (a raw control byte in a JSON string is invalid JSON that some parsers
accept and others reject), `\uXXXX` decodes to UTF-8 including surrogate pairs, and
a **lone surrogate becomes U+FFFD** instead of invalid UTF-8 that would make a
provider reject the whole body.

### Tests

**47 new.** Translator (24): declaration shape, byte-for-byte schema forwarding, all
five `tool_choice` cases, arguments-string ↔ input-object, parallel calls, text+call
in one turn, tool-result turns, consecutive-result merging, the trailing-turn close
(a malformed-JSON regression guard), malformed pieces dropped, `content:null`, no
`tool_calls` key on a plain answer, and a **full round trip**. Anthropic `input` →
OpenAI `arguments` → back to Anthropic `input`, through `"say \"hi\"\nnow"`,
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
**87,933 RPS** at 90k offered, the top of the canonical 84–87k band. Tool-response translation itself costs about **+4 µs** (19 µs vs 15 µs for a plain
response), measured live. The bench-harness A/B that would confirm it needs a cold
box: the attempt ran with the machine at its 73 °C thermal floor and produced a
4,540 µs outlier on a *plain* run, so no number is published from it.

## [0.6.0]. 2026-08-01

Per-request observability metadata. A client can now see, for every response, what the
gateway cost versus what the provider cost, **when** the request arrived, **where it
sits in a total order**, and how many tokens it moved. Opt-in (`--timing-headers`,
default off), because adding a header is a visible API change.

### Added

- **`--timing-headers`**: response headers decomposing where the time went. Metadata
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
  | `x-llmbridge-t0` | n/a | epoch **nanoseconds** at request arrival |
  | `x-llmbridge-seq` | n/a | monotonic sequence number, a total order needing no clock |
  | `x-llmbridge-gateway-us` | (t1−t0)+(t4−t3) | **our compute**: framing, translation, auth mapping, re-serialisation |
  | `x-llmbridge-connect-us` | t2−t1 | TCP connect + TLS handshake; ~0 on a pooled connection |
  | `x-llmbridge-upstream-us` | t3−t2 | the provider: network + inference |
  | `x-llmbridge-tokens-in` | n/a | prompt tokens, **the provider's own count** (non-streaming) |
  | `x-llmbridge-tokens-out` | n/a | completion tokens, likewise (non-streaming) |

  Streaming cannot report t4 (headers precede the body) so it emits
  `x-llmbridge-upstream-ttfb-us` (time to the provider's first byte) instead of a total
  it does not yet have.

  Measured live against `api.anthropic.com`: **gateway 17–51 µs** against **1.2–2.6 s**
  of provider time (a gateway share of **0.001%**) with `connect-us` falling from
  ~50,000 µs on the cold connection to ~36 µs once pooled, which incidentally makes
  connection reuse visible per request.

- **`x-llmbridge-seq`**: a process-wide monotonic sequence number, `std::atomic<uint64_t>`
  with `fetch_add(relaxed)`. **Not `volatile`**: volatile provides neither atomicity nor
  inter-thread ordering in C++, and workers are `std::thread`s sharing the process, so a
  volatile counter would be a data race that hands two requests the same number. Relaxed
  ordering is sufficient and cheapest: every atomic has a single total modification
  order, so the values are unique and increasing in the order the increments happened;
  we need the counter ordered, not the memory around it.

  **Why it exists:** two requests can share a nanosecond, and clocks on different hosts
  cannot be trusted to sub-millisecond agreement without PTP. `(t0, seq)` is a total
  order that needs neither. This is the sequencer pattern: an exchange defines order by
  arrival at a sequencing point, not by comparing timestamps, and it is the argument
  for the tape for inference **sequencing instead of timestamping**.
- **`x-llmbridge-tokens-in` / `-out`**, the provider's own counts, never estimated,
  asserted by a test that the header value **equals the body value exactly** (one source
  of truth). Extraction is a bounded scan of the last 256 bytes, since `usage` is the
  tail object of the response we build, so a multi-KB completion does not pay an extra
  full pass. No match means the headers are omitted instead of guessed.
- **`metrics::wall_ns()`**, an orderable wall clock. `now_ns()` is `steady_clock`:
  correct for intervals, but it has no epoch and cannot order anything. A raw
  `CLOCK_REALTIME` read is not a substitute either, because NTP can **step** it, leaving
  two requests orderable by arrival but not by timestamp: the one property an order
  book cannot lose. So realtime is read **once** at startup and every later stamp is
  that anchor plus a monotonic delta: epoch-meaningful, strictly increasing, immune to
  NTP steps. Trade-off stated at the definition: ppm drift from true wall time over long
  runs, and cross-host joins at sub-millisecond accuracy need PTP, which one gateway
  cannot promise alone.
- New `Connection::ts_req_built` stamp: the end of our request-side work, which is what
  makes the gateway/connect split possible.

### Why `connect-us` is separate

The first implementation folded connection setup into `gateway-us`, and the live API
said **56 ms**. It was measuring truthfully and reporting misleadingly: a cold TCP+TLS
handshake to Anthropic, against 47–63 µs for the same gateway once the connection was
pooled. A customer reading 56 ms as gateway overhead would be right to walk away and
wrong about the software. One number cannot honestly carry both, so there are two.

### Tests

16 new, both backends: headers absent by default; all present when enabled; **t0 is
epoch-scale** (>1.7e18; a monotonic uptime counter would be ~1e10); **t0 strictly
increasing across five requests**; the JSON body byte-identical with headers on;
streaming emitting TTFB while *not* claiming a gateway total it cannot have; token
headers matching the body exactly; and streaming asserted to carry `seq` but **no**
token headers.

The sequence-number test drives **4 threads × 6 concurrent requests and asserts no
duplicate**: the one failure this must never have, and the one a non-atomic counter
would produce.

### Known gaps

- **Passthrough (`--translate none`) emits no headers at all.** That mode forwards the
  upstream's bytes verbatim by contract; injecting headers would break the byte-exact
  guarantee. Translated and streaming responses carry them.
- **Streaming carries no token counts and no chunk count.** Both are end-of-stream
  facts and headers precede the body; inventing them would be worse than omitting them.
  A streaming client that wants counts sets `stream_options.include_usage` and reads
  the provider's own numbers from the final chunk. A chunk count would have to go
  somewhere other than the response: gateway stats, or the telemetry path.
- Timestamps are per-process. Ordering holds within one gateway; across workers or hosts
  it needs synchronised clocks, which is a shadow-order-book concern instead of a
  gateway one.

## [0.5.2]. 2026-07-31

Performance and API cleanup for the two preceding releases. No behaviour change: the
same requests produce the same bytes. Found by an A/B regression check against the
pre-0.5.0 build, not by profiling a guess.

### Fixed

- **Auth-header scanning walked the client's header block six times per request**,
  four to validate the credential-bearing headers, then up to two more to fetch the
  value it needed. One pass now collects all four into a struct. Measured on a
  thermally-gated interleaved A/B at 20k RPS: this cost **~40 µs at p99 (~8%)** with
  p50 unchanged: the signature of a little extra work on every request. After the
  fix the two builds' p99 distributions interleave (min −20 µs, median +15 µs vs the
  pre-0.5.0 baseline), i.e. no measurable difference. First-occurrence-wins is
  preserved, and validation still covers every credential header the client sent, not
  only the one the target dialect uses.
- **`parse_response` copied the body on the `Content-Length` path**, where the
  previous `parse()` handed out a `string_view` over the receive buffer. 0.5.1
  introduced that copy on the highest-rate path for no reason; the body is already
  contiguous there. It is now a view again. The copy remains only for **chunked**,
  where it is inherent to the encoding: body bytes are interleaved with chunk-size
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
  stays a parameter because the caller owns the reusable buffer; that ownership is
  the allocation-avoidance above.
- **Lifetime contract, now explicit in the header:** `body` aliases the receive buffer
  (`Content-Length`) or the scratch buffer (chunked), so it is valid only until either
  is modified; in the gateway, before `rbuf` is erased or the upstream released. The
  ordering is verified by the suite under ASan/UBSan, which is what would catch a
  dangling view.

### Benchmarks

Verified against the pre-0.5.0 build on both paths, interleaved and temperature-gated:
streaming per-token p50 **108 µs vs 109 µs** and p99 **182 µs vs 183 µs** at 64
concurrent streams with 256/256 streams completing; non-streaming p99 restored as
above. No published figure changes.

`bench/BENCHMARK-CONFIG.md` gains **§3d, the A/B discipline**: interleave arms, gate
on temperature, **re-run with the arms swapped**, report min as well as median, stay
under the knee, and verify the process is alive. Every rule is there because its
absence produced a confident wrong answer during this work: thermal noise hid the
40 µs regression above; an ephemeral-port collision read as "the new build crashes";
and first-run-is-colder invented a 0.3 ms TTFT regression that vanished when the arm
order was reversed.

## [0.5.1]. 2026-07-31

Read chunked upstream responses on the non-streaming path. Found by the first live
run against the real Anthropic API, which returned `502` for every non-streaming
request while streaming worked, a gap no mock had ever exercised.

### Fixed

- **Non-streaming responses framed with `Transfer-Encoding: chunked` are now read
  correctly** (previously `502 Bad Gateway`). Real providers return non-streaming
  completions chunked over HTTP/1.1, which a server does whenever the body length is
  unknown when headers are sent, which is the normal case for a generated completion.
  Anthropic does exactly this; it is invisible over HTTP/2 (native framing, no chunked
  encoding exists there), so a `curl` probe that negotiates h2 will not reveal it.
  The gateway's whole-body path used `http::parse()`, which rejects `Transfer-Encoding`
  outright, so the response could not be framed at all.
- `ChunkDecoder::consumed()`: a chunked message's end is found by *decoding*, not by
  arithmetic on `Content-Length`. Without it the gateway could not tell where the
  message stopped, which on a pooled connection would leave stray bytes to be
  mis-read as the head of the next response.

### Added

- `http::parse_response()`: response-side framing that accepts **both** body
  encodings, deliberately separate from `parse()` so the request path is untouched
  (see "Why the asymmetry is safe"). Idempotent like `parse()`: re-frames from the
  buffer start and returns `NeedMore` until the whole message is present.
- 18 regression tests across both backends × {1, 3, 17} chunks: translated round-trip,
  passthrough re-framing, and **three sequential requests over a pooled connection**.
  A wrong end-of-message offset only shows up on the *second* request, so a single
  request would not have caught it. The test mock can now reply chunked; every mock in
  the suite previously replied with `Content-Length`, which is precisely why this
  survived 767 tests.

### Changed

- A chunked response on the **passthrough** path (`--translate none`) is re-framed to
  the client with `Content-Length` instead of relayed verbatim. We have already
  decoded it, and forwarding framing we did not re-verify would push the problem
  downstream.

### Why the asymmetry is safe

The gateway now **sends** `Content-Length` and **accepts** either encoding in
responses. That is deliberate, and it is the standard posture for a proxy:

| leg | framing | rationale |
|---|---|---|
| client → gateway (request) | `Content-Length` only; `Transfer-Encoding` **rejected** | we are the server, bytes are attacker-controlled, and a TE/CL desync against a TE-honouring upstream is the classic smuggling attack, and worse here, since a desync on a **pooled** upstream lets one client's trailing bytes become the head of another client's request |
| gateway → upstream (request) | `Content-Length`, built by us | nothing to desync: we choose the framing |
| upstream → gateway (response) | `Content-Length` **or** chunked | we are the client of a configured, TLS-verified provider; chunked is ordinary HTTP/1.1 and refusing it means refusing real providers |
| gateway → client (response) | `Content-Length` (non-streaming) | the client never sees framing we did not produce |

**Content cannot break the framing.** Both encodings are *length-prefixed*, not
delimiter-scanned: each chunk declares its size and the decoder copies exactly that
many bytes regardless of what they contain. Model-generated text inside the body
including text engineered by prompt injection to look like `0\r\n\r\n`, is just
bytes inside a chunk whose length the provider already declared. There is no
delimiter for content to forge. (This is also why the request path can safely keep
its stricter rule: the danger there was never content, it was two parties disagreeing
about which *header* defines the length.)

Defence in depth on the response path, all pre-existing and now load-bearing:

- **`Transfer-Encoding` and `Content-Length` both present → refused.** Even from a
  trusted origin that combination is a smuggling signal (a compromised or buggy
  middlebox), so it is rejected instead of resolved by preference.
- **Chunk sizes are bounded**, a size line over 64 bytes or a chunk over `kMaxBodyLen`
  fails the stream; the framer is covered by the `fuzz_http` target in CI.
- **A framing error never pools the connection.** `error_respond()` closes the upstream
  instead of releasing it, so a connection we could not frame cannot be handed to the
  next request.

### Known gaps

- The upstream is trusted to the extent that TLS verification makes it so. A genuinely
  compromised provider could desync responses deliberately; a compromised provider can
  already return arbitrary content to every one of its clients, so this changes little,
  but it is the reason the bounds and the close-on-error rule above exist.
- HTTP/2 to the upstream is still not implemented, so llmbridge always negotiates
  HTTP/1.1 and therefore always meets chunked in practice.

## [0.5.0]. 2026-07-31

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
  `TranslateMode::None` is unchanged: byte-forwarding already carries every header.
  Streaming shares the forward path, so streamed requests carry auth identically.
- **`Host` header on rebuilt upstream requests**, previously absent entirely. HTTP/1.1
  requires it; the benchmark mocks tolerated the omission, real providers do not.
  Derived from the parsed upstream hostname (falls back to `ip:port`), default ports
  omitted.
- `http::find_header()`: case-insensitive, zero-copy, first-occurrence-wins lookup.
  First-wins is deliberate: a duplicated credential must resolve the same way for us
  and for the upstream.

### Security

- **Whitelist, not echo.** Only credential headers the target dialect understands cross
  a rebuilt request. Arbitrary client headers (cookies, tracing, anything) do not.
  Echoing them would be both a smuggling surface and a privacy leak to a third-party
  provider.
- **Header-injection fix, found by audit before any real key was used.** Credential
  values are now charset-validated (printable ASCII only) and a malformed one fails the
  request with `400` **without contacting the upstream at all**. This closes a real,
  measured hole: `find_header` splits on CRLF, so a **bare CR** survived inside a value
  and reached an upstream as `x-api-key: sk\rX-Smuggled: yes`. A lenient parser treating
  bare CR as a line terminator would have seen an injected header, and because upstream
  connections are **pooled and shared between clients**, that is a cross-client
  request-splitting vector, not merely a self-inflicted malformed request. Regression
  tests cover bare CR, control characters, a second credential header attempting to
  hide behind a clean first one, and oversized values.
- Trailing whitespace is trimmed from credential values (forwarding it verbatim breaks
  real provider auth).
- Credentials are read as a view over the client's request buffer and written straight
  into the upstream bytes: never stored, never logged, never placed in stats or error
  responses.
- **Pooled upstream buffers are scrubbed, not just cleared.** A pooled connection idles
  up to 30 s; `std::string::clear()` leaves the credential bytes in the allocation, so
  release now `explicit_bzero`s the request buffer first (`explicit_bzero`, not `memset`,
writing to soon-unused memory is a dead store the optimizer may delete). ~100 bytes
  once per request, ≈0.04% of a core at 84k RPS.
- **Cross-client leak tests.** Pooled connections are shared, so "one client's key can
  never ride another client's request" is asserted instead of assumed: a key must appear
  **exactly once** across every request the upstream ever saw (three-client variant too),
  the pool is asserted to have actually been exercised, and no credential appears in any
  response, including the `400` path, the likeliest place for a "helpful" echo.
- **Plaintext warning at startup.** A non-loopback, non-TLS upstream now prints a loud
  warning that forwarded credentials travel unencrypted. Loopback is exempt (mocks,
  benchmarks, sidecar deployments).

### Known gaps

- **Scrubbing is targeted, not exhaustive.** The pooled upstream request buffer is
  scrubbed (the only place a credential outlives its request). Transient buffers, namely the
  client's `rbuf`, freed allocations after a move, are not: they are overwritten within
  microseconds, and scrubbing every one would put a `memset` on the hot path for no real
  gain. An attacker able to read live process memory would find the in-flight request and
  the TLS session keys regardless.
- **`--translate none` forwards every client header verbatim**, credentials included
  that is the byte-forward contract, not a leak, but it means the passthrough mode offers
  no header whitelist.
- No gateway-side credential store: the gateway forwards the client's key and holds
  none of its own. Per-client keys, rotation and quota live in the commercial layer.
- Verified only against hermetic mocks (project policy forbids tests hitting live
  provider APIs); no request has yet been made to a real provider through this path.

## [0.4.0]. 2026-07-31

TLS to the upstream. The gateway can now front an `https://` provider endpoint,
`llmbridge --upstream https://host`, on both event-loop backends. **Opt-in at build
time** (`-DLLMBRIDGE_TLS=ON`, needs OpenSSL ≥ 3.0): the default build remains
zero-dependency end to end, and the translator library never links OpenSSL in any
configuration. See DESIGN.md § "TLS to the upstream" for the data-flow diagram.

### Added

- **TLS transport** (`net/tls.hpp`): OpenSSL driven through **memory BIOs**, not
  `SSL_set_fd`: the event loop keeps the socket and the `SSL` object is a pure byte
  transform, which is the only shape that works on io_uring (multishot recv hands the
  loop bytes that have *already* been read; there is no read left for OpenSSL to
  perform). Same four calls on both backends.
- **Gateway TLS integration**, both backends. Design invariant: `rbuf`/`wbuf` hold
  **plaintext always**. TLS interposes strictly at the socket edge, so HTTP framing,
  the SSE pump, translation, and stale-pool retry-resend are unchanged and unaware.
  io_uring serializes one send at a time so the ciphertext buffer stays immutable
  while the kernel reads it; ciphertext produced meanwhile stages in the write BIO.
- **TLS sessions survive the keep-alive pool**: a pooled reuse pays no second
  handshake (asserted by test: N requests, one handshake).
- **`--upstream` now accepts `HOST:PORT`, `http://HOST[:PORT]`, `https://HOST[:PORT]`**
  (`net/upstream.hpp`), with DNS resolution via `getaddrinfo` once at startup (A
  records, resolver order kept for the future failover PR). `IP:PORT` unchanged. The
  parser is a security boundary (the host string feeds the Host header and SNI) so
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

- **Certificate verification cannot be disabled**: no insecure flag exists. Chain
  (`SSL_VERIFY_PEER`) and hostname (`SSL_set1_host`) both derive from one argument,
  so SNI and the verified name cannot disagree; a partially-initialized session is
  torn down so ignoring an init failure cannot yield an unverified handshake.
- TLS 1.2 floor; server-initiated renegotiation refused (`SSL_OP_NO_RENEGOTIATION`);
  `ERR_clear_error()` before every SSL operation (a stale thread-local error-queue
  entry misclassifies a benign `WANT_READ` as fatal); span lengths clamped to
  `INT_MAX` instead of cast.
- **A fatal TLS record error mid-stream aborts the client on both backends**, found
  by review on epoll, where corruption previously fell into the clean-EOF path and a
  corrupted stream could be finalized with a well-formed `[DONE]`.

### Known gaps

- No OCSP/CRL revocation checking (usual for non-browser TLS clients; documented).
- A provider EOF without `close_notify` is treated as a normal end of a
  close-delimited stream; providers rarely send it, and strict truncation detection
  would break real streams.
- Re-resolution on DNS TTL expiry: a long-lived gateway pins the startup A record
  (congruent with connection pooling; revisit with the failover PR).
- Auth headers are **not yet forwarded**, so calling a real provider still fails auth;
  that is the next change.

## [0.3.0]. 2026-07-29

Streaming (SSE). `llmbridge` now translates a live Anthropic event stream into
OpenAI `chat.completion.chunk`s token-by-token, end to end through the gateway, on
both event-loop backends.

### Added

- **SSE translator** (`provider/sse.hpp`). `AnthropicToOpenAiSse`, an incremental,
  stateful `feed()`/`finish()` pump: raw upstream bytes in, OpenAI chunks out. Holds
  cross-chunk state (id/model/`created`, role-emitted, finish reason) and buffers
  partial events, so a network read splitting an event mid-JSON is handled correctly.
- **Streaming response framing** (`net/http.hpp`). `parse_response_head()` (detects
  `text/event-stream` and chunked encoding) and `ChunkDecoder`, an incremental
  chunked-transfer decoder. Applied to the **upstream response path only**; client
  request framing stays Content-Length-only, so the anti-smuggling posture is
  unchanged.
- **Gateway streaming pump** on **both backends**, epoll and io_uring. A
  `"stream": true` request through `--translate anthropic` is forwarded with the flag
  set, and the upstream SSE response is decoded, translated, and written to the client
  incrementally. Client responses are close-delimited (`Connection: close`).
- **Back-pressure**: epoll pauses upstream reads while a slow client drains; io_uring
  uses a bounded output buffer (8 MiB) because pausing a multishot recv would require
  cancel/re-arm. Different mechanisms, same guarantee: a slow client cannot make the
  gateway buffer without bound.
- **`stream_options.include_usage`**: when the client requests it, every chunk carries
  `"usage": null` and a final chunk with empty `choices` plus real token counts is
  emitted before `data: [DONE]`, matching OpenAI's contract. Counts are the provider's
  own (Anthropic's `message_start` / `message_delta`), never estimated.
- **Upstream idle timeouts**: a new `--upstream-timeout SECONDS` flag and
  `Gateway(..., upstream_idle_ns)` parameter (default **120 s**, `0` disables). A
  request with no upstream progress is aborted instead of pinning a client connection
  and two fds indefinitely. Runs on each loop's existing periodic tick.
- **Upstream provider error passthrough**. `provider::upstream_error_to_openai()` maps
  Anthropic / OpenAI-style / Gemini error bodies into the OpenAI error envelope, and the
  gateway relays the upstream's **own status code**.
- **New stats**. `upstream_timeouts` and `stream_pauses`, both surfaced in the
  gateway's stats output.
- **SSE fuzzer** (`fuzz/fuzz_sse.cpp`) plus a seed corpus, wired into CI. It asserts two
  invariants, not just "doesn't crash": output strictness (no bare control bytes) and
  fragmentation-invariance (whole-feed output == chunked-feed output).

### Changed

- **Upstream errors are no longer flattened to `502`.** A provider's `429` (rate limit),
  `529` (overloaded), `400` (context length), or `401` (auth) now reaches the client with
  its real status code, type, and message, so clients can decide whether to back off and
  retry. Previously any non-translatable response became a generic gateway failure.
- **A non-200 upstream never becomes a `200` stream.** Streaming is entered only when the
  upstream returns `200` with `text/event-stream`.
- **`created` is injectable** on the SSE translator (defaults to wall clock), making
  output deterministic for tests and letting the gateway align a stream's timestamp with
  the non-streaming path.
- Shared internals extracted to `provider/src/openai_common.hpp`. `created_now()`,
  `anthropic_finish_reason()`, `to_ll()`, and `append_sanitized()`, so the streaming and
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
  CPU. The scan now resumes at the join point, linear regardless of fragmentation.
- **`message_delta` without a `stop_reason`** (Anthropic sends usage-only deltas
  mid-stream) emitted a premature finish chunk.
- Aborted streams are counted as errors instead of served requests.

### Security

- **SSE output is strict.** All passthrough spans (content, `id`, `model`, and the
  upstream error `message`/`type`) have C0 control bytes escaped as `\u00XX` on the way
  out. The hand-rolled JSON parser is deliberately lenient and accepts raw control bytes
  inside strings; relaying them verbatim would emit invalid JSON and could desynchronise a
  strict client's SSE parser.
- **Bounded buffers on every untrusted path**: the SSE translator caps its line
  (1 MiB) and event (4 MiB) buffers with a sticky failure, `ChunkDecoder` rejects absurd
  chunk sizes and size-lines, and the streaming output buffer is capped.
- **Stalled upstreams can no longer hold resources indefinitely** (see idle timeouts).

### Known gaps

Streaming covers **OpenAI ⇄ Anthropic text** only. Tool-call streaming, the reverse
direction, and Gemini / Cohere streaming are not implemented. There is no cap on the
number of concurrent streams a single client may open.

## [0.2.0]. 2026-07-27

- Structured HTTP error responses: `400` / `502` passed through to the client instead of
  a bare connection close.
- Recovery from silently-dead pooled upstream connections (reconnect and retry once).
- Hardened JSON parsing (depth-limited against recursion bombs).
- Build cleanup, optional `-march=native`, and a Clang CI fix for `json::Value`'s
  self-referential special members.

## [0.1.1]. 2026-06-14

- Benchmark chart artifacts stored alongside the website assets.

## [0.1.0]. 2026-06-11

Initial release: the C++20 translator (OpenAI ⇄ Anthropic / Gemini / Cohere,
non-streaming chat completions), the reference gateway proxy (epoll and io_uring),
the benchmark harness, and the test suite.

[Unreleased]: https://github.com/kottos-ai/llmbridge/compare/v0.10.0...HEAD
[0.10.1]: https://github.com/kottos-ai/llmbridge/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/kottos-ai/llmbridge/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/kottos-ai/llmbridge/compare/v0.8.1...v0.9.0
[0.8.1]: https://github.com/kottos-ai/llmbridge/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/kottos-ai/llmbridge/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/kottos-ai/llmbridge/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/kottos-ai/llmbridge/compare/v0.5.2...v0.6.0
[0.5.2]: https://github.com/kottos-ai/llmbridge/compare/v0.5.1...v0.5.2
[0.5.1]: https://github.com/kottos-ai/llmbridge/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/kottos-ai/llmbridge/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/kottos-ai/llmbridge/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/kottos-ai/llmbridge/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/kottos-ai/llmbridge/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/kottos-ai/llmbridge/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/kottos-ai/llmbridge/releases/tag/v0.1.0
