# Changelog

All notable changes to `llmbridge` are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html), with one
pre-1.0 caveat: **the API is unstable until v1.0.0, so breaking changes may land in
minor (0.x) releases.** Breaking changes are always called out explicitly below.


## [0.106.0]. 2026-10-29

Gemini clients routed by model, Vertex as a venue, and a Gemini reply read in its own
words. A non-Gemini client sees no change but two: a `key` query parameter is refused, and
a log line's request line stops at its query. DESIGN.md "Gemini clients and Vertex".

### Added

- **A Gemini client's model is its path's.** `/v1beta/models/{model}:generateContent`
  and Vertex's `.../publishers/google/models/{model}:...` carry no `model` in the body, so
  `RequestFacts::model` was empty and a policy could neither route nor price them. It is
  now the path segment, and `RequestFacts::stream` is `:streamGenerateContent`, both known
  on the head. `Decision::model`, a `Retry`'s and a substitution rewrite that segment,
  never the body, so a body decided on its head still streams with no park.
- **`vertex`**, `Venue::Vertex`: Gemini's body at Google Cloud's publisher path,
  `/v1/publishers/google/models/{model}:generateContent`, which an API key reaches with
  no project. A Gemini API path sent to it is rewritten into that layout, a Vertex
  publisher path sent to a `gemini` venue into `/v1beta/models/`, and a project-scoped
  Vertex path goes as written.
- **`Upstream::auth_bearer`**: a Google venue that takes an OAuth token. A named key goes
  as `Authorization: Bearer` there, and as `x-goog-api-key` everywhere else; a
  translated request sends exactly one, since Google refuses two.
- **What a Gemini reply states in its body**, read by `provider::gemini_facts` from a
  whole reply and from each `data:` line of a stream, a line split across reads joined
  first: `modelVersion` as `served_model`, `responseId` as `venue_req_id`, `serviceTier`
  or Vertex's `trafficType` as `served_tier` (now held to 32 bytes, for
  `PROVISIONED_THROUGHPUT`), and `groundingMetadata.webSearchQueries` as
  `web_search_requests`. `RequestRecord::tool_prompt_input` says URL context or code
  execution ran, so `tool_prompt_tokens` bill as input.
- **`RequestRecord::stream_error`**: a 200 stream that then sent `data: {"error":...}`,
  as Google does when a stream fails after its head; `upstream_error` names its `status`.
- **Gemini usage**: `AUDIO` entries of `promptTokensDetails` and
  `candidatesTokensDetails` as audio in and out, and a `thoughtsTokenCount` left out (3.1
  Pro at thinking level LOW) taken from what `totalTokenCount` holds beyond the rest.

### Changed

- **ABI: rebuild a sink and an embedder against these headers.** `RequestRecord` and
  `Upstream` have new fields, `Venue` a new value and `kSinkCaptureMax` is 3. Source
  compatible.
- **Gemini stream stamps.** The first token is a part not marked `"thought": true` with
  non-empty `text`, a `functionCall`, `inlineData` (an audio or image stream) or
  `executableCode`; one event holding a thought and then text stamps both; an empty part carrying only a
  `thoughtSignature` is not. Thinking is a part marked `"thought": true`. Before, a
  Gemini stream never stamped either: `"content": {` is no content string.
- **`scan_error_type`** reads Google's `error.status` when the first 512 bytes name no
  `code` or `type`, within 8 KiB, past a long `message`. Google's numeric `code` is
  still not a name.
- **`RequestRecord::comment_unsafe`: the client breaks on a reply note**, and the sink
  decides whether to send one. Measured 2026-10-29 against a stream the gateway writes:
  google-genai 2.34.0 for Python, sync and async, fails it with `UnknownApiResponseError`
  (it parses every line that is not `data: ` as JSON, a comment line included), while
  @google/genai 2.32.0 for JS reads it. The Python SDK names itself in
  `x-goog-api-client` (`google-genai-sdk/2.34.0 gl-python/3.13.17`, JS `gl-node/`), so a
  Gemini client whose header carries a `gl-python/` token is marked. llmbridge writes
  whatever the sink returns, as for every other client.
- **`kSinkCaptureMax` is 3** (was 2), for a header a sink reads to choose its note.
- **Declined attempts are reported as Anthropic bills them.** `prior_*` held every attempt
  declined before a server-side fallback; Anthropic bills one that produced output, and one
  that declined before any output only in the `bio`, `frontier_llm` and
  `reasoning_extraction` categories. Each attempt is now judged alone, its category read from
  the `fallback` content block that marks its switch (`trigger.category`), in a whole reply
  or a stream, a block split across reads included: `prior_*` holds the billed ones,
  `prior_attempts` every one, `RequestRecord::prior_billed_attempts` those counted, and
  `prior_unverified_attempts` those no block named, left out. `openai::refusal_billed`,
  `scan_refusal_triggers` and `bill_declined` are the rule; `StreamUsage::usage()` returns
  its counts so judged, by value. DESIGN.md "Declined attempts".

### Security

- **Refused before any venue hears of it**, on both backends, with a fixed message and
  the target never logged: a `key` or `access_token` query parameter, whatever the
  dialect, its name read as a server reads it (percent-decoded, in any case, `&` or `;`
  apart, so `?%6Bey=` and `?KEY=` are refused too, and never sent beside a named key); the
  Gemini Interactions API (`/v1beta/interactions`); a `:streamGenerateContent` without
  `alt=sse`, whose single JSON array would be held to its end; and a path model outside
  `[A-Za-z0-9._-]`, 1 to 255 bytes (`provider::path_model_ok`). A policy's model that
  cannot stand in the path, or for a target naming none, is a 500, every hedge leg's
  checked before any leg is sent. An OAuth (`auth_bearer`) Vertex venue is sent only a
  project-scoped path: the publisher path is express mode's, which takes an API key alone. A log line's request line now stops at its query.

### Tests

- `ProxyGeminiPath.*` and `ProxyVertex.*` on both backends; `GeminiFacts`,
  `GeminiUsage`, `ScanErrorType`, `GeminiPathModel` in the translator tests.

## [0.105.0]. 2026-10-26

A sink can tell the client something the record knows only at the end of a stream, such
as what the request cost. Output is unchanged without a sink that overrides the new hook.
GATEWAY-INTERNALS.md 6d.

### Added

- **`RequestSink::reply_note`**, a virtual whose default returns nothing, and
  `kReplyNoteMax` (1024), in `gateway/sink.hpp`. At the clean end of a streamed reply the
  gateway builds the request's record as it stands (status 200, `streamed`) and asks the
  sink; a non-empty answer is written as one SSE comment line, `: <note>` and a blank
  line, after the last event (the venue's `[DONE]` or `message_stop`, or the translator's
  `[DONE]`) and before the terminating chunk, in a chunk of its own. `on_request` follows
  at the finalize as before. A parser that follows the SSE specification ignores a
  comment line, so a client that does not look for the note reads the stream it read
  before.
- **Only a clean end a client reads gets one**: never a truncated, corrupt or failed
  stream, a whole reply, or a hedge leg that lost. A hedged stream asks once, after the
  race, with the serving leg's record.

### Security

- **A note cannot become an event.** One over `kReplyNoteMax`, or holding a CR, LF or
  NUL, is dropped whole, never cut or cleaned. So is a note after a byte-forwarded venue's
  stream that stopped inside an event, which the note would have joined: a close-delimited
  venue's EOF is always a clean end, mid-event included. Each drop is a WARN with the
  reason and the length, never the text.

### Changed

- **The terminating chunk is staged by `stream_flush`**, once, after a hedge has chosen
  its leg, and no longer by `stream_step`: that is what puts both stream kinds and both
  backends through one note path. The bytes are the same. A stock build pays one
  predicted-false branch per stream and two byte stores per byte-forwarded read; a sink,
  one record built and one virtual call per stream. `stream_pump`'s cap check now runs
  before the terminator is staged, so its five bytes, and a note, are not counted against
  io_uring's 8 MiB cap.
- **ABI: rebuild a sink against this header.** `RequestSink` has a new virtual. Source
  compatible.

### Tests

- `ProxyReplyNote.*` on both backends: the note after a byte-forwarded venue's `[DONE]`
  and after the translator's, in its own chunk before the terminator, unframed and last on
  a client that asked to close, with the record's final token counts and `reply_note`
  before `on_request`; no sink and a sink that keeps the default leaving the stream as it
  was; no note, and no call, on a cut, corrupt or failed stream or a whole reply; a note
  with a CR, LF, CRLF or NUL, or one byte over the cap, dropped whole, and one exactly at
  the cap written; a venue stream that stopped inside an event left as it was, and one that
  ended on a blank line in CRLF, CR or mixed line endings given its note; a hedge's
  one note from the leg that served, whether it won in the read that ended it or earlier,
  and none from the leg that lost. Two mutations each fail the test that guards them and
  no other: without the length and CR, LF and NUL checks every one of the five notes
  reaches the client, on both backends, and without the event check both venue streams do.

## [0.104.1]. 2026-10-23

### Fixed

- **A reply that reasons in a field named `reasoning` is stamped.** Together streams
  gpt-oss reasoning as `"reasoning":"We"`, with `"content":""` beside it and
  `"reasoning":null` where the model did not reason, as captured from api.together.xyz.

## [0.104.0]. 2026-10-23

### Fixed

- **A request that never completed is recorded.** When a connection closed after the
  request was dispatched and before the reply finished, the gateway closed both sides and
  wrote nothing to the sink, so the slowest requests, the ones a client gave up on or the
  gateway cut, were missing from every record set. `forget()`, which every close passes
  through, now records them once: **499** (the code nginx uses for a client that closed the
  request) with the stamps reached so far and `ts_done` at the close, a stream the gateway
  cut stays a truncated 200 as at its normal end, and a reply already complete upstream
  keeps its own status. `streamed` is set once the SSE head has gone out. A hedged request
  is still recorded leg by leg.

## [0.103.0]. 2026-10-17

A hedge is now a race: the first leg with an answer serves the client, whichever venue it
went to, and 0.102.0's rule that the decision's own leg always served is gone.
GATEWAY-INTERNALS.md 13.

### Changed

- **The first leg to answer wins.** Every leg, the decision's own included, is a request of
  its own, and the client sends none until one wins: a stream with its first token or
  thinking delta, never its head, or a whole 2xx reply. The winner's request state, staged
  bytes, venue connection and SSE translator move to the client mid-flight, and every other
  leg is reset as before. In 0.102.0 the decision's leg served the client, and its stream
  head reached the client before any leg had a token.
- **A failure waits for the others.** A leg's non-2xx reply, or a stream that ends without a
  token, is recorded and closed while another leg may still answer. The last leg to fail
  answers the client with its own status and body. A last leg that ends with nothing to
  show (aborted mid-stream, or closed by the sweep) gets the client the gateway's 502.
- **The decision's own leg has its own seq**, like every leg: `hedge_group` (the client
  request's seq) is on no leg's record, and the client's own state leaves none. A client
  leaving mid-request now cuts and records every leg, the decision's included.
- **`Stats::hedge_legs` counts every leg**, the decision's own included. Only the leg a
  client got counts in `requests` or `errors`.

### Tests

- `ProxyHedge.*`: the fastest leg serving the client while the decision's own is reset; a
  stream winning at its first token, the losing head never reaching the client; a winner's
  head held from its arrival to its token; the last failure answering the client, with
  every failure recorded; a failure waiting for a success; every venue closing unanswered;
  a request pipelined behind a hedge; an Anthropic client's legs to two Anthropic-dialect
  venues, each with its own `x-api-key`; a translated stream winning with its translator
  mid-stream; a leg to a SigV4 venue failing alone and reaching nothing.
  `GatewayCrossVenueTls.EachHedgeLegIsVerifiedAgainstItsOwnVenue`: a leg to a venue
  presenting the wrong certificate fails, sends nothing, and the verified leg serves.
  Seven mutations each fail the tests that guard them: only leg 0 allowed to win, a head
  winning, a failure winning, a held head dropped, staged bytes left behind, the translator
  left behind, and the decision's leg not reset.

## [0.102.0]. 2026-10-17

A request can go to several venues at once. The decision's own venue serves the client, and
every other leg still waiting when it answers is reset; each leg is a request of its own,
with its own key and its own record. GATEWAY-INTERNALS.md 13.

### Added

- **`Decision::hedge` and `Decision::hedge_legs`**, appended last: up to `kMaxHedgeLegs` (3)
  more venues, each a `HedgeLeg` with its own `upstream_index`, `model`, `service_tier`,
  `speed`, `inference_geo`, `anthropic_beta` and a required `credential_header`. Every leg
  is sent the same request at once, rebuilt for its venue with its own key and no other.
  The body is held to its last byte first, on a decision taken on the head too, and a
  confirm that adds a hedge to a body already streaming redoes it held.
- **The legs are cut once the decision's own leg has its answer**: a whole reply, a
  stream's first token or thinking delta (not its head), or the client leaving. A cut leg's
  venue is reset (`SO_LINGER` 0) and never pooled, the only way HTTP/1.1 tells a venue to
  stop generating. A leg that answers first runs to its end, its reply dropped.
- **`RequestRecord::hedged`, `hedge_group`, `hedge_leg`, `hedge_won` and `hedge_cut`**,
  appended: one record per leg, each `hedged` and with its own `seq`, grouped by the client
  request's seq. A process's first request has seq 0, so `hedged`, never the group, marks a
  hedge. A cut leg has status 0 and no token counts, since its usage never arrived.
- **`Stats::hedged_requests`, `hedge_legs` and `hedge_legs_cut`**. No leg counts in
  `requests` or `errors`.

### Security

- **No leg goes without a key of its own.** A hedge whose decision or any leg names no
  `credential_header` is a 500 before any venue is contacted: the client's own key would
  otherwise reach every leg's venue, companies it was never meant for. So is a leg past the table,
  which never falls back to venue 0, and an over-long override or a beta that is not a
  token. Tested with three venues and three keys, each venue receiving its own once and
  neither other.
- **A leg is never failed over**: the policy hears its failure with `retryable` false.

### Changed

- **ABI: rebuild a policy and a sink against these headers.** `Decision` and
  `RequestRecord` grew. Source compatible: existing designated initializers are unchanged.

### Tests

- `ProxyHedge.*` on both backends: each leg's key alone at its venue; the losing legs reset
  and unpooled, counted, and no leg in `requests`; one record per leg in one group, cut
  legs with status 0 and no tokens; a stream cutting at its first token; nine malformed
  hedges each a 500 that reaches no venue; a failing leg heard and never failed over; a
  body decided on its head reaching every leg whole and none before; a client leaving
  cutting every leg. Five mutations each fail the tests that guard them: a leg sending the
  decision's key, a cut without a reset, failover left on, the body not held, and no cut
  when the client leaves.

## [0.101.0]. 2026-10-15

A policy hears what each venue answered, not only which ones failed: a venue's own 503 is
relayed to the client as sent and never reached `on_failure`, so a policy ejecting venues
on their 5xx replies could not see them, and the client's status cannot tell a venue's 502
from the gateway's after a failure already counted. DESIGN.md "When a venue answers".

### Added

- **`Policy::on_reply(const ReplyFacts&) noexcept`**, appended last in `Policy`, default a
  no-op. `ReplyFacts` carries the venue (`upstream_index`), the `status` it sent, `attempt`,
  `tag` and `seq` as `FailureFacts` carries them, and `streamed`. Once per venue response,
  on the loop thread, before any of it reaches the client, and it cannot change the reply:
  a whole response once it is whole, byte-forwarded or translated (a translated venue's
  error keeps its own status here, as it does for the client); a 200 event stream at its
  head; after a failover, only the venue that answered, with its attempt. A head whose body
  cannot be framed (close-delimited, a broken chunk, over the cap) is reported with the
  venue's status while the client gets the gateway's 502. Never a status the gateway made
  up, and a response cut short before it was whole goes to `on_failure` instead, so each
  attempt reaches at most one of the two hooks; a head the gateway refuses (a bad status
  line, both framings, a length over the cap) and an interim 1xx reach neither. Not for a
  reply to a client already gone, whose venue connection is closed unread. Without a policy
  nothing is called; with one, the cost is one virtual call per response.

### Changed

- **ABI: rebuild a policy against this header.** A new virtual changes `Policy`'s vtable, so a
  policy compiled against 0.100.0 must not be linked into 0.101.0. Source compatible: no
  existing override changes.

## [0.100.0]. 2026-10-14

The rest of what each venue states about a request's usage, read in the passes that
already run and handed to the sink: reasoning counts the parser missed, Anthropic's other
attempts and server-tool counts, what speed, region and tier actually served, per-call
search tools, and the venue's own price. Checked against each seller's documentation, not
against live traffic.

### Added

- **`RequestRecord` fields, appended**: `image_in_tokens`; `web_search_requests`,
  `web_fetch_requests`, `code_exec_requests` and `file_search_calls`, server tools billed
  per call; `venue_cost_pdollars`, the venue's own price in picodollars (DeepInfra's
  `estimated_cost`, `1e-05` exponents included); the compaction and declined-attempt
  counts of Anthropic's `usage.iterations` with `prior_model` and `prior_attempts`;
  `served_speed`, `served_geo` and `served_latency`; `asked_cache_ttl`. Each is -1 or
  empty when the venue stated none.
- **Anthropic's `usage.iterations`.** A compaction pass is billed beside top-level counts
  that leave it out, and an attempt declined before a server-side fallback is billed at its
  own model's rate. Both are read into their own fields; a `message` entry with no
  `fallback_message` beside it is the served attempt and is not counted twice.
- **Anthropic's `usage.server_tool_use`, `usage.speed` and `usage.inference_geo`**, the
  speed and region the venue served, which may differ from what was asked.
- **The Responses API's search calls**, which its usage never states: counted from a
  body's `web_search_call` and `file_search_call` items, or a stream's `.completed` events
  (once per call; each item is named three times), a name split across reads counted once.
  Only on `POST /v1/responses`, decided once at framing from the request line.
- **Bedrock's `X-Amzn-Bedrock-Service-Tier` and `X-Amzn-Bedrock-PerformanceConfig-Latency`
  response headers**, the only place Bedrock states what served. Read in the head's one
  header walk; the request-side header classification is unchanged. The tier fills
  `served_tier`, and a response without it clears an earlier attempt's.
- **`TopLevelFacts::cache_ttl`**, `prompt_cache_options.ttl` from the same top-level walk.
  DeepInfra prices a five-minute and a one-hour write differently and states neither in
  its reply.
- **`openai::usage_of`, `openai::merge_stated` and `openai::count_search_calls`**, the
  shared usage reader, the merge a stream's restated usage needs, and the search-call
  counter. `StreamUsage::reset` takes whether to count search calls.

### Changed

- **Reasoning counts that were dropped are read**: Anthropic's
  `output_tokens_details.thinking_tokens` (Claude on Bedrock too) and the Responses API's
  `output_tokens_details.reasoning_tokens`. `reasoning_tokens` was always -1 for Claude.
- **Bedrock Mantle's Responses cache writes** (`input_tokens_details.cache_write_tokens`)
  are a write, not plain input, and **Together's flat `usage.cached_tokens`** (Llama 3.3
  70B Turbo and other non-reasoning models) is a cache hit, not zero.
- **A stream's prompt counts keep the largest statement.** Anthropic restates input and
  cache counts cumulatively on `message_delta` once server tools have run (its web-search
  example grows from 2,679 to 10,682), and the first statement used to win. The cache
  split stays message_start's, which alone carries it.
- **Behaviour change: the OpenAI-in translator reads usage with the shared parser**, on the
  stream and on a whole reply. A client speaking OpenAI to an Anthropic venue now gets
  `completion_tokens_details.reasoning_tokens` when Claude thought, and a streamed input
  count that includes what server tools added.
- **Behaviour change: a repeated or escaped key inside `prompt_cache_options` is refused**
  where a sink or policy reads the body, as one inside `stream_options` already was: our
  reader takes the first `ttl` and a venue may take the last.
- **`kUsageWindow` is 4096 bytes**, from 2048: `usage.iterations` can triple a usage block.

## [0.99.1]. 2026-10-13

Logging only, from an audit of the 0.99.0 paths: what fails now says why, and what repeats
says it once. No behaviour or API change beyond the one appended counter.

### Added

- **`Stats::upstream_local_failures`**, appended: connects this host could not make (out of
  descriptors, memory, a full submission queue or epoll set), each answered 503. The exit
  profile and the SIGUSR1 dump print it beside `upstream_unresolved`.
- **A boot WARN for a venue that can never be dialled**: an `ip` that is not an IPv4
  address and no `host` to resolve. Every request to it is refused, and before this only
  the refusals said so.
- **A WARN when an upstream TLS session cannot be set up** (out of memory, or an `sni_host`
  OpenSSL refuses), with OpenSSL's reason and the SNI name. The request went on as a
  failed connect with nothing saying it was TLS.
- **An INFO when the engine shuts down with a name lookup still running**, before it
  leaves the resolver thread to finish on its own.

### Changed

- **A 503 for this host's want names the errno.** "no upstream (gateway out of resources)"
  now reads, for example, "(gateway out of resources: EMFILE, the process descriptor
  limit)": EMFILE, ENFILE, ENOBUFS, ENOMEM, ENOSPC and EAGAIN are each raised in a
  different place. The same reason on acquire, a connect's completion and a stale resend.
- **A 5xx past dispatch names its venue**, `venue=N` at the end of the reply line.
- **A 504 for a client that stopped reading its reply says so**: "client stalled, not
  reading", where it read "upstream idle timeout" right after the correct TIMEOUT line.
- **A lookup that finds the addresses a venue already has is DEBUG**, not INFO: a venue
  refusing connects is looked up again every second, and each said nothing new.
- **A resolver job that cannot be queued** (no thread, no memory) names the exception and
  whether the venue keeps an address, backs off as a failed lookup does, and logs once
  per outage instead of once a second per venue.
- **DESIGN.md "Logging"** states the rule the code has followed since 0.34.0: every reply
  of the gateway's own is WARN, a client's 4xx included. It still said 4xx was DEBUG.

## [0.99.0]. 2026-10-13

A policy hears of every venue that failed while the client was sent nothing, and can key
its own state on the request: `FailureFacts::seq`, and `retryable` false where the gateway
cannot redo the request but the failure still counts. A first attempt whose connect fails
at once fails over, a failure of this host is a 503 that blames no venue, and a venue given
by its name alone is resolved off the loop instead of refused until a restart. DESIGN.md
"Venue addresses" and "When a venue fails", GATEWAY-INTERNALS.md 3c, 3d, 4 and 8.

### Added

- **`FailureFacts::seq`**, appended: the request's sequencer, the value `RequestFacts::seq`
  and `RequestRecord::seq` carry, so a policy keys its own per-request state (the venues
  already tried, its ranking) on it. The first request in a process has seq 0.
- **`FailureFacts::retryable`**, appended, default `true`: whether the gateway acts on the
  `Retry`. False with a one-venue table, once a response byte is in (the venue may have
  acted, so a re-send could run the request twice), and for the third venue of a chain.
- **A venue given by `host` alone, with no address yet.** `run()` asks the resolver for it,
  the sweep asks again as its backoff allows, traffic or not, and an acquire refused for
  want of an address asks too. Until an answer arrives the venue is refused as
  "no upstream (venue unresolved)", which a policy fails over like any venue that cannot
  be reached. The stock binary still refuses a name that does not resolve at startup.
- **`Stats::upstream_unresolved`**, appended: acquires refused because the venue's name had
  no address yet. The exit profile and the SIGUSR1 dump print it beside `upstream_reresolved`.

### Changed

- **Behaviour change: `on_failure` is called for venue failures it never saw.** A
  one-venue table, a venue that failed after a response byte arrived (EOF, silence), and
  the third venue of a chain now reach the hook with `retryable` false, and whatever it
  returns is ignored. A policy counting failures toward ejection counts them all; one that
  logs a failover should check `retryable` first. Still never reported: a reply already
  begun (a stream's head, an interim `100 Continue` owed), a parked body no venue holds.
- **Behaviour change: a connect that fails for want of a local resource is a 503.** Out
  of descriptors or memory (`EMFILE`, `ENFILE`, `ENOBUFS`, `ENOMEM`, `EAGAIN`), or a full
  io_uring submission queue or epoll set (`ENOSPC`), whether `connect()` says so at once or
  its completion does (`Engine::on_connect_error`, one rule for both backends), and for a
  stale keep-alive's resend too: the client gets 503, nothing fails over and the policy
  hears nothing. It was a 502, and the policy counted it against a venue that was not at
  fault. No free local port (`EADDRNOTAVAIL`) stays the venue's: Linux picks a port per
  destination, so another venue may still have one.
- **A client the idle sweep finds stalled is answered 504 directly**, never through the
  policy: one owing only TLS records used to reach `on_failure` and could be failed over.
- **Re-resolution is rate-limited.** One venue's name is looked up at most once a second;
  a name that keeps failing waits twice as long each time, up to 30 s. The failure is
  logged once per outage, with the resolver's own error.
- **The bare `IP:PORT` form's Host header follows the address in use**, after a failed
  connect moves the venue or a lookup answers; it kept the boot address.
- **Destroying a `Gateway` never waits on a lookup in flight.** The resolver thread shares
  only its own state, so a lookup that would take the resolver's timeouts while DNS is
  down is detached and finishes on that state.
- `IoOps::connect` (internal) returns 0 or the errno that stopped it, not a bool.

### Fixed

- **A first attempt whose connect fails at once fails over.** epoll learns an unreachable
  network inside `connect()`, before the request had been kept for a failover, so the
  client got a 502 and the policy was never asked; io_uring, learning it from a completion,
  failed over. A venue whose address does not parse fails over the same way.
- **A connect error known at once moves the venue to its next address and asks for its
  name again**, as the same error arriving later as an event did.
- **epoll: a complete answer read together with the venue's close is served.** A reply of
  a multiple of the 16 KiB read with its FIN in the same segment was a 502 "upstream EOF",
  and under this release's reporting would have been a failure counted against a venue
  that answered. The bytes are framed first now. io_uring was not affected.
- **A lookup the resolver thread could not finish is asked again.** Out of memory on that
  thread left the venue marked in flight forever, so its name was never looked up again.
- **epoll: a socket the epoll set would not take is not a connect started.** The failed
  `EPOLL_CTL_ADD` was ignored, so the connect never completed and the request waited out
  the connect deadline, then blamed the venue.
- **A resolver thread that could not be started or queued is asked again a second later**,
  not at every sweep with a warning each time.
- **An empty string_view in a log line adds nothing**, where it passed a null pointer to
  `memcpy`, which is undefined even for zero bytes; UBSan reports it on any field a caller
  leaves empty.

### Tests

- `CoreFailover.*`: a first attempt failing over (synchronous connect error, no address,
  no free local port), the local errors answered 503 on a first attempt, on a failover, in
  a connect's completion and on a stale resend, the sequencer (past 0) and tag carried, the
  third venue and a one-venue table reported unretryable, a failure after
  a response byte reported unretryable, nothing reported for a reply begun or a parked
  body, and a client stalled on TLS records answered without the policy.
- `CoreResolve.*`: a name-only venue resolved off the loop then dialled, every venue with
  a name and no address asked for, the backoff doubling and resetting, the sweep asking
  again and installing the answer, the Host following
  the address, and the engine never waiting on a lookup in flight.
- `ProxyRoute`, both backends: `AFirstVenueWhoseConnectFailsAtOnceFailsOver` (failed on
  epoll before), `AFirstVenueWithNoAddressFailsOver`, `TheThirdVenueOfAChainIsReportedButNotRetried`,
  `AVenueThatClosesMidResponseIsReportedButNotRetried`, `AVenueGivenOnlyANameServesOnceItResolves`,
  `AVenueGivenOnlyANameIsResolvedWithNoRequestAtAll` and
  `AnAnswerReadTogetherWithTheVenuesCloseIsServed` (failed on epoll before).
- Each behaviour fails its test when removed: the first-attempt save, the local
  classification (at once and in a completion), the unretryable report, the sequencer, the
  stalled-client branch, the acquire's ask, the start and sweep asks, the backoff, the Host
  recomputed with the address, and the epoll close behind an answer.

### Known gaps

- An IPv6-only name never resolves (the resolver asks for IPv4); with the backoff it costs
  one lookup every 30 s and one log line per outage.
- Each worker's engine has its own resolver, so N workers each look up an unresolved name.
- One resolver thread per engine looks names up one at a time: a lookup that hangs while
  DNS is down delays the next venue's by the resolver's timeouts.
- A venue that fails after the SSE head reached the client is outside the seam: the
  request record says the stream was truncated, and no hook is called.


## [0.98.0]. 2026-10-13

A body decided on its head no longer parks when its decision sets `service_tier`, `speed`
or `inference_geo`: the members are spliced into the body as it streams, with any model
substitution, so mixes of flex and standard tiers, fast mode and US-only inference
stream. GATEWAY-INTERNALS.md 12e, DESIGN.md "The policy seam".

### Added

- **`service_tier`, `speed` and `inference_geo` spliced into a streamed body.** On a head
  decision with no `model` override and no `hold`, a JSON body to a venue that
  byte-forwards streams with them inserted right after the root's `{`, in the text the held
  `apply_overrides` writes for absent keys (`"service_tier":"flex","speed":"fast"`, that
  order), then `,`, or a space when the object is empty or its first member is one the
  splice blanks. The declared Content-Length is the client's plus the members' and their
  separator, plus a substitution's growth (0.96.0): one stream, both deltas.
  - **The client's own member.** One by the same name goes as spaces, from the separator
    before it (the client's comma, or the one after our members) through its value, so the
    policy's value wins and the venue never reads two, whether the member is first, in the
    middle or last, with any whitespace. No placement needs a redo. The venue gets the held
    rewrite's members and values, the client's member blanked rather than replaced where
    it stood; with none of the client's own in a non-empty object, the held rewrite byte
    for byte.
  - **What the scan holds.** BodyScan, reset with a members mask
    (`reset(all_keys, track_model, members)`, `BodyScan::kServiceTier`, `kSpeed`,
    `kInferenceGeo`), holds what follows the root's `{` until the first key or the empty
    object's `}`, and each top-level member from the separator before it until its key
    closes, a member the mask names until its value closes, then tells its span
    (`open_at`, `has_member`, `members`, `member_count`). It refuses a root that is not an
    object, a non-string value under a member it replaces and a repeat of one, which the
    held rewrite refuses too: a 400, the venue reset short of the body.
  - A value the splice cannot write as it is (a control byte, DEL, quote or backslash)
    parks the body, which the held rewrite then refuses, as before.
  - `detail::MemberSplice` and `detail::Splice` (request.hpp) build the venue's bytes.
- **`RequestFacts::service_tier_spliced`, `speed_spliced` and `inference_geo_spliced`**,
  appended: at `confirm()`, the streaming body went with that member spliced in.
- **`RequestRecord::service_tier_spliced`, `speed_spliced` and `inference_geo_spliced`**,
  appended: at a confirm that kept the body. `asked_tier` is the client's own tier the
  splice blanked, as a held rewrite records the one it replaced.
- **`Stats::streamed_override_spliced`** (kept splices) and **`streamed_override_redone`**
  (confirms that redid a spliced body a venue held part of, also in
  `streamed_body_redone`), appended. The exit profile and the SIGUSR1 dump print them.

### Changed

- **`confirm()` judges a keep on what the venue gets.** On a spliced body a decision that
  names no tier (or speed, or geo) keeps when the client's own equals the one spliced; one
  that changes a value, or drops it where the client sent another, redoes the request.
- **A failover while the body streams** resends it from its first byte with the `Retry`'s
  own `service_tier`, `speed` and `inference_geo` spliced, when the head decision set
  each of them too; a `Retry` naming one the head decision did not parks the body, as for
  `model`. A stale connection's retry rebuilds the spliced head and bytes.
- A body parked by a failover no longer reports a splice at its confirm: the
  substitution flag of the abandoned attempt is cleared (`model_substituted` read true
  for a parked body whose head decision had substituted).
- Translated venues, bodies not sent as JSON, a `model` override and `hold` still park.
- A policy without the opt-in, a body held to its last byte, and a head decision with no
  member override are unchanged: the same bytes, headers, records and stats. The sink
  corpus records match 0.97.0's. Instructions per request (callgrind, 3000 requests, one
  worker) are within 0.03% of 0.97.0's, on epoll 17,353 against 17,356 and on io_uring
  17,408 against 17,413, once the splice moved to its own file, splice.cpp: in
  request.cpp it changed how the byte-forward rebuild was compiled, +1.2%. The
  allocation ceilings are unchanged.

### Tests

- `BodyScan.*`: a replaced member held with the separator before it, first, in the
  middle and last, with whitespace, in every piece size; the root's `{` waiting for the
  first key and an empty object's `}`; every member in body order with a model tracked
  between them; a non-object root, a non-string value, a repeat and an escaped key refused.
- `ProxyHeadDecide.*`, both backends, with a policy that opts in: the openai-python shape
  with "auto" and a flex tier streaming with both spliced, the venue body the held
  rewrite byte for byte under the declared length, no park and no redo; `speed` and
  `inference_geo` into the anthropic-python shape at an Anthropic venue with the beta
  token; an empty object with whitespace before its `{`; a client `service_tier` first, in
  the middle and last, with and without whitespace, blanked, the held rewrite's members
  and values, `asked_tier` the client's; a client tier equal to the policy's going once,
  and a confirm naming none keeping; a confirm dropping or changing the tier redoing the
  request; a failover splicing the retry's tier and substitution; a failover naming a
  member the head did not parking; a failover after the whole spliced body sending it
  rewritten whole, the record saying no splice; a stale retry re-splicing; a non-string client
  member refused before it leaves; a value the splice cannot write parking; a `speed`
  override on a body not sent as JSON parking (`ASpeedOverrideParksTheBody`, now that a
  JSON body streams).
- `fuzz_body_stream` splices members too, with the first byte's bits 7 and 3 (bits 1 and
  2 pick one of four sets, alongside any model pair): the bytes sent are the declared
  length, valid JSON with no repeated member or model, exactly our members after the `{`
  and each replaced member blanked from its separator, and the held rewrite's members and
  values; with none of the client's own, the held rewrite but for our spaces. Seeds for
  the SDK shape, a client member first, in the middle, last with whitespace and alone, an
  empty object, an Anthropic body, a non-string member and a non-object root.
- Each behaviour fails its test when removed, on both backends: the comma held with the
  member after it, the `{` held, a member held to its value, each refusal, the empty
  object's release, the separator, the blanking, the insertion, streaming at the head and
  its JSON and plain-value checks, the failover's subset check, the held rewrite skipped
  while streaming, the splice flag, the declared growth, the keep judged on what the venue
  gets, both counters, `asked_tier`, the confirm's facts, the sink fields, the failover
  park clearing the splice, the failover re-splice, the scan's tracking, the reset on a
  whole rewrite and the stale retry's re-splice. The `{` and the member holds show in
  `BodyScan`'s tests only, since a gateway test's reads seldom split there;
  `fuzz_body_stream` finds a comma where a space belongs within seconds.

### Known gaps

- The held rewrite parses with unique keys at every depth and refuses a body repeating a
  key inside a nested object; spliced, as substituted since 0.96.0, that body streams.
- `speed` and `inference_geo` still reach a byte-forwarded body only (0.97.0).


## [0.97.0]. 2026-10-12

A policy can set two more Anthropic request options the way it sets `service_tier`:
fast mode (`"speed": "fast"` plus the `anthropic-beta: fast-mode-2026-02-01` header) and
US-only inference (`"inference_geo": "us"`). DESIGN.md "Body edits and top-level readers".

### Added

- **`Decision::speed` and `Decision::inference_geo`**, at most 32 bytes each, set as
  top-level body keys, replaced when the client sent one and inserted when it did not.
  Byte-forward only, as `service_tier` is: a translated request is built by the
  translator and carries neither. Empty leaves the body alone. A longer value is a 500;
  a quote, backslash or control byte in one is a 400, as for the tier.
- **`Decision::anthropic_beta`**: one beta token, such as `fast-mode-2026-02-01`, sent as
  an `anthropic-beta` header to a venue whose auth is `x-api-key` (Anthropic) and to no
  other venue. It must pass the new `beta_token_ok` (1 to `kMaxBetaToken` = 64 bytes of
  `[a-z0-9._-]`), public so a policy can `static_assert` its own; anything else is a 500
  and reaches no venue. Empty adds nothing.
  - On a byte-forward the client's own `anthropic-beta` value and the token go as one
    line, `<client value>,<token>`, or the client's line unchanged when it already names
    the token. The client's value must pass `header_value_safe`, and two client
    `anthropic-beta` lines are refused: both are a 400 before any venue is contacted.
    A stripped `anthropic-beta`, or one a `Connection` line names, is treated as absent.
  - A translated request has never carried the client's `anthropic-beta` (it is rebuilt
    from the credential lines alone), and still does not: it gets the token alone.
  - Without a token the client's `anthropic-beta` is forwarded exactly as before.
- **`Retry::speed`, `Retry::inference_geo` and `Retry::anthropic_beta`**: a failover
  sends the new listing's values, never the failed one's; empty sends none. A retry
  naming a value too long or a bad token is not taken, and the client gets the failure.
- `Policy::confirm()` compares all three: a change in any of them redoes the request.
- **`provider::Overrides` and `apply_overrides(body, Overrides, had_tier, out)`**: model,
  service tier, speed and inference geo in one parse and one copy. Absent keys share
  one insertion after the opening brace. The model-and-tier forms remain and call it.
- `net::http::Field::AnthropicBeta`, at the end of the enum, so `classify` names the
  header in the walk that already reads every line.

### Changed

- **Behaviour change: inserting a key beside an escaped top-level key is refused.** The
  parser keeps keys as written, so `sp\u0065ed` was not found as `speed` (or
  `service_tier` as itself), and the insert handed the provider two spellings of one
  key. The route override is refused with a 400 instead. A body without an escaped
  top-level key is unaffected.
- A body decided on its head parks, as with a model or tier override, when `speed` or
  `inference_geo` is set; the beta token alone is a head change and lets it stream,
  through `forward_parts`, so the head that goes ahead of a streamed or spliced body
  (0.96.0's model substitution) and its stale retry carry it too.
- Walks per request are unchanged: one header walk at framing, which now also records
  the first `anthropic-beta` span and a count of its lines; one copy walk in the
  byte-forward rebuild, which drops the client's line in the same pass; and one parse
  plus one copy of the body when any body override is set, none otherwise, as before.

### Tests

- `ApplyOverrides.*`: `speed` and `inference_geo` inserted when absent and replaced when
  present; all four overrides in one call with the tier's previous value reported; a
  key quoted inside a string left alone; a quote, backslash, CR, LF, 0x7F or another
  control byte refused in either value; a non-string value already there refused; an
  insert beside an escaped key refused; the model-and-tier form unchanged.
- `ProxyRoute.*`, both backends: an Anthropic venue receiving `"speed":"fast"`,
  `"inference_geo":"us"` and one `anthropic-beta: fast-mode-2026-02-01` line; the token
  joined to the client's value in one line, and not added twice when already there; an
  OpenAI venue getting no `anthropic-beta`; a translated request carrying the token and
  not the client's value; a failover to a listing without options sending none of the
  three, and one with its own options sending those and not the first listing's; a
  client `anthropic-beta` holding a CR, an LF, an 8-bit byte or a control byte, or
  sent twice, refused with a 400 and nothing reaching the venue; a bad token or a 33-byte
  speed a 500 with nothing reaching the venue.
- `ProxyHeadDecide.*`, both backends: a head-decided body to an Anthropic venue streams
  with the token joined to the client's line, a spliced body carries it with the grown
  length declared, and a `speed` override parks the body and sends it whole with the
  field. Removing the token from `forward_parts` fails the first two.
- `WalkHeaders.NamesTheHopByHopAndCredentialFields` names `anthropic-beta`.
- Each behaviour fails its test when removed: the merged line, the venue check, the
  failover reset of `speed` and of the token, the client value check, the repeat check,
  the already-present check, the token check, and the escaped-key refusal.

### Known gaps

- `speed` and `inference_geo` reach a byte-forwarded body only. An OpenAI client of an
  Anthropic venue gets the beta header without the field, which Anthropic serves at
  standard speed. Carrying them through the translator is a follow-up.
- The sink does not yet record the `usage.speed` and `usage.inference_geo` a venue
  reports, so a fast-mode request cannot be told from a standard one after the fact.
- Without a policy token the client's `anthropic-beta` is forwarded on a byte-forward as
  it always was: the framing walk refuses a bare CR or LF, and 8-bit bytes pass.


## [0.95.0]. 2026-10-12

A policy may now decide on the head of a large request body and let it go to the venue as
it arrives, instead of holding it to its last byte: "decide on the head, confirm before the
last byte". Opt-in; a policy that does not opt in behaves exactly as before.
GATEWAY-INTERNALS.md 12c, DESIGN.md "Deciding on the head of a body".

### Added

- **`Policy::decides_on_head()`**, false by default, read once at construction. True, a
  body that would stream without a policy (16 KiB or more, still arriving at its head, not
  encoded, `--stream-body on`, and for an OpenAI client `--stream-openai-body on`) is
  decided on its head, provided one venue in the table byte-forwards the client's dialect
  and the worker streams fewer than `--stream-body-max` bodies; otherwise it is decided
  whole, as before.
  - `decide()` gets `RequestFacts::body_whole` false: `model` is a top-level `model` that
    has closed in the bytes so far, else empty; `stream` and `include_usage` are false;
    `prefix_hash` covers the same bytes as a held body's, the gateway waiting for
    min(body, 4 KiB) of it when the policy wants the hash.
  - A refusal is answered at once, before the upload ends, and reaches no venue. The rest
    of the body is read and dropped, and the connection, kept alive, carries the next
    request.
  - Allowed with no override and no hold, to a venue that byte-forwards, the body streams
    there as it arrives. Otherwise (a model or service-tier override, `Decision::hold`, a
    venue that translates) it is **parked**: kept unsent in the gateway to its last byte,
    holding no venue connection and no `--stream-body-max` slot, an OpenAI client's JSON
    still checked as it arrives, under the upload deadline of 0.94.0.
- **`Policy::confirm(whole, early)`**, called once per request decided on its head, with
  the whole body's facts, exactly a held body's, before the last byte leaves. The venue
  never has that byte before the confirm. `early` is the decision in force: its venue
  (after any failover, never -1), overrides, key header and tag.
  - Return `early` to keep it: the last byte goes. A change of tag alone is a keep, the
    tag updated.
  - Refuse (`allow` false, checked as `decide()`'s): a venue holding part of the body is
    reset, so it holds one short of its Content-Length and never runs it, and the client
    gets the policy's status and message.
  - Any other allowed decision redoes the request: the venue is reset and the whole body
    sent as a held body under the new venue, model, service tier, key header and tag.
  - The request was counted at `decide()`; a policy must not count it again here.
- **`Decision::hold`**: allowed, but nothing goes upstream before the body's last byte.
  Ignored on a whole body and from `confirm()`.
- `RequestRecord::decided_on_head` and `RequestRecord::confirm_outcome`
  (`ConfirmOutcome::None`, `Kept`, `Redone`, `Refused`), at the end of the record. With
  `body_streamed`, which a redo leaves set, a refusal or a redo of a body a venue held
  part of is a wasted partial upload: what to measure, per venue, before opting in.
- `Stats::policy_head_decisions`, `streamed_body_parked` (failover parks too),
  `streamed_body_redone` (redos of a body a venue held part of) and
  `streamed_body_confirm_refusals` (also counted in `policy_denied`), at the end of
  `Stats`. The exit profile and the SIGUSR1 dump print them when there was a head
  decision.

### Changed

- **A failover of a body decided on its head resends it from the client's buffer.** While
  it is still arriving, a failover to a venue that takes it as it is streams it there from
  its first byte; with an override, or to a venue that translates, it parks. Once whole it
  goes as a held body. A request pipelined behind it keeps its place. The original-bytes
  copy (`saved`) is no longer taken for a body that stays in the buffer, which also removes
  an underflow its size arithmetic had on a body not yet whole.
- **A stale connection's retry of a streamed body carries the key the policy named.** It
  rebuilt the head with the client's own credential headers, on the assumption that a
  policy's requests never stream.
- A policy without the opt-in, and every request with no policy, is unchanged: the same
  stamps, headers, histogram samples, sink fields and replies. The sink corpus records
  match 0.94.1's. Instructions per request (callgrind, 3000 requests, one worker) are
  within 0.7% of 0.94.1's on epoll (17,340 against 17,221) and 0.2% on io_uring (17,306
  against 17,279); `protocol_micro`'s medians are within 1.6%, its code unchanged; the
  allocation ceilings are unchanged.

### Tests

- `ProxyHeadDecide.*`, both backends, with a policy that opts in: a body reaching the
  venue before its last byte, decided once on its head and confirmed once with a held
  body's model, streaming flags and prefix hash; a policy without the opt-in holding the
  body and deciding once, whole; a head refusal answered before the upload ends with no
  venue contacted, the rest drained and the next request on the connection served; the
  last byte withheld while `confirm()` runs; a confirm refusal resetting the venue short
  of the body, with the policy's status and message, counted and recorded; a confirm redo
  sending the whole rewritten body to the second venue, the first reset, the record saying
  redone; a model override, a hold and a translating venue each parking the body, the
  confirm running before anything went, the rewrite and the translation correct; a body
  that names its model last, empty at the decision and present at the confirm; a decision
  waiting for the 4 KiB the prefix hash covers; a repeated `model` after the decision
  refused and never forwarded; a failover after a refused connect streaming to the next
  venue, a failover with an override parking, a parked body outliving the venue idle
  timeout, and a venue closing after the whole body failing over from the client's buffer
  with a pipelined request served in step; a stale retry of a streamed body carrying only
  the named key; past `--stream-body-max` a body decided whole, and a parked body taking
  no slot; a parked body cut at its deadline, stopped or trickling; a client leaving
  mid-body resetting the venue; the stamps of a streamed, a parked and a redone body.
- `ModelOf.EveryPrefixYieldsNothingOrTheWholeBodysModel`: `model_of` on every prefix of a
  body yields nothing or the whole body's model.
- Each behaviour fails its test when removed, on both backends, except withholding the last
  byte, which only epoll can show: io_uring submits the send after the confirm returns
  either way. The two `body_len - 1` caps, in `pump_body` and `dispatch`, are defence in
  depth: the read that brings the last byte stages nothing before its confirm.

### Known gaps

- A venue sees part of a prompt the policy may then refuse or move, though never a request
  it can run (SECURITY.md). A policy that must not answers `hold`.
- A non-OpenAI client's top-level keys are still not checked for repeats, held or arriving
  (SECURITY.md).


## [0.94.1]. 2026-10-12

### Tests

- `ProxyBodyStream.OnAndOffReadTheSameUpload` failed about one run in a hundred under a
  loaded ThreadSanitizer build, on both backends. It compared the streamed run's upload
  with the held run's, and held the wait before the first byte to a fixed 75 ms, but
  both are the test client's: its paced sleeps wake late, so two runs upload for
  different times. `send_slowly` now records when the client connected and when its
  first and last parts went, and each run's gateway stamps are checked against its own
  sends: the first byte and the last stamped after the client began the send that
  carried each and soon after it ended, and the wait before the first byte no longer
  than the client's own from its connect to its first send. The held record's
  equivalences, the upload's floor and our part's bound are unchanged.
- The provider's wait in that test and the first-token time in
  `ProxyBodyStream.OnAndOffReadTheSameFirstToken` are bounded for the streamed body
  only, by half of the run's own upload instead of a fixed 75 ms: counted from the
  first byte instead of the last, either would hold the whole upload. For the held body
  both are the mock venue reading a MiB at its own pace, which a loaded machine
  stretched past 75 ms and once past the upload; its last byte is t3, as checked.
- `GatewayTls.PartsStagedDuringTheHandshakeDoNotMoveT3` raced a fixed 150 ms venue
  handshake hold against a 50 ms client sleep, so a late wake-up sent the rest of the
  body after the handshake. The venue now holds its handshake until the test releases
  it, after the head was dispatched and the rest sent, and the hold is checked against
  the release time instead of a fixed 100 ms.


## [0.94.0]. 2026-10-12

### Security

- **A slow upload no longer holds a provider connection for free.** Since 0.90.0 a
  byte-forwarded body of 16 KiB or more is forwarded as it arrives, so while it arrives
  it holds a connection to the venue (a TLS handshake when cold) and a second
  descriptor. Every byte restarted the idle clock and there was no upload deadline, so
  a client trickling its body (a slowloris) held both, per connection, with almost no
  bandwidth; before 0.90.0 a slow upload held only the client socket. Now
  (GATEWAY-INTERNALS.md 12b):
  - **An upload deadline.** A streamed body gets `--body-grace` (10 s) and one second
    more for every `--body-min-rate` bytes it has delivered (1024 a second, 8 kbit/s,
    below a 2G data link), and with `--body-timeout` (off by default) a limit on the
    whole upload from its head. A slow but steady upload keeps earning time, so no
    client whose link is merely slow is cut, however large its body; a trickle runs out
    of time. A pause spends that time only up to `--upstream-timeout` (120 s): a longer
    one is cut as a client stall (below), whatever the body earned. A body that misses
    the deadline gets `408 Request Timeout` with a fixed message and its connection
    closes once the reply is out; the venue's connection is reset, never pooled, and
    the bytes that came after the deadline, the body's last included, are never
    forwarded, so the provider never holds the whole body. Counted in
    `Stats::streamed_body_timeouts` and logged as `TIMEOUT streamed body too slow`.
  - **A cap on streamed bodies.** A worker streams at most `--stream-body-max` bodies at
    once (256). Past it a body is held to its last byte exactly as before 0.90.0, one
    descriptor and no venue connection, counted in `Stats::streamed_body_capped`. It is
    bounded as a held body is: on a connection's first request it must arrive within
    the 30 s setup deadline, so while the slots are full a slower upload on a fresh
    connection is closed with no reply.
  - **`--stream-body off`** holds every body, for every client.

### Fixed

- **A client that stops sending mid-body is a client stall, not the venue's timeout.**
  When neither leg moved for `--upstream-timeout`, a streamed body cut short by its
  client was counted in `upstream_timeouts` and answered 504. A body the venue has all
  of (nothing staged, nothing in flight, nothing the kernel holds unacknowledged) is
  now the client's: `client_idle_timeouts`, a 408, the venue reset. Bytes the venue has
  not taken are still the venue's: `upstream_timeouts` and a 504, the venue reset too.
  The stall is answered directly, so it can neither fail over nor be retried.
- **A venue that stops reading mid-body times out while the body still arrives.** Each
  part a client sent restarted the idle clock, so a venue that stopped reading was
  noticed only once the client finished, however long that took. Only a send that
  moves bytes is progress now.
- **A venue left with a body that will not be finished is reset on every path.** A
  refusal already reset it; an error reply mid-body and a client leaving mid-body closed
  it with a FIN. On io_uring a reset close sent a FIN ahead of its RST; it shuts only
  the read side first now.
- The rest of a body cut short by an early response, read only to be dropped, could be
  trickled to hold the client connection until `--client-idle` (three days). It keeps
  the body's deadline: past it the connection closes.
- **A client gone before its `100 Continue` no longer has its request dispatched**
  (since 0.90.0). On epoll, a client that sent `Expect: 100-continue` and reset before
  the gateway wrote the interim line was closed by that failed write, and a body that
  would stream was dispatched anyway: the venue was paired with a client freed at the
  end of the event batch (a use-after-free once the venue's connection was written to
  again), and the body took a `--stream-body-max` slot that was never given back.
  `try_frame` now stops when the interim line closed the client. io_uring met it only
  with its submission queue full, where the send tears the client down at once; its
  send otherwise completes later, and the reset closes both legs.

### Added

- **`--stream-body on|off`** (config: `runtime.stream_body`, default on): off holds every
  byte-forwarded body to its last byte. `--stream-openai-body` stays the narrower
  switch, for an OpenAI client's body.
- **`--stream-body-max N`** (config: `runtime.stream_body_max`, 0 to 1048576, default 256,
  0 for no cap).
- **`--body-grace SECONDS`** (config: `timeouts.body_grace_s`, default 10),
  **`--body-min-rate BYTES`** (config: `timeouts.body_min_rate`, 0 to 2^30 bytes a second,
  default 1024, 0 off) and **`--body-timeout SECONDS`** (config: `timeouts.body_s`,
  default 0, off). The example config gives the reasoning for each default.
- `Stats` gains two counters at its end, `streamed_body_capped` and
  `streamed_body_timeouts`; existing fields keep their places. The exit profile and the
  SIGUSR1 dump print the streamed-body counters when there are any.
- `Gateway::set_stream_body`, `set_stream_body_max`, `set_body_timeout_ns`,
  `set_body_grace_ns`, `set_body_min_rate`, and `kDefaultStreamBodyMax`,
  `kDefaultBodyTimeoutNs`, `kDefaultBodyGraceNs`, `kDefaultBodyMinRate`.
- `net::unacked_bytes()`: what a socket's kernel holds unacknowledged (`SIOCOUTQ`).

### Changed

- **Behaviour change:** a streamed body that averages less than 1 KiB a second once
  its 10 s grace is spent is cut with a 408 by default; `--body-min-rate 0` turns that
  off.
- **Behaviour change:** past 256 bodies streaming at once on a worker, the next is held
  to its last byte.
- **Behaviour change:** a client stall mid-body is a 408 counted in
  `client_idle_timeouts`, where it was a 504 counted in `upstream_timeouts`; a venue that
  stops reading mid-body gets its 504 a full `--upstream-timeout` after it stopped,
  where it waited for the client's last byte.
- **Behaviour change:** a venue whose body will not be finished gets an RST and no FIN,
  on both backends, whatever ended the body.
- A 408 the gateway writes has the error type `timeout_error`.
- A held body, and a body held by `--stream-body off` or by the cap, reports exactly
  what it did: every stamp, header, histogram sample and sink field is unchanged. The
  sink corpus records match 0.92.0's.

### Tests

- `ProxyBodyStream.*` on both backends: a trickling body cut at `--body-timeout` with a
  408 typed `timeout_error`, its connection closed, the venue reset and never given the
  whole body, and not pooled; a body that stops coming cut by the sweep; a body under
  the minimum rate cut, and a steady slow one above it not; the rest of a cut-short
  body on the same deadline, a steady rest earning its time and the connection carrying
  its next request; past `--stream-body-max` a body held and still served, the slot
  freed for the next; `--stream-body off` holding an OpenAI and an Anthropic client's
  body with a held body's record; a client stopping mid-body on a pooled connection
  inside the stale-retry window counted as a client stall, never retried; a venue that
  stops reading timed out while the client still sends; a client leaving mid-body
  resetting the venue. `GatewayTls.ATrickledBodyToATlsVenueIsCutAtItsDeadline`.
- `Core.*` with the `FakeIo` and a clock the test sets: the defaults never cut 2 KiB a
  second over a 16 MB body and cut 512 bytes a second at 20 s; the body in at the head
  earns its time; bytes after the deadline never forwarded; every way a body ends frees
  its slot, and a cap of 0 holds no body; a stall told from a venue that stopped
  taking, and bytes the kernel holds unacknowledged counted as the venue's; only a send
  is progress; a client whose `100 Continue` failed never dispatched.
- `ProxyBodyStream.AClientGoneBeforeItsContinueIsNeverDispatched` on both backends: a
  sink holds the loop while the client's head and its reset both arrive; without the
  fix epoll leaks the slot and ASan reports the use-after-free.
- Each check, each credit, each slot release and each reset fails its tests when
  removed, on both backends wherever the test runs on both.
- The new flags' parse, bounds and `--help` lines; the example config pins every default.


## [0.93.0]. 2026-10-12

A request may carry one provider key per `x-kottos-key-<domain>` header, and the policy
names which one goes to the venue it picked. Mechanism only: which header serves which
venue is the policy's decision. DESIGN.md "Naming the key for a venue".

### Added

- **`Decision::credential_header` and `Retry::credential_header`**: the client header
  holding the key for the venue that decision or retry picked. Empty keeps today's
  behaviour, the key from `Authorization`, `x-api-key`, `x-goog-api-key` or `api-key`.
  Set, its value goes out as the venue's own key header and is the only credential the
  venue receives: `x-api-key` for Anthropic, `Authorization: Bearer` for
  OpenAI-compatible venues and Cohere, `x-goog-api-key` for Gemini, `api-key` for Azure.
  Translated and byte-forwarded, streamed or not. A request translated for Anthropic
  carries the client's `anthropic-version`, or 2023-06-01; a byte-forward keeps the
  client's as sent and adds none. Both fields come last in their structs, so existing
  initializers keep compiling, and both are copied, so they need only outlive the call.
- `kKeyHeaderPrefix` (`x-kottos-key-`), `kMaxKeyHeaderName` (64) and `key_header_ok` in
  `gateway/policy.hpp`, public so a policy can `static_assert` its own names;
  `refuse::kKeyHeaderMissing`, `kKeyHeaderRepeated`, `kKeyHeaderUnusable` and
  `kKeyHeaderTooMany`.

### Security

- **No `x-kottos-key-*` header reaches any upstream**, with or without a policy. Every
  header whose name starts with the prefix, in any case, is dropped from a byte-forwarded
  request: at dispatch, in the head of a body sent as it arrives, and in a stale
  connection's retry. The strip list matches exact names, so a client's
  `x-kottos-key-anything` reached the provider. A translated request never carried one.
- **A named key never falls back.** Refused with a 400 before any upstream is contacted,
  with fixed text and none of the client's bytes: the named header absent or empty,
  repeated in any case, whichever copy comes first, or holding a control or 8-bit byte or
  more than 8 KiB; a name outside the family; more than eight family headers a policy
  could name, since a repeat could hide past what the gateway reads; a Bedrock venue,
  which signs with a key pair and has no header for a bearer key. `Authorization` never
  stands in, because it may hold another provider's key.
- **A key named for a venue the gateway does not have is refused.** A decision that
  names a key with an `upstream_index` past the end of the table gets a 500 before any
  upstream is contacted; the first venue would have received a key meant for another
  provider as its own. Unnamed, such an index still means the first venue, and -1, the
  default, means the first venue named or not.
- **A failover carries the next venue's key, never the first one's.** The retry's header
  is adopted before the rebuild. A retry that names none after any earlier attempt that
  named one, the decision or a retry, gives up, and the client sees the failure; one that
  names a header the client never sent gets the 400, and the next venue is not contacted.
- **A sink cannot capture a family header, nor a credential header under another
  spelling.** A capture name is checked as the lookup reads it, without its trailing
  colon and whitespace, so `Authorization:` is refused as `authorization` is; it
  captured the client's bearer, in 0.91.0 too. A name with a colon left inside is
  refused, since it names no header and can match into a value.
- The key is read in the framing walk, which records each family line a policy could
  name, at most eight, as offsets into the head: no second walk and no allocation,
  named or not. A strip-list entry for the named header does not hide it from that read.
  Instructions per request (callgrind) are within 0.3% of 0.91.0 on both backends,
  byte-forwarded and as a translated stream.

### Changed

- **Behaviour change: a gateway no longer forwards `x-kottos-key-*` headers**, with no
  policy installed too. A deployment that relied on a provider seeing one loses it.
- **Behaviour change: with a key named, a byte-forwarded request no longer carries the
  client's `Authorization`, `x-api-key`, `x-goog-api-key` or `api-key`**; the named key
  replaces them.
- **Behaviour change: a sink capture name is read without its trailing colons and
  whitespace**, so `x-tenant: ` now captures `x-tenant`, and one with a colon inside,
  which captured nothing or part of a header's value, is refused at `set_request_sink`
  and logged.

### Tests

- `gateway_request_test`: `KeyHeader` and `KeySwap`. Each venue's own header, the
  client's `anthropic-version`, absent, empty, repeated and unsafe values, a bare CR
  refused by the framing walk, a signed venue at both the resolution and the signing,
  the family never read when nothing is named, an exact name match, a stripped key
  header still read, the table's bound with lines that cannot be named left out, the
  byte-forward swap, every refusal leaving the buffer empty, the family and lines no
  policy could name stripped when nothing is named, and a golden rebuild without the
  family. `TheNamedKeyIsReadFromTheFramingWalkAlone` poisons every head byte outside
  the recorded spans and still resolves the key, so nothing walks the head again;
  `ANamedKeyStaysWithinTheSameBound` holds the named path to the unnamed path's timing
  bound, which says nothing about how many walks it takes.
- Both backends, `ProxyKeyHeader`: translated requests to Anthropic, Gemini, Cohere and
  Azure, OpenAI and Anthropic client byte-forwards, streams, every malformed header
  refused with nothing sent, the table's bound, a SigV4 venue, names outside the
  family, the family with no policy installed, failover across dialects, a failover
  naming no usable key, a failover to a key the client never sent, an unnamed decision
  failing over to a named key, an unnamed retry after a named one, a key named for a
  venue past the table, three clients on one pooled connection, the sink under every
  spelling of a credential header's name, and the log.
  `ProxyBodyStream.TheKeyFamilyNeverLeavesWithAStreamedBody`: the streamed head and a
  stale connection's rebuild.
- `AllocCeiling.NamedKeyPassthrough` and `NamedKeyTranslated`: no allocation on either
  backend, as with the same policy naming none.
- `fuzz_body_stream` asserts no family header leaves in the held or the streamed head.
- Mutation-checked in a scratch copy, each mutation failing at least one test: falling
  back to `Authorization` when the named header is absent, skipping the family strip on
  a byte-forward, on a streamed head, or when the walk recorded no nameable line,
  keeping the first venue's header on a retry, keeping the client's own credential
  headers beside the named key, letting a repeated header win first, recording only
  the first copy, dropping the charset check, dropping the table's bound, retrying
  unnamed after a named decision or a named retry, accepting any name, letting the sink
  capture a family header, signing a Bedrock request despite a named key, a strip-list
  entry hiding the named header, reading the key with a second walk inside `named_key`,
  the wrong header for a venue, dropping `anthropic-version`, not adopting the
  decision's header at all, sending a key named for a missing venue to the first one,
  and checking a capture name with its colon.

### Known gaps

- A Gemini client byte-forwarding to a Gemini venue can carry its own key in the query
  (`?key=`); a named key header is added beside it, not instead, so the venue receives
  two keys and Google decides which applies.


## [0.92.0]. 2026-10-12

### Fixed

- **A streamed request body reaches a TLS venue whole.** The session tickets a TLS 1.3
  venue sends after its handshake arrive as records with no response byte in them,
  and the first one ended the body as though the venue had answered: the rest was read
  and dropped, the venue waited for a body short of its `Content-Length`, and the
  client got no reply until a timeout. Only a response byte ends the body now. A
  plaintext venue was not affected.
- **A streamed body's upload is the client's time again, not ours.** t3 was stamped
  again as each part of the body went, so the whole upload was reported as the
  gateway's: in `x-llmbridge-upwrite-us`, in `request-path` and in `added-total`, while
  `client_upload_ns` and `x-llmbridge-client-upload-us` read about 0. For a 10 MB
  transcript at 100 Mbit/s `request-path` printed 839 ms, the upload; it now prints 25
  to 27 ms on epoll and 15 ms on io_uring, the scan, copy and send of the parts, where
  the same body held reads 15 to 27 ms (LATENCY.md 2b). That is still past the
  histogram's 2.62 ms range, so a multi-MB body's sample, held or streamed, prints
  `[overflow!]`, but it is our work now and not the upload.
  - t3 is the attempt's first send, the head and the body there at dispatch, in the
    kernel, counted in wire bytes: parts queued behind it while it is backed up, or
    that came during a cold venue's TLS handshake, do not move it. A stale retry's
    fresh connection takes its own.
  - `client_upload_ns` runs from the request's first byte to the body's last, and on
    a connection's first request `client_conn_setup_ns` runs there too, so it still
    holds the upload and subtracting one from the other still works.
  - The provider's wait, `x-llmbridge-upstream-us` and `x-llmbridge-upstream-ttfb-us`,
    starts at the body's last byte handed to the kernel, and `first-token` at the
    body's last byte in: it held the upload too, about 300 ms of a 300 ms upload
    against 4 ms for the same request held.
  - Our work on each part pumped after t3, the scan, the copy and the send, is summed
    into `request-path` and `added-total`; a part pumped before t3 is inside
    `connect-us` or `upwrite-us` already.
- The request after a streamed body on the same connection reported an upload
  reaching back into that body's; it times its own, after a body cut short by an early
  response too.

### Added

- `RequestRecord` (`gateway/sink.hpp`) gains four fields at its end: `ts_body_end` (the
  body's last byte in), `ts_body_sent` (its last byte in the kernel, where the
  provider's wait begins), `body_work_ns` (our work on its parts after t3) and
  `body_streamed`. A held body reads t0, t3, 0 and false, so one formula serves every
  request. Existing fields keep their places.
- **`x-llmbridge-body-work-us`**: with `--timing-headers`, on a request whose body
  streamed, our work on its later parts. A held body's reply does not carry it.
- `timing_split()` takes the body's last send and the work after t3, both defaulted
  to 0, which is a held body.
- `Gateway::set_upstream_sndbuf_for_test()`: SO_SNDBUF on upstream connections, as
  `set_client_sndbuf_for_test()` sets it on clients.

### Changed

- **Behaviour change:** for a streamed body, `x-llmbridge-upwrite-us`, `request-path`,
  `added-total` and `first-token` no longer hold the client's upload;
  `client_upload_ns` and `x-llmbridge-client-upload-us` do, and so does
  `client_conn_setup_ns` on a connection's first request, which now runs to the
  body's last byte; `x-llmbridge-upstream-us` and `x-llmbridge-upstream-ttfb-us`
  start at its last byte sent, as they did. This replaces 0.90.0's "t3 the last piece
  sent".
- A held body reports what it did: every stamp, header, histogram sample and sink
  field is unchanged. A body is held when it is under 16 KiB, compressed, translated,
  sent while a policy is installed, an OpenAI client's with
  `--stream-openai-body off`, or already whole when its head was framed, whatever its
  size; `body_streamed` says which, and a reader should go by it and not by size.

### Tests

- `ProxyBodyStream.*` on both backends, against a throttled body: the upload lands in
  `client_upload_ns`, and `upwrite`, `request-path` and the histograms hold only our
  work, the histogram's sample being the sink's number; a stream's timing headers
  match the sink; a held body's new fields are t0, t3 and 0 and its headers and
  histogram what they were; a last part whose write came up short (the epoll write
  cap; io_uring always) ends the body without moving t3; a stale retry stamps the
  fresh attempt; the upload runs from the head to the last byte and the next request,
  after a whole or a cut-short body, starts its own; on and off read the same upload.
  All fail on both backends against 0.91.0, and each of the retry reset, the first-byte
  restarts, the once-only t3, the provider wait and the body work fails its tests when
  removed.
- `ProxyBodyStream.PartsQueuedBehindTheFirstSendDoNotMoveT3` holds the venue's reads
  until every part is queued behind a first send that cannot drain;
  `GatewayTls.PartsStagedDuringTheHandshakeDoNotMoveT3` stages them during a held
  handshake; `ProxyBodyStream.OnAndOffReadTheSameFirstToken`; a fresh connection's
  setup holds its streamed upload. Each fails on both backends without its fix, the
  TLS one also with the first send measured after the whole push, and the first with
  the body work counted before t3.
- `GatewayTls.AStreamedBodyReachesATlsVenueWhole` fails on both backends against
  0.91.0; `GatewayTls.AStreamedBodysLaterPartsDoNotMoveT3` covers the TLS flush paths.
  The suite's sink is a fixture member, joined before it goes.
- `TimingSplit.AStreamedBodysWaitStartsAtItsLastByteAndItsWorkIsOurs`,
  `AHeldBodysSplitIsUnchanged`.


## [0.91.0]. 2026-10-11

### Changed

- **Large bodies translate faster.** At 10 MB, translating a chat transcript to
  Anthropic went from 24.0 to 15.5 ms (best of 15 runs; BENCHMARKS.md), and a body of
  tool schemas from 48.9 to 23.5 ms, with no allocation left at `--retain-mb 16`.
  The bytes sent are unchanged: the translate goldens and the bijection test are
  byte-identical.
  - A string's plain run is found sixteen bytes a step (SSE2 on x86-64, an
    eight-byte word scan elsewhere) instead of a branch per byte, in the parser and
    in the streamed-body scan.
  - A request body is parsed for translation with a tool's `parameters` opaque: the
    schema is checked exactly as before, repeated keys at any depth included, but no
    node is built for it, since every translator forwards it byte for byte.
- `json::parse()` takes a list of opaque member names; `provider::parse_request_body()`
  and `kOpaqueRequestMembers` name the request's. `json::detail::string_stop()`.

### Tests

- The run scanner against a byte loop at every offset of random strings; an opaque
  member is its span with no children and gets every refusal a built one does
  (repeated key, unescaped repeat, bad escape, control byte, leading zero, trailing
  comma, nesting past `kMaxDepth`, more than `kMaxNodes` values). `fuzz_json` checks
  the scanner against the byte loop, and the same verdict and the same tree with and
  without opaque members, on every input.


## [0.90.0]. 2026-10-11

### Added

- **A large request body reaches its venue as it arrives.** A byte-forwarded body of
  16 KiB or more that is not all here when its head is framed is sent on in the pieces
  it arrives in, so the venue no longer waits for the whole upload before its first
  byte. At 100 Mbit/s a 10 MB body's first byte reached the venue 0.15 ms after
  the client began instead of 843 ms, and a 1 MB body's 0.32 ms instead of
  80 ms, and the reply came 15 ms sooner at 10 MB (BENCHMARKS.md). A translated
  body, and every body while a policy is installed, still waits for its last byte:
  both read the whole body. GATEWAY-INTERNALS.md 12.
- **`--stream-openai-body on|off` (config: `runtime.stream_openai_body`, default on).**
  An OpenAI client's streamed body is checked before each piece goes: JSON syntax,
  nesting, and the top-level and `stream_options` keys a held body is refused for. A
  key is held back until its closing quote arrives, so a repeated `model` never leaves
  the gateway, even in the body's last read. On a refusal the gateway stops sending,
  resets the upstream connection (the venue holds a body short of its
  `Content-Length`, which it never runs) and answers the client the 400 a held body
  gets. Off, the body is held to its last byte exactly as before.
- **`--retain-mb` (config: `runtime.retain_mb`, 0 to 16, default 4).** The JSON parse
  storage a worker keeps between bodies, which was fixed at 4 MiB. At 16 a 10 MB chat
  transcript translates with no allocation after warm-up (5 at 4). The cost is up to
  twice that in resident memory per worker, arena and scratch each: 12 MB more was
  measured after 10 MB of tool schemas.
- `provider::BodyScan` (`provider/body_scan.hpp`, installed): an incremental JSON
  check for a body that arrives in pieces. `json::set_retain_bytes()` and
  `Gateway::set_retain_bytes()`, `Gateway::set_stream_openai_body()`.
- Stats: `streamed_bodies`, `streamed_body_refusals`.
- Benchmarks of 1, 4 and 10 MB bodies in three realistic shapes (a chat transcript,
  many tool schemas, base64 images): `bigbody` (`bench/bigbody.cpp`) sends one through a running
  gateway at a set rate and times the venue's first and last byte and the reply
  (`bench/run_large_body.sh`, or `LARGE=1 bench/run_bench.sh`); `protocol_micro
  --large` times the translators, the scan and the parse on them and counts their
  allocations and JSON values.

### Changed

- **Behaviour change:** a non-OpenAI client's byte-forwarded body of 16 KiB or more
  (an Anthropic client to an Anthropic venue, say) is forwarded as it arrives, without
  the check, which is not optional: the gateway reads nothing in it. A venue that
  answers before the body is all sent gets its connection closed instead of pooled,
  and the rest of the body is read and dropped so the client's next request frames.
- **Behaviour change:** for a streamed body, t0 is the head's framing and t3 the
  last piece sent, so the rest of the upload counts as upstream wait, not gateway
  overhead (LATENCY.md).
- The JSON parser keeps four parked arenas per thread instead of one, so a parse
  inside a parse (a tool call's arguments) takes one back too.
- `json::kMaxNodes` stays at 2^20, measured against the 10 MB shapes: a transcript is
  50 K values, images 57; only a body of nothing but tool schemas, at 790 K, comes
  near it.

### Tests

- `ProxyBodyStream.*` on both backends: the venue gets the body before its last byte,
  and not with the option off; a repeated key in a middle or the last read and an
  invalid byte after streaming began are never forwarded, the mock venue recording
  that it never received a complete body, and the client gets its 400; a stale pooled
  connection's retry resends the whole body; an early response leaves the client
  connection in step; a body not declared JSON is forwarded unchecked. With the check,
  the discard or the retry's rebuild removed, the matching tests fail.
- `BodyScan.*` against the held parse on every split of hand-written and random
  bodies; `fuzz_body_stream` checks the scan against the parse on arbitrary splits.
- `AllocCeiling.LargeTranslatedRequestWithBuffersKept`: a 10 MB translated request
  at `--retain-mb 16` allocates nothing on either backend.


## [0.89.0]. 2026-10-11

### Fixed

- **A Gemini request names its model in the path.** The target was always
  `/v1beta/models/gemini:generateContent`, which names no model, so every request
  through the dialect failed against Google. It is now
  `/v1beta/models/{model}:generateContent`, from the request's `model` or a policy's
  override, sent as one percent-encoded segment (an API-style `models/` prefix is not
  doubled). A request with no model is refused (400).
- **A streamed request to a venue that cannot stream is refused.** An OpenAI client
  asking a Gemini or Cohere venue for `stream: true` got one JSON body, which an SDK
  reads as an empty stream; it now gets a 400, as Bedrock's did. A Gemini or Cohere
  client byte-forwarding to its own dialect still streams.
- **Finish reasons follow each provider's documented list.** Anthropic `refusal` is
  `content_filter` and `model_context_window_exceeded` is `length` (both were `stop`);
  Gemini `SPII`, `IMAGE_SAFETY` and `LANGUAGE` are `content_filter`, as is a prompt
  blocked before any candidate (`promptFeedback.blockReason`), which was a successful
  empty answer. Applies to the Anthropic stream as well.
- **A finish reason that is the provider's own failure is a 502.** Cohere `ERROR` and
  `TIMEOUT`, and Gemini `MALFORMED_FUNCTION_CALL`, `UNEXPECTED_TOOL_CALL` and
  `TOO_MANY_TOOL_CALLS`, read as `stop`, a successful answer; they are now no
  completion at all, and the gateway answers 502.
- A Bedrock model id reaches the path decoded from its JSON form: an escaped `\/` in
  an inference-profile ARN was sent with its backslash.

### Changed

- **One venue table.** `provider/dialect.hpp` (installed) holds `Venue`, `Wire`,
  `Auth`, `VenueSpec`, `spec()`, `venue_named()`, `venue_names()`, `finish_of()` and
  `client_wire()`: each venue's one name, body wire, credential scheme, streaming,
  translators, path builder and finish table. The gateway looks rows up where it
  switched on the venue, and the flag and config choices are the table's names.
- **Breaking: `UpstreamDialect` is an alias of `provider::Venue`**, and
  `llmbridge::Dialect` of `provider::Wire`. The enumerators and their values are
  unchanged; a forward declaration of `enum class UpstreamDialect` no longer compiles.
- **The body of a translated request is parsed once.** A model override reaches the
  venue's translator and path through `RequestOut`, instead of a splice and a second
  parse. The bytes sent are unchanged.
- **Behaviour change:** the gateway logs `venue cannot stream` for a refused stream on
  any venue (Bedrock's was `bedrock venue cannot stream`).
- **Behaviour change:** `gemini_to_openai_response` and `cohere_to_openai_response`
  (and the `anthropic_` one, should Anthropic add a failure reason) return empty or
  false for a failure finish reason.
- `provider::openai::Finish` and `finish_name()` name a finish reason;
  `json::detail::parse_count()` counts the documents parsed on a thread.
- `translate.hpp` is frozen as string facades over the table.

### Tests

- `ProxyVenueTable.AGeminiRequestNamesItsModelInThePath`,
  `GeminiAndCohereRefuseAStreamedRequest` and `ACohereErrorFinishIsABadGateway` fail
  on both backends against 0.88.0; `FinishTables.EveryDocumentedValue` lists each
  provider's documented values and `ABlockedGeminiPromptIsAContentFilter`, both failing
  against 0.88.0; `VenueTargets.*`, `Dialect.OnlyAVenueThatStreamsServesAStreamedTranslation`
  and `EveryVenueHasOneName`; `ParseOnce.AnOverriddenTranslatedRequestParsesTheBodyOnce`
  on a 130 KB body, which fails with the splice put back.


## [0.88.0]. 2026-10-11

### Fixed

- **A relayed provider error keeps the headers that say when to retry.** A translated
  non-200 reply and a chunked byte-forwarded reply were re-framed with only
  `Content-Type`, `Connection` and `Content-Length`, so a client got the provider's 429
  without its `Retry-After`, rate-limit counts or request id. Both now keep the
  upstream's `Retry-After`, `retry-after-ms`, `x-ratelimit-*`, `anthropic-ratelimit-*`,
  `ratelimit*`, `x-request-id`, `request-id` and `x-amzn-requestid` headers, verbatim;
  nothing else crosses. A stream's head carries them too.
- **A chunked byte-forwarded reply keeps its `Content-Type`** (and its
  `Content-Encoding`), where it was always labelled `application/json`.
- **A policy's refusal keeps any 4xx or 5xx it names.** A code the error table did
  not list (405, 409, 422, 451, 501, 599) went out as `502 Bad Gateway`, with
  a 4xx's own message under it. One status table now serves every reply the gateway
  writes; an unlisted code keeps its number and its class's phrase and error type.
- **A byte-forward no longer passes the client's hop-by-hop headers upstream.**
  `Connection`, `Keep-Alive`, `Proxy-Connection`, `TE`, `Trailer`, `Upgrade` and any
  header a `Connection` line names are dropped, so a client sending
  `Connection: close` no longer costs the provider leg a fresh connection (and over
  TLS a handshake) per request, and `Upgrade` no longer invites a 101.
- **`/v1/threads/{id}/messages` and other OpenAI paths ending in `/messages` are
  OpenAI.** The client's dialect was a substring search of a request line cut at 128
  bytes, so an OpenAI venue refused these with "no translator". It is now an exact
  route on the method and the full target, the query stripped.
- **Azure's `api-key` header is a credential.** It was not read, so an AzureOpenAI
  SDK client was refused with "control characters are refused". Azure takes
  `api-key`, then `x-api-key`, then a Bearer token; with none of them the request is
  refused as having no credential.
- **The Bearer scheme is matched in any case** (RFC 9110 11.1): `BEARER k` was not
  mapped, so the provider answered 401.

### Changed

- **Behaviour change: an HTTP/1.0 request is byte-forwarded as HTTP/1.1**, and the
  venue's connection is pooled on the venue's response alone, whatever the client's
  `Connection` said. The client's own connection still closes as it asked.
- **Behaviour change: a head with more than four `Connection` lines is refused**
  (400) on a byte-forward, rather than forwarded with a listed header kept.
- **Behaviour change: a client's dialect is an exact route.** A POST ending in
  `/v1/messages` or `/v1/messages/count_tokens`, or any method under
  `/v1/messages/batches`, is Anthropic; a POST ending in `:generateContent` or
  `:streamGenerateContent` is Gemini; a POST ending in `/v2/chat` is Cohere. Anything
  else is OpenAI. `client_dialect_from_target` is replaced by
  `client_wire(method, target)` in `gateway/dialect.hpp`.
- An Azure request with no credential is refused with its own message
  (`refuse::kNoCredential`).
- The reason phrase for 413 is `Content Too Large` (RFC 9110) everywhere, where a
  relayed 413 said `Payload Too Large`.
- **The request head is walked once.** `net::http::parse_request` takes a visitor, and
  the gateway reads the credentials, `Expect` and `Connection` in the framing walk.
  `scan_auth_headers`, `expects_continue` and the walk in `request_without` are gone;
  `Field` names the hop-by-hop, credential and request-id headers, and
  `net::http::hop_by_hop` says which are which.
- **The request builders write straight into the kept scratch**: the byte-forward
  rebuild (`build_forward`) in one pass, the translated request with its credential
  lines appended in place. `net/http_write.hpp` holds the status table, the head
  writers and the relay allowlist; `append_http_status` and `reason_for` are gone.

### Performance

- A byte-forwarded request makes no allocation once warm, from 3 (the head copy, the
  `Content-Length` and the `Host` line); a translated request and a translated stream
  make none either, from 2 (the credential lines' temporary). `alloc_test` ceilings
  lowered to 0.

### Tests

- `ProxyRelay.*` (a translated 429, a chunked byte-forward 429, a stream head),
  `ProxyPolicy.ADenyKeepsAnyStatusItNames`, `ProxyHop.*` (hop-by-hop dropped, clients
  that close sharing one pooled upstream, HTTP/1.0 sent as 1.1, an Assistants path
  byte-forwarded), `ProxyAuth.TheBearerSchemeIsCaseInsensitive`,
  `ProxyAzure.AnApiKeyHeaderIsTheCredential` and `NoCredentialIsNamedAsMissing`: each
  fails on both backends against 0.85.0.
- `AuthHeaders.AnAzureApiKeyIsACredential`, `TheBearerSchemeIsCaseInsensitive`,
  `ErrorReply.AnyStatusKeepsItsOwnCode`, `Rebuild.HopByHopAndConnectionListedHeadersAreDropped`,
  `AnHttp10RequestLineGoesAsHttp11`, `MoreConnectionLinesThanTheWalkHoldsAreRefused`,
  `Dialect.OnlyAnExactRouteNamesAnotherDialect`, and `net` tests for the visitor, the
  field names, the status table and the allowlist.

## [0.87.0]. 2026-10-11

**Behaviour change: io_uring pauses a slow client's stream instead of dropping it.**

### Changed

- **io_uring applies back-pressure to streams**, as epoll does. When a stream owes its
  client more than 1 MiB, the upstream's multishot recv is cancelled and the venue is
  read no further until the client has drained what it is owed; then the recv is
  re-armed. Before, the io_uring backend kept reading and dropped the stream once
  8 MiB was owed (`CAP stream buffer exceeded`), so a client that paused for a moment
  on a long answer lost it. That stream now completes, byte for byte.
- **The 8 MiB cap stays on io_uring as a ceiling**, paused or not: bytes the kernel
  placed before the cancel still arrive. It now counts ciphertext as well as plaintext,
  so it bounds a TLS client too. epoll still has no cap; its reads are budgeted per
  event.
- **A client that stops reading mid-stream is held, not dropped, on io_uring**: its
  upstream is paused and the stream is cut when `--upstream-timeout` passes with
  neither leg moving, logged `client stalled` and counted in `client_idle_timeouts`, as
  on epoll. `stream_pauses` now counts io_uring pauses, and each one logs
  `CAP backpressure`.
- A paused io_uring upstream is not polled, so a venue that resets during the pause is
  noticed when the client has drained and reads resume.
- `IoCaps` carries the threshold (`stream_pause_above`: 0 on epoll, a short write;
  1 MiB on io_uring, where every send is queued while in flight) and the cap
  (`stream_cap`).

### Measured

- The 32 MiB flood to a TLS client that never reads
  (`GatewayTls.ClientThatNeverReadsCannotGrowUsWithoutBound`): process peak RSS on
  io_uring 55 MB, from 69 MB when the cap dropped the stream (5 runs each, no overlap);
  epoll 53 MB, unchanged.
- Stream saturation on io_uring at 512 and 2,048 streams, and the request benchmarks,
  are within noise.


## [0.86.0]. 2026-10-11

### Changed

- **A connection is a `ClientConn` or an `UpstreamConn`**, both `final` and derived from
  `ConnBase`, which holds what a socket needs on either leg (fd, buffers, TLS Session,
  the backend's flags). The request, its stream and the client's stamps live only on a
  client; the venue, the response framer and the pool links only on an upstream. The
  single `Connection` struct, with its `is_client` flag and fields that meant one thing
  or another by kind, is gone, and so is every branch on that flag.
- **A pooled upstream is 496 bytes, from 2,112** (TLS build), held under 512 by a
  `static_assert`; a client is 1,872 bytes. Measured with 8,192 clients and 8,192
  pooled upstreams on one worker, RSS falls by 15.1 MB on both backends (epoll
  118.2 to 103.1 MB, io_uring 135.1 to 120.0 MB).
- **Each backend tags an event with the connection's kind**: io_uring in `user_data`
  next to the op, epoll in the event data. A completion goes to a typed handler; shared
  code that holds a `ConnBase` reaches the concrete type through one `visit()`.
- The request link is `ClientConn::up` and `UpstreamConn::client`, still written only
  by `pair()` and `unpair()`.
- `Gateway::retire_wbuf` and `adopt_warm`, the buffer-rotation test seams, take an
  `UpstreamConn*`: a client's buffer cannot be offered.

No behaviour change: the sink records of the corpus tests are identical on both
backends, and so is the test suite's outcome.

## [0.85.1]. 2026-10-10

### Changed

- **Each backend is a `final` class that owns its loop**: `EpollIo`
  (`gateway/src/io/epoll_io.cpp`) and `UringIo` (`gateway/src/io/uring_io.cpp`) hold their
  own state (the epoll instance; the ring, its provided buffers and the drain count) and
  implement `IoOps`. `gateway_epoll.cpp` and `gateway_uring.cpp` are gone; no `ep_`/`ur_`
  method is left.
- **`Gateway` is a facade over `detail::Engine`** (`gateway/src/engine.hpp`), which holds
  the configuration, the registries and the one request lifecycle in `core/`. The public
  API is unchanged; `gateway.hpp` no longer includes `net/http.hpp`, `net/tls.hpp`,
  `net/uring.hpp` or `provider/translate.hpp`, and its t0-t6 table is gone: LATENCY.md is
  the only one.
- **A close is two halves**: `Engine::forget` does the bookkeeping on both backends
  (unpair, deregister, pool, doomed); the backend's `close` does the descriptor.
- **A fresh upstream costs one `epoll_ctl`**: it is registered once with
  `EPOLLIN | EPOLLOUT`, where it was added for reads and then modified to arm the write.
  It already took no `fcntl` (`make_client_socket` creates it non-blocking).
- **The convention check enforces the boundary instead of the prefixes.** Checks 1, 2
  and 5b (prefix crossings, one-sided unprefixed methods, constant prefixes) are
  replaced by: `core/` includes no backend header, `core/` makes no socket call, and
  `io/epoll*` and `io/uring*` never mention each other. Each rule is first run against a
  planted violation, and the run fails if one goes unnoticed. LATENCY.md cites
  `Class::method` for backend stamp sites.
- The epoll instance is created when the loop starts, not in the constructor; a
  failure there is logged and `run()` returns 1.
- DESIGN.md "Naming conventions" and GATEWAY-INTERNALS.md sections 4-8 and 11 describe
  the boundary and the two classes.
- The venue's RTT (`net::tcp_rtt`) stays in the shared `Engine::on_response`: a
  read-only `getsockopt` that works on either backend's socket, so it is not an `IoOps`
  call and no driver repeats it.


## [0.84.0]. 2026-10-10

### Fixed

- **An SSE response framed by Content-Length ends at its last byte**. A stream
  ended only at the upstream's EOF unless it was chunked, so a provider that framed it
  with Content-Length and kept the connection open left the client without its
  terminal chunk until the idle timeout truncated the stream (120 s by default), and
  the upstream was closed instead of pooled. The stream now counts its body against
  the length; bytes past it keep the upstream out of the pool.

### Changed

- **The response head is parsed once**. `net::http::ResponseFramer` on each
  upstream caches the head and frames the body by its `Body` kind; the gateway used to
  parse the head to detect a stream and then again on every read through
  `parse_response`. `parse_response` stays, sharing the body framing.
- **One response side for both backends**: `on_upstream_bytes`, `on_response`,
  `finish`, `stream_pump` and `stream_flush` replace the `ep_`/`ur_` twins. Backpressure
  is a capability: where `IoCaps::stream_backpressure` holds (epoll) a slow client
  pauses its stream's upstream; elsewhere (io_uring) the 8 MiB cap, now
  `kStreamBufCap`, drops the stream. 3 `ep_`/`ur_` twin pairs remain, all transport.
- **One decision and one log text per upstream failure**:
  - **Behaviour change: io_uring `resp_path` now includes the response parse**:
    t4 of a whole response is stamped before the parse on both backends.
  - The venue's address is parsed once, when the connection is created, and an
    unparseable one fails the acquire on both backends (epoll's `start_connect`
    already refused it, so only the decision site moved). epoll connects to that
    address with `make_client_socket` and `connect`.
  - An upstream send failure is `upstream write failed` on both backends; io_uring
    reported it as `upstream EOF`.
  - A TLS read failure is `upstream TLS failed` on both backends; io_uring logged it as
    `upstream write failed`, epoll as `upstream EOF`. Operators grepping logs for
    the old texts should be told.
- The convention check's "unmarked" rule follows calls through shared methods, now that
  a backend reaches most shared methods only through other shared ones.
- The venue's RTT (0.81.1) is read in the shared `on_response`, at the head of a
  stream or of a whole response, on both backends, as before.
- epoll's send disarms `EPOLLOUT` whenever it completes, so a write event after a
  stream or a reply finds nothing armed.

### Tests

- `ProxyForwardStream.AContentLengthStreamEndsWithoutWaitingForEof` fails on both
  backends against 0.83.1; `ResponseFramer.FramesLikeParseResponseAndParsesTheHeadOnce`;
  core tests for a slow client pausing its stream's upstream and a drain resuming it
  (deterministic, no kernel buffers), the cap where reads cannot pause, a
  Content-Length stream ending without EOF, bytes past it, and one head parse across
  ten body reads.


## [0.83.1]. 2026-10-10

### Fixed

- **Pipelining past the buffer cap pauses the client instead of closing it**.
  A client may pipeline behind a request in flight; past `kMaxClientBuffered`
  (16 MiB + 32 KiB, more than any legal request) the gateway used to abort the pair,
  legitimate requests included. It now stops reading that client (`set_read`), counts
  `Stats::client_read_pauses`, and resumes once its requests drain the buffer. On
  io_uring a pause cancels the multishot recv and a resume re-arms it.
- **A client that disconnects mid-reply is an error on both backends**. epoll
  counted it; io_uring, whose reply no longer held the upstream, closed it as if idle.
  One decision now: a client gone with a request, reply or stream still owed is an
  abort and counts in `errors`; one gone between requests does not.

### Changed

- **Behaviour change: io_uring `req_path` now includes parsing**. t0 is stamped
  just before the parse that frames the request on both backends; io_uring stamped it
  after. io_uring's `req-path` and `added-total` rise by the parse cost. This is not a
  regression: the two backends now measure the same span. LATENCY.md rows t0-t3 name
  the shared sites.
- **One request side for both backends**: `try_frame` (frame one request from `rbuf`)
  and `dispatch` (translate or rebuild, acquire, send) replace `ep_on_client_readable`'s
  framing, `ur_try_forward_buffered`, `ep_forward` and `ur_forward`.
- **A finished request re-frames from the buffer** instead of issuing another `read()`
  on epoll: one fewer syscall per keep-alive request.
- **The interim `100 Continue` is staged in `OutBuf`** and sent like any reply, through
  the backend's send; `send_interim_continue` and its direct `send()` are gone. An error
  reply while the interim line is still queued behind a full socket now closes the
  client instead of following it.
- A client write failure with no request owed closes without counting an error on
  epoll, as io_uring did.

### Tests

- `ProxyBackend.PipeliningPastTheCapIsPausedAndEveryRequestServed` (41 requests,
  40 MiB pipelined behind one whose reply is held until the client blocks, all
  served, no error) fails on both backends
  against 0.82.0; `ProxyBackend.BytesPipelinedBehindARequestInFlightAreBounded` now
  asserts a pause, not a close; `ProxyBackend.AClientGoneMidReplyCountsAsAnErrorOnBothBackends`
  fails on io_uring against 0.82.0. Core tests for the pause, the interim line and
  `client_gone`.
- `ProxyBackend.RunningOutOfDescriptorsDoesNotSpinTheWorker` serves one request
  before it fills the descriptor table: UBSan's first type check behind a call
  through `IoOps` probes memory with a pipe, which cannot be made at that point.


## [0.82.0]. 2026-10-10

### Changed

- **The `IoOps` seam** (`gateway/src/core/io.hpp`): the shared request lifecycle reaches
  a socket only through `send`, `connect`, `set_read` and `close`, which each backend
  implements over its own primitives (`Gateway::EpollIo`, `Gateway::UringIo`), plus
  `IoCaps::stream_backpressure` (epoll pauses a slow stream's upstream, io_uring caps
  its buffer). GATEWAY-INTERNALS.md 3d.
- **Nine `ep_`/`ur_` twins are one method each**: `error_respond`, `upstream_failed`,
  `abort_pair`, `begin_stream`, `stream_on_upstream_eof`, `finalize_stream`,
  `acquire_upstream`, `retry_upstream` and `release_upstream`. The convention check
  counts 7 twin pairs, down from 16. The sweep's `close_conn` and `abort_request`
  dispatchers are gone.
- io_uring closes a stream's upstream when it sends EOF, as epoll did, instead of
  holding it to the finalize; the finalize's release refused it anyway.
- io_uring submits a fresh upstream's connect when the connection is opened. A full
  submission queue there now fails the acquire (failover, else a 502) instead of
  aborting the client.
- epoll no longer disarms `EPOLLOUT` when it pools an upstream; a TLS upstream that
  armed it while its request went out takes one spurious writable event, which
  disarms it.
- io_uring replies through the shared `respond()`, which waits for a send already in
  flight instead of submitting a second one.

### Tests

- `gateway/tests/core_test.cpp`: the shared lifecycle driven by a `FakeIo`, with no
  event loop: an error reply behind a slow client's queued reply aborts instead of
  following it, a failed send aborts the pair, a request still queued for the
  upstream keeps it out of the pool, acquire, retry, failover, stream entry, EOF and
  finalize. Each folded method was broken once on purpose and failed `gateway_test`
  on both backends.
- `ProxyBackend.APooledConnectionDroppedRightAfterTheRequestIsRetriedOnce`: the
  stale-connection retry asserted on both backends; the existing retry tests reach it
  deterministically on epoll only. `ProxyForwardStream.ACloseDelimitedStreamEndsWhenTheProviderClosesLater`:
  the EOF that ends a close-delimited stream arrives after the body went out, so only
  the EOF path can flush the terminal chunk.


## [0.81.1]. 2026-10-10

The record of each request carries the kernel's round-trip time to the venue.

### Added

- **`RequestRecord::upstream_min_rtt_us` and `upstream_srtt_us`**: the kernel's
  minimum and smoothed round-trip estimates for the venue's socket, read with
  `TCP_INFO` when its response head arrives, on both backends, streamed or not, error
  statuses included. 0 when no venue answered.
- `net::tcp_rtt(fd)` and `net::TcpRtt`. In their own file, `net/src/tcp_rtt.cpp`,
  because glibc's `struct tcp_info` has no `tcpi_min_rtt` and `<linux/tcp.h>` cannot be
  included beside `<netinet/tcp.h>`.

### Performance

- One `getsockopt` per request, after t4 is stamped, so no timing span grows: 0.73 µs
  at p50 and about 1 µs at p99 over 200,000 calls on a loopback socket.

### Tests

- `net_socket_util_test`: a connected socket reports both estimates; a closed fd, an
  unconnected TCP socket and a UDP socket report zero, never the kernel's `~0U`
  starting minimum.
- `gateway_test`, both backends: a dialled request, a pooled one, a venue's 529 and a
  stream carry the estimates; a request no venue answered carries none. Each call
  site, the sink copy and the `~0U` mapping were removed in turn, and each removal
  failed a test.

## [0.81.0]. 2026-10-10

### Fixed

- **A failed request write after a stream began ends the stream**. A provider
  that answered early and reset while our request was still going out had its write
  failure taken for one before any response: the gateway wrote a 502 into the open
  stream, on both backends, and a pooled upstream would have re-sent the request
  behind a reply in progress. An error once a reply has begun, or with bytes still
  owed to the client, now closes the client.
- **A TLS `100 Continue` flushed in pieces no longer completes a request on epoll**.
  The write event that drained it found nothing owed, counted a reply, reset the
  message and closed a connection still waiting for its body.
- **A stale-connection retry never moves a buffer a send is reading**. The retry
  takes the request with `OutBuf::take()`, which copies while an io_uring send is in
  flight.

### Changed

- **Behaviour change: retry and failover only before any response byte.** A request
  is `Dispatched` until the first response byte, and only then may it be re-sent, on a
  fresh connection or another venue. A response cut short after it began used to be
  eligible for failover; it now gets the 502, because the venue may have acted on it.
- **`OutBuf` replaces `wbuf`, `woff`, `wpending` and `send_inflight`**
  (`gateway/src/core/outbuf.hpp`), and TLS ciphertext is an `OutBuf` too. Only an io_uring
  send pins one; output staged meanwhile waits behind the pin, so an io_uring stream no
  longer moves its pending output into a fresh buffer on every flush. The reply
  builders append to the connection's buffer, keeping its capacity (GP1).
- **Request phases** (`ReqState::phase`: `Idle`, `Dispatched`, `Responding`,
  `Streaming`, `Replying`) gate finishing, retry and failover, in place of reading the
  buffers; `client_interim_inflight` is gone. GATEWAY-INTERNALS.md 3c.
- io_uring's 8 MiB stream cap counts bytes not yet sent, where it counted the whole
  buffer being sent plus the pending one.
- `Gateway::set_epoll_write_cap_for_test(size_t)`, a seam that splits every epoll write.

### Tests

- `ProxyEarlyResponse.AWriteFailureAfterTheStreamBeganAbortsIt`, both backends, fails
  on both against 0.80.0; `GatewayTls.APartialInterimFlushFinishesNoRequest`, both
  backends, fails on epoll against 0.80.0 with the seam added; `OutBuf` unit tests
  (`gateway/tests/outbuf_test.cpp`), the pinned `take()` among them.


## [0.80.0]. 2026-10-10

### Fixed

- **A slow client that keeps reading a stream is no longer cut by the idle timeout**
  (G7). The deadline counted only upstream reads, so a stream whose upstream epoll had
  paused for a slow client, or one the provider had finished while io_uring was still
  sending it, aged toward `--upstream-timeout` while the client was taking bytes, was
  truncated, and was logged as `upstream silent`. The deadline now measures progress on
  either leg: a response byte read, a request byte sent, a byte the client took.

### Changed

- **Behaviour change: a client that stops reading is timed out as the client's
  stall.** When neither leg moves and bytes are still owed to the client, the sweep
  logs `TIMEOUT client stalled` and counts `client_idle_timeouts`, where it logged
  `upstream silent` and counted `upstream_timeouts`. The action is unchanged: a stream
  is cut without `[DONE]`.
- **The idle sweep walks the clients once per tick**, with no temporary vectors, where
  it walked them five times (GP1). Its teardowns pick the running backend themselves,
  so `sweep_idle` takes no `uring` flag. The heartbeat line is unchanged; it is now
  written after the walk that counts it, with the counts the tick began with.
- **Connections are kept in `Registry` sets** (`gateway/src/core/registry.hpp`), dense
  with O(1) add and remove: live clients, live upstreams (pooled ones too) and the
  closed awaiting their free. They replace the clients `unordered_map` and the doomed
  vector, which io_uring scanned on every free (LP1). The destructor and the io_uring
  drain walk the registries instead of reaching in-flight upstreams through `peer`.

### Tests

- `ProxyStream.ADrainingSlowClientOutlivesTheIdleTimeout` and
  `AClientThatStopsReadingIsTimedOutAsAClientStall`, both backends, each failing on
  both with the fix reverted; `ASilentUpstreamStillGetsA504AtTheShortDeadline`.
- `ProxyHeartbeat.TheHeartbeatCountsAnIdleClientAndItsPooledUpstream` and
  `TheHeartbeatCountsARequestInFlight` pin the heartbeat's values, both backends.


## [0.79.0]. 2026-10-10

One option table for the flags, the config file and `--help`. Each fix has a
regression test that fails without it.

### Fixed

- **Flags are validated like config keys** (P16). They used `atoi`/`atof`, so
  `--listen 70000` served port 4464, a negative `--upstream-timeout` switched the
  upstream idle sweep off, `--io urnig` became `auto`, `--workers` had no cap, and a
  flag missing its value was ignored. Each is now refused at startup, naming the flag.
- **`--duration` and `runtime.duration_s` keep a fraction.** `0.5` was truncated to 0,
  which ran until killed.
- **`listen.port` and `runtime.workers` refuse a fraction** (P16): `8088.9` served 8088
  and `1.7` ran one worker.
- **`upstream.strip_headers` in an `upstream` array is honoured** (P4). It was accepted
  in an array entry and ignored, so the header the operator meant to strip was still
  forwarded. The list stays gateway-wide: it applies to every venue, and entries that
  set it must list the same headers.
- **The config file accepts the `bedrock` and `azure` dialects** (P14), which the flag
  already did, so a venue table can hold them.
- **SIGINT or SIGTERM ends a `--duration` run at once** (P15); it used to wait out the
  deadline. The signal handlers' gateway pointer is cleared before the gateways are
  destroyed.

### Changed

- **Breaking: bad flag values are refused instead of coerced.** A value outside its
  range (`--listen` 0..65535, `--workers` 1..4096, timeouts, `--warmup` and `--duration`
  0..31536000 s, `--prefault-mb` 0..4096), a number with trailing text, an unknown
  `--io`, or a flag whose value is missing or is another `--flag` stops startup.
  `--workers 0` used to run one worker.
- **Breaking: `--help` is one line per flag**, with its range or choices and the config
  key it sets, generated from the table; file-only keys are listed after the flags.
  `--help` now wins even next to an unknown flag.
- A worker whose event loop returns now stops the others instead of leaving them
  serving its share of the port. No test: nothing outside `gateway/` can make a loop
  return early.
- Dialect, range and choice refusals share one wording between the flag and the key
  (`--io must be one of: auto epoll uring (got "urnig")`).
- `app/config.hpp` and `app/config.cpp` are replaced by `app/options.hpp` and
  `app/options.cpp` (`Settings`, `Option`, `parse_cli`, `parse_config`, `help_text`);
  the internal CMake target `llmbridge_config` is now `llmbridge_options`.
- `bench/run_stream_cpu.sh` and `bench/run_stream_headtohead.sh` pass
  `--upstream-dialect`; the retired `--translate` they used stopped startup.

### Tests

- `app_options_test` replaces `app_config_test`: the config cases ported, plus
  `Config.ArrayFormStripHeadersIsHonoured`, `Config.ArrayEntriesThatSetStripHeadersMustAgree`,
  `Config.BedrockAndAzureAreDialects`, `Config.IntegersRefuseAFraction`,
  `Config.DurationKeepsItsFraction` and `Config.EveryOptionAcceptsAValueOfItsKind`;
  `P16/CliReject.*` (one case per P16 example), `Cli.TheBenchFlagsStillParse`,
  `Cli.AFlagBeatsTheFileWhereverItSits` and the `--help` golden
  `Cli.HelpWinsAndIsGolden`.
- CLI tests `CliDurationTakesAFraction`, `CliSigintEndsADurationRun` and
  `CliRefusesBadPort`.
- `fuzz_config` fuzzes the config parser in CI.


## [0.78.0]. 2026-10-10

### Changed

- **Behaviour change: Gemini and Cohere refuse tool calling instead of dropping it.** A
  request carrying `tools`, an assistant `tool_calls` turn or a `tool` result is
  answered 400. Before, the tools were dropped, the call became an empty turn, and the
  result became an anonymous user turn (Gemini) or a `tool` message without the
  `tool_call_id` Cohere requires. Every request translator now reads `messages` through
  one walk, and a construct the dialect has no hook for is refused.
- **Behaviour change: a missing or unknown message role is refused** by every request
  translator. Anthropic forwarded it verbatim, Gemini made it a user turn, and Cohere
  forwarded it.
- **Behaviour change: there is no default model.** Anthropic and Bedrock refuse a request
  whose `model` is absent, empty or not a string, where they sent the retired
  `claude-3-5-sonnet-latest` (on Bedrock, as a path no model has). Cohere refuses it
  too, where it sent `command-r-plus`.
- **Behaviour change: an Anthropic `temperature` outside 0 to 1 is refused.** OpenAI
  accepts up to 2; Anthropic answers 400.
- **Behaviour change: `tool_choice: "none"` keeps the tools on Anthropic** and sends
  `{"type":"none"}`. The tools used to be dropped, and Anthropic refuses a history
  holding `tool_use` blocks when no tools are declared.
- `developer` messages are system messages in every dialect (Anthropic forwarded the
  role, Gemini made it a user turn, Cohere forwarded it).
- `max_completion_tokens` wins over `max_tokens` (Anthropic, Gemini and Cohere); it was
  ignored, and Anthropic fell back to 1024.
- Anthropic: `stop` becomes `stop_sequences` (a string or an array of strings; anything
  else is refused), and `parallel_tool_calls: false` becomes
  `"disable_parallel_tool_use": true` inside `tool_choice` (`{"type":"auto"}` when the
  request named no choice). Both were dropped.
- Empty text is skipped where the provider refuses it: a Gemini message or system prompt
  with no text is left out, and Anthropic's block form (content carrying a
  `cache_control`) leaves out empty text blocks.

### Added

- `json::unescape_append` (installed header `provider/json.hpp`): `unescape_string`
  onto the end of an existing string.
- `fuzz_json` asserts every request translator writes one JSON object or refuses, with
  an agent-turn seed. `bench/protocol_micro` gains a tool-carrying agent request.

### Performance

- The Anthropic request is written front to back into the kept buffer: no prefix string
  inserted at the end, tool schemas appended in place (copied once instead of three
  times), and each history tool call's arguments decoded straight into the output and
  parsed there, with no temporary. Translated requests make 2 allocations, from 3
  (`alloc_test` ceilings lowered); a 130 KB tool-carrying agent request translates about
  30% faster. Gemini and Cohere bodies are built in one buffer.


## [0.77.0]. 2026-10-10

### Fixed

- **The keep-alive pool cap bounds connections** (G1). It compared the number of
  venues with `kMaxIdleUpstreams`, so it could fire only with 8192 venues and the
  pool was bounded by peak concurrency alone. It now counts pooled connections across
  every venue, and the debug `pool=` field prints that count instead of the number of
  venues.
- **io_uring no longer pools an upstream that already sent EOF**. When the EOF
  arrived after the final chunk but while the stream's last send to the client was in
  flight, the connection went back to the pool with its recv over, and the next request
  on it was never answered. An upstream that reports EOF is never pooled, on either
  backend.

### Changed

- **One reusability check.** `UpstreamPool::release` (`gateway/src/core/pool.cpp`)
  decides for every path, streams included: open, keep-alive, a message boundary
  reached, no EOF from the venue, nothing past the response buffered, our request fully
  sent, and room in the pool. Two of its answers differ from before: **behaviour
  change:** a response followed by bytes nobody asked for now closes the connection,
  where they were discarded and the connection pooled; and the response-translation
  failure path keeps the connection only if the venue said keep-alive.
- **The pool is one intrusive list per venue.** Acquire takes the newest, a close
  removes in O(1) where it scanned the venue's pool, and the idle reaper visits only
  expired connections, where erasing from the front of a vector made reaping a large
  pool quadratic.
- **`peer` is written only by `pair()` and `unpair()`, and every close unpairs**; a
  closed client takes its in-flight upstream with it. Neither leg can keep a pointer
  to a freed connection, and `doomed` is
  still set only by the two close functions.
- `gateway/src/loop.hpp` is gone: its constants are in `core/limits.hpp`, its stream
  reuse test is the comment on `UpstreamPool::release`.
- `Gateway::set_pool_cap_for_test(size_t)`, a test seam.

### Tests

- `ProxyRoute.ThePoolCapCountsConnectionsNotVenues` and
  `ProxyPoolHygiene.AnUpstreamThatClosedAfterTheStreamIsNeverPooled`, both backends;
  the first fails with the fix reverted on both, the second on io_uring.
- `ProxyAuth.UpstreamBodyLongerThanContentLengthDoesNotPoisonTheNextRequest` now
  asserts the connection with residue is closed, not cleaned and pooled.


## [0.76.0]. 2026-10-10

### Fixed

- **A failover no longer reads the policy's model after it is gone, and sends the new
  venue its own model** (G3). `Decision::model` and `service_tier` were kept as views
  into the policy's memory, which `policy.hpp` promises only for the decision, and a
  failover read them again from a later event: a use-after-free under ASan, and venue
  B sent venue A's model name in any build. The overrides are now copied at the
  decision, and a failover takes `Retry::model` and `Retry::service_tier`.
- **Each request's timing stamps are its own** (G5). t2 and t3 were stamped only while
  still 0 and never reset, so a keep-alive client's next request on a fresh upstream
  kept the first one's: `connect-us` went negative and `upwrite-us` counted the time
  between the two requests, in the headers, the histograms and the sink. A failover's
  new venue also gets its own t2.
- **A framing error is a request of its own** (G9). Its 400 was sent before the
  sequencer ran, so the log line and the sink record carried the previous request's
  sequence number, stamps, venue and model on a keep-alive connection.
- **A venue given only `ips` gets a Host header** (G17). The constructor built the
  bare IP:PORT form's `Host` from `ip` before defaulting `ip` from `ips`, sending
  `Host: :PORT`. Library callers only; the app always sets `ip`.

### Changed

- **One per-request reset.** A request's state is `RequestCtx`
  (`gateway/src/core/req.hpp`), reset by `begin()` at framing and nowhere else. The
  resets in `sink_emit` (which ran only with a sink installed) and
  `stream_reset_for_next` are gone, and so is the `shrink_to_fit` of the stream
  scratch on every request. Strings a request keeps are bounded `FixedStr` copies;
  `GATEWAY-INTERNALS.md` 3b.
- **Behaviour change: a policy's `model` or `service_tier` longer than the gateway
  keeps (255 and 32 bytes) gets the request a 500** with a warning, where it used to be
  spliced whole. A failover whose `Retry` names one that long is not taken: the client
  gets the failure.
- **Behaviour change: a failover sends the client's own `model` unless `Retry::model`
  names another.** It used to resend the failed venue's override.
- `policy.hpp`: `Retry` gains `model` and `service_tier` (additive). The gateway's own
  error bodies know 500 (`api_error`); it went out as a 502 before.
- The sink's `upstream_index` is -1 for a request refused before a venue was chosen,
  instead of the previous request's venue.

### Tests

- `ProxyRoute.AFramingErrorAfterASuccessIsARequestOfItsOwn`,
  `AVenueGivenOnlyAnAddressListNamesItInTheHostHeader`,
  `AFailoverSendsTheRetryModelNotTheFailedVenuesOverride`,
  `AFailoverWithoutAModelSendsTheClientsOwn`, `AnOverrideLongerThanTheGatewayKeepsIsA500`
  and `ProxyRequestReset.ASecondRequestOnAColdUpstreamGetsItsOwnStamps`, both backends;
  each fails with the fix reverted.


## [0.75.0]. 2026-10-10

### Changed

- **`gateway.cpp` is split by concern, with no behaviour change.** Construction,
  teardown and `run()` are in `gateway/src/engine.cpp`, re-resolution in
  `resolve.cpp`, and the shared methods in `gateway/src/core/`: `transport.cpp` (the
  `tls_*` helpers and the interim 100 Continue), `sink.cpp`, `sweep.cpp`,
  `stream.cpp`, `upstream.cpp` and `client.cpp`. Function bodies moved unchanged:
  `git diff --color-moved` shows only moved lines, and the comment-stripped code is
  identical apart from includes and namespace lines.
- **Breaking (in-tree embedders only; the gateway library is not installed):
  `gateway/connection.hpp` is gone.** Its parts are `gateway/venue.hpp`
  (`UpstreamDialect`, `TlsConfig`, `Upstream`, `IoBackend`), `gateway/refusals.hpp`
  (`refuse::`) and the private `gateway/src/core/conn.hpp` (`Connection`). `Stats`
  moves from `gateway.hpp` to `gateway/stats.hpp`. `gateway.hpp` includes the three
  public headers, so code that includes it is unaffected.
- `scripts/check_conventions.py` reads every `.cpp` under `gateway/src`, so a new
  source file cannot escape the `ep_`/`ur_` checks.
- Comment essays from `connection.hpp`, `stream_truncate()`, `tls_invariant_ok()` and
  `tls_feed()` move to DESIGN.md "Dialect resolution" and GATEWAY-INTERNALS.md 6b
  and 10d.


## [0.74.0]. 2026-10-10

### Fixed

- **A truncated translated stream is no longer presented as complete.** Anthropic states
  the end of a message in-band, so a stream that ends before `message_delta`'s
  `stop_reason` (FIN, RST, or TLS EOF without `close_notify` on a close-delimited
  upstream) now fails: no fabricated `finish_reason` and `[DONE]`, an error counted, and
  the client's stream left cut. A stream that closes after the stop reason, missing
  only `message_stop`, still ends cleanly.
- **An Anthropic `error` event fails the stream.** It was ignored, and a clean end of
  body then closed the stream with `finish_reason: "stop"` and `[DONE]`.
- **SSE framing follows the spec.** A bare CR or CRLF ends a line as LF does (a CR-only
  stream used to buffer until the 1 MiB cap and fail), a `data` field without a colon
  counts as an empty data line, and nothing after the terminal event (`message_stop` or
  `[DONE]`) is read, where a later delta used to be emitted after `[DONE]`.
- **Translated-stream amplification is bounded.** `message_start`'s id and model are
  echoed into every chunk; one over 256 bytes now fails the stream, where a model name
  near 1 MiB turned each one-token delta into a 1 MiB write.

### Changed

- **Breaking: `AnthropicToOpenAiSse::finish()` writes nothing and returns false for a
  stream cut before its stop reason** (installed header `provider/sse.hpp`). It used
  to append a finish chunk and `[DONE]` for any stream not already ended.
- **One writer of OpenAI objects.** `provider::openai::Envelope` writes every
  completion, chunk and usage block (five hand-written envelopes and four usage writers
  before), and `write_error` both error envelopes. Translated output is byte-identical;
  the gateway's own error bodies gain `"code":null`, as OpenAI's carry.
- `provider/sse.hpp` gains `SseFrameReader`, `AnthropicToOpenAiSse::reset()` and
  `usage()`; `provider/translate.hpp` gains `anthropic_`, `gemini_` and
  `cohere_to_openai_response(body, out, usage)`, which write into a kept buffer and
  return the counts they wrote.
- **The gateway no longer scans translated bodies for usage**; it records the counts
  the translator wrote (GP1). The SSE translator is held in the connection and reset
  per stream instead of allocated per stream.

### Performance

- **Allocations** (`gateway_alloc_test`, ceilings lowered to match):

  | Path | Before | After |
  |---|---|---|
  | Passthrough request | 3 | 3 |
  | Translated request (OpenAI to Anthropic) | 12 | 3 |
  | Translated stream, 20 deltas | 7 | 3 |
  | Each further stream delta | 0.05 | 0.03 (measured 0.00 on both backends) |

  The three left are the request side's (`build_translated_request`), on every path.
- `bench/protocol_micro` p50, GCC Release, three interleaved runs against 0.70.0:
  `anthropic_to_openai_response` 822 to 581 ns; `AnthropicToOpenAiSse::feed` 202 to
  211 ns (+4.5%: one CR probe per read, which bare-CR framing needs).
- A stream chunk's hex size is written by hand: 45 to 11 ns per chunk, `snprintf` gone.

### Tests

- `SseFrames.*` (CR and CRLF at every split, a colonless `data`, nothing after the end),
  `Sse.AnErrorEventFailsTheStream`, `Sse.EofBeforeAStopReasonIsACutStream`,
  `Sse.ALongIdOrModelFailsTheStreamInsteadOfRepeatingInEveryChunk`,
  `Sse.AResetTranslatorIsANewOne`, `TranslateEnvelope.*`,
  `ProxyStream.AnUpstreamErrorEventFailsTheStream`,
  `ProxyStream.TruncatedUpstreamFailsTheClientStream` (it replaces
  `TruncatedUpstreamStillTerminatesClientStream`, which asserted the fabricated end) and
  `ProxyStream.UpstreamClosingAfterTheStopReasonEndsCleanly`, both backends.
- `fuzz_sse` checks that the LF, CRLF and CR spellings of a stream translate alike.

### Known gaps

- No per-step output budget on the stream pump: the echo cap bounds translated output to
  a small multiple of the input, which the existing buffer caps then hold.


## [0.73.0]. 2026-10-10

HTTP strictness. Each fix has a regression test that fails without it. Kept apart
from 0.72.0 so it can be reverted alone.

### Fixed

- **An HTTP/1.0 upstream reply is not pooled unless it says keep-alive.** It was
  treated as keep-alive, so under load every reuse of such a connection became a
  stale-connection retry.
- **`Connection` is read as a token list**, on requests and responses: `keep-alive,
  close` closes, and `closed` is no longer read as `close`.
- **A status code is exactly three digits**, followed by SP or the end of the line.
  `2000` and `200x` read as 200; a missing code framed with status 0. A status line
  with no reason phrase (`HTTP/1.1 200`) is still accepted.
- **1xx, 204 and 304 responses end at their head** whatever Content-Length or
  Transfer-Encoding say. A 304 carrying its resource's length waited for bytes that
  never come, and the client got a 504 after the upstream idle timeout.
- **A response to a forwarded HEAD ends at its head.** Byte-forward relays any
  method, and a HEAD reply carries its GET's Content-Length, so it timed out the same
  way and then failed over to every venue.
- **Chunk size lines are strict hex**: digits, optional whitespace, then a chunk
  extension (still accepted) or CRLF. `0x10` read as the last chunk and swallowed
  the data as trailers; `5zz` and a bare CR inside the line read as 5.
- **A malformed request line is refused with 400**: it must be `method SP target SP
  HTTP/1.0` or `HTTP/1.1`, with a token method and no whitespace or control byte in
  the target. Only the version was read, so such lines were forwarded.

### Changed

- **Breaking: `net::http::ResponseHead` gains `body`** (`net::http::Body`: `None`,
  `Length`, `Chunked`, `UntilClose`), and `parse_response_head` / `parse_response`
  take an optional `head_request`. `Message` gains `head`. New
  `net::http::has_token` and `net::http::request_line_ok`. `parse_response_head` now
  returns Error for a status line it used to accept.
- `fuzz_http_diff` now allows the new refusals and the deliberate keep-alive and
  bodyless-response changes, and still requires agreement on everything both framers
  accept. The strict checks cost nothing measurable: `parse_response_head` p50 0.77
  us against 0.75 us in 0.72.0 (`bench/protocol_micro`, 3 interleaved runs).


## [0.72.0]. 2026-10-10

### Changed

- **One header walker.** `net::http::walk_headers` splits a head once, checks each
  line's ending as it goes, and hands each line to the caller classified as a
  `net::http::Field`. `parse_request` and `parse_response_head` are rebuilt on it;
  `find_header` keeps its lenient contract and shares the name compare. No behaviour
  change: `fuzz_http_diff` runs every input through the new framer and through a
  frozen copy of the v0.70.0 one (`fuzz/http_legacy.hpp`, kept for one release) and
  asserts identical results, and CI fuzzes it for 60 s.
- The rationale comments in `net/http.hpp` moved to DESIGN.md "Parsing & framing".

### Performance

- **Head parsing is 2.6x faster.** The separate line-ending pass is gone and each
  line is classified once instead of tested against every known name.
  `bench/protocol_micro` `parse_response_head` on the 1 KB Anthropic head, p50 of 3
  interleaved runs, GCC Release: 2.13 to 0.80 us.


## [0.71.0]. 2026-10-10

### Fixed

- **Usage, the error type and the served tier are read at keys only.** The bounded
  scans over provider bytes matched substrings, so text and nested objects the model
  wrote could stand in for the provider's own fields. They now accept a key only where
  JSON puts one (after `{` or `,`, before `:`), take the last usage object in the
  window, and walk it member by member:
    - a tool's `input` ahead of the usage (`{"usage":{"cache_creation_input_tokens":900000}}`,
      `"service_tier":"priority"`) no longer replaces the real counts or tier, and a
      completion ending in `\"prompt_tokens` no longer hides `prompt_tokens`;
    - `"prompt_tokens_details":null` no longer reads `completion_tokens_details` as
      its own (audio input 5 where none was stated);
    - a count of more than 18 digits is "not stated" instead of a signed overflow;
    - the error type is the error object's own `code` or `type`, not one nested
      inside it.
    - a Cohere reply's cache write is "not stated" (-1, as it is) instead of 0: its
      `usage.billed_units` was read as an Anthropic usage block.
- **Gemini thinking tokens are output.** Gemini states `thoughtsTokenCount` beside
  `candidatesTokenCount`, and `totalTokenCount` is prompt + candidates + thoughts, so
  `completion_tokens` (translated) and `tokens_out` (sink, byte-forwarded) now include
  it; `reasoning_tokens` still states it on its own. One mapping serves both paths.
- **A translate refusal names the real cause.** A key repeated inside any object was
  logged and answered as "body is not valid JSON"; it is now "an object in the body
  repeats a key". Tool arguments with leading whitespace were named as the cause of
  any other failure; the leftover length check behind that is gone (0.69.0 known gap).

### Changed

- **One usage type**, `provider::openai::Usage` in the new installed header
  `provider/openai.hpp`, with `scan_usage`, `scan_string`, `scan_error_type` and
  `StreamUsage`. The gateway's `BodyUsage` is an alias of it, and Anthropic's prompt
  normalisation (fresh + cache read + cache write) is written once for the whole-body
  translator, the stream translator and the scan.
- The DOM-free top-level walk behind `model_of` and the body facts moved to
  `provider/src/json_scan.hpp` with no change in what it reads.

### Performance

- **A byte-forwarded stream's usage is read once per byte.** Every chunk that
  mentioned `usage` rescanned the kept 4 KiB tail, and with `include_usage` every
  OpenAI chunk carries `"usage":null`. `StreamUsage` searches only new bytes and keeps
  only an unfinished usage object. Per 176-byte chunk, GCC -O2, best of 7:

  | Chunk | Before | After |
  |---|---|---|
  | carries `"usage":null` | 1,699 ns | 67 ns |
  | no usage | 80 ns | 42 ns |

  A whole-body scan: 719 to 660 ns on a 3 KB OpenAI completion, 1,714 to 474 ns on an
  Anthropic one.

### Tests

- `JsonScanUsage.*` and `JsonScanStream.*` (every split of a stream gives the same
  counts), `ServedTier.AToolInputBeforeTheUsageCannotNameIt`,
  `TranslateFailure.NamesARepeatedKeyAndNotWhitespaceAroundArguments`; the Gemini
  fixtures now state `totalTokenCount` as Gemini does.
- New fuzz target `fuzz_scan` (in CI): the scans on any bytes, and a stream's usage
  equal however its bytes are split. `fuzz_json` checks the top-level walk against the
  parser's own reading of `model` and `stream` on every body the parser accepts.

### Not changed

- A client of another dialect (Claude Code on `/v1/messages`) still has its body read
  for `model` with an early stop and the first copy wins, as
  `BodyFactsPerf.AnotherDialectStopsAtTheModel` pins: refusing a repeated `model`
  there needs a full walk, about 290 us on an 800 KB body. OpenAI clients are refused
  on any repeated top-level key, and every translated request on any repeated key.


## [0.70.0]. 2026-10-10

### Fixed

- **SigV4 canonical form.** Header values keep their case (only trimmed, with inner
  spaces collapsed), so a capitalised host no longer fails with 403. Query names and
  values are percent-decoded before they are encoded, so `%3A` no longer signs as `%253A`.
- **The stderr log sink never blocks the loop.** When the stderr pipe is full the line is
  dropped and counted, and a write interrupted by a signal is retried.
- **Logging a double** that is NaN, infinite or past 2^64 no longer hits undefined behaviour.
- **A base path of several slashes** (`https://host//`) normalises to no path instead of
  keeping one slash and sending requests to `//v1/...`.
- **Sockets are created non-blocking and close-on-exec** in one call, and the epoll
  backend accepts with `accept4`, so no descriptor leaks into a child process.
- **A failed `TlsContext` init leaves no context behind**, and a second init replaces the
  first instead of leaking it.
- **Histogram percentiles use the nearest rank**: p99 of ten samples is the tenth, and a
  rank such as `100 * 0.29` no longer rounds one sample too low.

### Performance

- **TLS ciphertext staging** advances a read offset instead of erasing the pulled prefix
  on every pull, so draining a large response in small pieces is linear.


## [0.69.0]. 2026-10-09

### Changed

- **Breaking: `json::parse` is stricter** (installed header `provider/json.hpp`). It
  now refuses, with `ok=false`:
    - anything but whitespace after the root value (`{}x`, `{} {}`, `1 2`);
    - an object that repeats a key, at any depth, compared after unescaping
      (`{"model":"a","m\u006fdel":"b"}`); pass `json::Keys::Any` as a third argument for
      the old first-wins reading;
    - a document of more than `json::kMaxNodes` (2^20) values.
      Every translator, `rewrite_model`/`apply_overrides` and the gateway's error
      classifier get this through the default, so a request with a repeated key is
      refused with 400 instead of translated on its first copy.
- **Breaking: the config file refuses a repeated key**, `_` comment keys included:
  "config: a key appears twice in one object". Give each comment its own name.
- **`json::parse` reuses its arena.** A dropped document parks its emptied arena in a
  per-thread slot for the next parse, so a parse no longer allocates the arena and its
  block list. Allocation ceilings (`gateway_alloc_test`) lowered to match:

  | Path | Before | After |
    |---|---|---|
  | Passthrough request | 3 | 3 |
  | Translated request (OpenAI to Anthropic) | 16 | 12 |
  | Translated stream, 20 deltas | 59 | 7 |
  | Each further stream delta | 2.05 | 0.05 (measured 0.00, 0.01 on io_uring) |

  `bench/protocol_micro` p50, GCC Release, median of 7 interleaved runs against
  master: `anthropic_to_openai_response` 1301 to 1313 ns (+0.9%),
  `AnthropicToOpenAiSse::feed` 355 to 342 ns (-3.7%), `openai_to_anthropic_request`
  (130 KB) 172.6 to 174.4 us (+1.0%). `parse_response_head`, which this change does
  not touch, moved +0.9%: that is the layout noise floor.
- **One string escaper.** `append_escaped_string` was a byte-at-a-time copy of
  `append_escaped`; it is now a forwarding alias, kept for source compatibility, and
  the translators call `append_escaped` directly. Output is byte-identical.

### Fixed

- **A body of tiny values no longer costs ~1 GB and pins half of it.** A 16 MiB
  `{"pad":[0,0,0,...]}` parsed 8 M nodes (~1 GB peak, a ~2 s loop stall), and the
  per-thread scratch stacks kept ~512 MB per worker afterwards. The node cap stops it
  at 2^20 values, and scratch capacity past 4 MiB is freed after each parse. DESIGN.md
  "Parsing & framing" has the sizing.
- **Tool-call `arguments` with leading whitespace are translated.** The hand-written
  trailing-bytes check in the Anthropic translator compared the object's span with
  the string's length, so `" {\"a\":1}"` was refused as an injection. The parser now
  refuses trailing bytes itself and the check is gone.

### Tests

- `JsonDocument`, `JsonKeys`, `JsonBounds` (node cap exact at 2^20, the 16 MiB flood
  refused, scratch released past 4 MiB and kept below it), an escaper oracle over all
  256 bytes and 5,000 random strings, a parked-arena test,
  `ToolReqInjection.WhitespaceAroundArgumentsIsStillOneObject`,
  `StrictKeys.ARepeatedKeyAnywhereRefusesTheRequest` and three config cases.
- `fuzz_json` asserts that `Keys::Unique` only ever refuses more than `Keys::Any`,
  that a strict-parsed body never draws `DuplicateKey` from the top-level walker, and
  that `append_escaped` of any bytes parses back and unescapes to those bytes.

### Known gaps

- `gateway/src/request.cpp` (`translate_failure`) still carries its own
  `parsed.sv.size() != args.size()` check on tool arguments, which names arguments
  with leading whitespace as the reason when a request fails for another reason.
  Left for the gateway lane; the parser makes the check redundant.

## [0.68.0]. 2026-10-09

Safety fixes in the event loops. Each fix is its own commit with a
regression test that fails without it, except where noted.

### Fixed

- **A failover whose connect fails at once no longer duplicates the request.** The
  copy put back in the client buffer for the attempt was inserted a second time on
  failure; the leftover ran as a new request, failed over the same way and left
  another: one request served indefinitely, each run billable. Both backends.
- **Bytes pipelined behind a request in flight are bounded.** Reading continued with
  no cap while a request was in flight, so one client holding a slow request open
  could grow the buffer until the process ran out of memory. More than
  `kMaxHeaderLen + kMaxBodyLen` buffered now aborts the pair.
- **A torn `100 Continue` closes the client through its backend.** It only set
  `doomed`: epoll spun on the still-registered fd, and io_uring freed a connection
  still in `_clients`. Not deterministically testable (needs a torn 25-byte send).
- **Running out of file descriptors no longer spins a worker.** epoll's listener
  stayed readable while `accept()` failed with EMFILE/ENFILE; io_uring re-armed into
  the same error, and a re-arm that found the SQ full left the listener dead with
  nothing logged. The listener now pauses for 100 ms and is re-armed from the tick.
- **epoll: one writer of the interest mask.** Arming or disarming a write re-enabled
  reads on a backpressure-paused upstream, after which the client buffer was
  unbounded.
- **epoll: an upstream reset while paused no longer spins.** The dead fd stayed
  registered with mask 0 and raised EPOLLHUP on every wait. It is now closed at
  stream EOF; its remaining bytes are already in the client's buffer.
- **io_uring: SQEs left by a partial submit are resubmitted.** `submit_and_wait`
  counted from its own last tail, so ops behind one the kernel rejected were never
  issued and their connections hung. EBUSY/EAGAIN from `io_uring_enter` no longer
  stop the worker, and a real exit is logged.
- **io_uring lifetimes.** A full SQ ends the client and upstream together instead of
  freeing one under the other's pointer, and callers stop using a connection after a
  failed arm; a timer re-arm that found the SQ full (which disabled every timeout)
  is retried; cancel uses `IORING_ASYNC_CANCEL_ALL`; `ur_error_respond` aborts instead
  of replacing a buffer an in-flight send reads; the provided-buffer ring is
  unregistered before it is unmapped, and a second `init` releases the first.
- **A pooled request is resent only if it failed within 1 s of reuse.** The
  stale-connection retry fired however long the provider had held the request, so a
  provider that ran it and then dropped the connection got it twice, billed twice.
- **Byte-forwarded targets are validated with or without a base path.** Dot segments
  and backslashes, plain or encoded (`/../`, `%2e%2e`, `%5c`), escaped the venue's
  base path, and with no base path an absolute-form target was forwarded verbatim,
  letting the client pick the upstream vhost. A literal `//` is still allowed.
- **The request line is never read or stripped as a header.** A first line shaped
  like a header was shown to the policy, then stripped, and the next line became the
  upstream's request line. The credential scan on the translated path skips it too.

### Added

- `Stats::accept_backoffs`: listener pauses for lack of file descriptors.
- Test seam: the mock upstream can reset mid-response and drop a connection's second
  request after a delay.

### Known gaps

- The translated-stream amplification (a hostile upstream's long `model` echoed into
  every chunk) and the fabricated `[DONE]` on a cut stream are fixed in the SSE
  translator, refactor step S8; request-line syntax validation belongs to the HTTP
  strictness step, S7.

## [0.67.0]. 2026-10-09

### Changed

- **12 files comments are shortened**.
- **Rationale moved into the docs instead of being deleted.** DESIGN.md gains
  "Upstream URLs", "Body edits and top-level readers", "Signing for Bedrock", the
  memory-BIO call protocol, and two smaller additions;
  GATEWAY-INTERNALS.md gains section 9b on how `secure_clear` erases.
- `sse.hpp` no longer claims the stream translator is text-only: it has handled tool
  calls for several releases.

## [0.66.0]. 2026-10-09

### Added

- **Allocation ceilings for the request path** (`gateway_alloc_test`, in ctest).
  Global operator new is replaced and counts only on the event-loop thread; each
  case warms up 20 requests, then averages 50, on both backends.

  | Path | Allocations |
  |---|---|
  | Passthrough request | 3 |
  | Translated request (OpenAI to Anthropic) | 16 |
  | Translated stream, 20 deltas | 59 |
  | Each further stream delta | 2 (2.01 on io_uring) |

  Under TSan nothing is replaced and the ceilings skip: its runtime defines operator
  new and delete itself, and a second definition does not link, which is what failed
  the ThreadSanitizer job's build. They still run under ASan.

- **`bench/protocol_micro`**: p50/p95/p99 of the protocol work outside the loop,
  on inputs shaped like production, not like the bench mocks.

  | Call | p50 |
  |---|---|
  | `parse_response_head`, 26-line ~1 KB Anthropic head | 2.5 us |
  | `anthropic_to_openai_response`, ~400 B body | 1.3 us |
  | `AnthropicToOpenAiSse::feed`, one text delta | 0.35 us |
  | `openai_to_anthropic_request`, 130 KB agent turn | 180 us |

## [0.65.0]. 2026-10-09

### Fixed

- **The two slow-client backpressure tests now test backpressure on any kernel.**
  `ProxyStream.SlowClientEngagesBackpressureAndLosesNothing` and
  `ProxyPoolHygiene.AnUpstreamPooledAfterASlowStreamStillAnswers`.

### Added

- **`Gateway::set_client_sndbuf_for_test(int)`**, a test seam that pins SO_SNDBUF
  on accepted client sockets on both backends, which turns off autotuning.

### Tests

- Both tests above set the seam to 4096 bytes and pass on epoll and io_uring.

## [0.64.0]. 2026-10-08

### Added

- **A policy sees whether an OpenAI client asked to stream, and whether it asked
  for usage.** `RequestFacts` gains `stream` (top-level `"stream": true`) and
  `include_usage` (top-level `stream_options.include_usage: true`). Absent, false,
  not a boolean, or nested anywhere else, such as a tool schema, reads false.
- **`RequestRecord::served_model`**, the model the venue's reply names: the
  top-level `model` of a non-streamed body or of an OpenAI chunk, and
  `message.model` of Anthropic's `message_start`. Read on both backends where the
  served tier is, at most four reads per stream, cut at 64 bytes, never a nested
  `model`. A sink compares it with the model asked, which is how a body naming
  `model` twice is caught for a client whose body is not key-checked (below).
- `provider::top_level_facts`, one walk of the top level returning the model, both
  stream flags and the key verdict; `provider::reply_model`;
  `provider::top_level_member_count`, the bare walk, for tests to price against.

### Security

- **An OpenAI client's body whose top-level keys we could read differently from
  the provider is refused with 400**, before the policy is asked and before any
  upstream is contacted. Our readers compare raw bytes and take the first match; a
  provider unescapes keys and may keep the last duplicate. So
  `"stream":false,"stream":true` read as not streaming here and streamed there, and
  a policy could bill, route or meter a request on a value the provider never used.
  Refused: a repeated key, a key spelled with a backslash escape, or more than 256
  keys, in the top-level object or in `stream_options`. Keys anywhere else are the
  client's schema and pass. The WARN names which rule fired and never prints the
  body; the client gets a fixed message.
- **Any other client's body is read for the model alone and not key-checked.** The
  stream trick cannot apply to it, since its usage is read from the venue's events,
  and a second `model` is caught after the fact by comparing with `served_model`.
  The read stops at the model, so Claude Code, which sends it first, pays about
  0.1 us at any body size instead of a walk of its whole context.
- All of this runs only where the gateway reads the body for a decision, with a
  policy or a sink installed. A stock build reads nothing and checks nothing.

### Performance

- **One walk per request.** The gateway calls `top_level_facts` once, through
  `detail::body_facts`; `model_of`, `wants_stream`, `stream_usage_of` and
  `top_level_key_check` are thin wrappers for other callers. Reading each key on
  its own walked the whole body once per key whenever the key came late or was
  absent, and the OpenAI SDK puts `model` and `stream` after `messages`.
- **String contents are skipped with `memchr`**, and a closing quote is the one
  with an even run of backslashes before it. What remains is dense structure, such
  as tool schemas, read one token at a time.
- Measured on an i7-9750H laptop, Release, median, OpenAI client, a Claude Code
  shaped body (system prompt, tool schemas, messages), keys first / keys last. The
  single pass is within 2% of a bare walk.

  | body | before: four reads, byte by byte | staged: four reads, memchr | now: one pass |
  |---|---|---|---|
  | 2 KB | 2.0 / 9.0 us | 1.2 / 5.6 us | 1.1 / 1.1 us |
  | 30 KB | 20 / 118 us | 11 / 73 us | 11 / 12 us |
  | 200 KB | 134 / 787 us | 74 / 484 us | 73 / 73 us |
  | 800 KB | 549 / 3155 us | 306 / 1942 us | 290 / 289 us |

### Fixed

- **`wants_stream` read `"stream": true` followed by whitespace as not streaming.**
  A bare literal ran to the next `,` or `}`, so a pretty-printed body ending in
  `true\n}` compared as `"true\n"`. The one caller decides whether a request to a
  Bedrock venue is refused for streaming, so such a request was signed and sent
  instead of refused.

### Changed

- Minor: the new fields sit at the end of `RequestFacts` and `RequestRecord` gains
  one, so aggregate initializers written against 0.63 keep compiling.
- Breaking for input only: with a policy or sink installed, an OpenAI client's body
  0.63 forwarded is refused when it repeats or escapes a top-level key. No SDK we
  know emits one.

### Tests

- Both backends: `ProxyPolicy.ThePolicySeesTheTopLevelStreamFlags`,
  `AnAmbiguousTopLevelKeyIsRefusedBeforeThePolicyAndTheUpstream` and
  `AnotherDialectIsServedWithAmbiguousKeysAndNoStreamFlags`;
  `ProxyRoute.TheServedModelIsReadOff{AnOpenAIStream,AnOpenAIBody,AnAnthropicStream,AnAnthropicBody}`,
  `TheServedModelIsEmptyWhenOnlyANestedOneExists` and `TheServedModelIsCutAt64Bytes`.
- `TopLevelKeys`, `TopLevelFacts`, `TopLevelWalk` (escaped quotes, backslash runs,
  structure inside strings, a quote at every offset, unterminated and unbalanced
  input, depth), `StreamUsage`, `WantsStream.WhitespaceAfterTheLiteralIsNotPartOfIt`,
  `ServedModel`, `BodyFacts`.
- **`BodyFactsPerf`**, run by ctest outside the sanitizers, bounds loose enough
  that only a pathological shape trips them: the OpenAI pass under 2x a bare walk
  in the same process (four reads were ~6x), under 1,000 us at 200 KB (73 us
  measured), cost per KB at 800 KB within 3x of 30 KB (a quadratic skip is ~27x),
  and another dialect's 800 KB body within 3x of its 2 KB body.
- `gateway_body_perf_test` prints p50/p95/p99 for both key orders and both client
  dialects; built by default, never run by ctest.
- `fuzz_json` now also runs the top-level readers on every input.

## [0.63.0]. 2026-10-08

### Added

- **A policy can say why it refused.** `Decision::message` is sent as the error
  message of a 4xx refusal; null keeps the status's generic one, and a 5xx never
  shows it. `reason` is still logged and never sent.
- **The message is checked before it is sent**, because it lands in a JSON string
  with no escaping: 1 to 256 bytes of printable ASCII without `"` or `\`. Anything
  else is dropped with a WARN and the generic message goes out; the refusal stands.
  The rule is `deny_message_ok`, public and `constexpr` so a policy can
  `static_assert` its own messages.
- **402 renders as `402 Payment Required`**, type `billing_error`, message
  `payment required`. A policy refusing with 402 used to go out as
  `502 Bad Gateway`, blaming the provider for our own refusal.

### Changed

- Minor: `Decision` gains a field after `reason`, so a policy built against 0.62
  keeps compiling and behaves identically.

### Tests

- `ProxyPolicy.ADenialMessageReachesTheClientWithItsStatus`,
  `AnUnsendableDenialMessageFallsBackToTheGenericOne` (a quote, a backslash, CR LF,
  other control bytes, 0x7F, non-ASCII, empty, one byte over the bound),
  `ADenialWithoutAMessageIsAnsweredAsBefore` and `ADenialMessageIsNeverShownOnA5xx`,
  both backends; `PolicyMessage.AcceptsPrintableAsciiWithinTheBoundAndNothingElse`.

### Known gaps

- A policy status in 400-599 that `error_shape` does not list (405, 409, 422, 500
  and others) still goes out as `502 Bad Gateway`. The seam checks the range, not the
  table.

## [0.62.0]. 2026-10-04

### Added

- **Every token count a venue states now reaches the sink.** `RequestRecord` gains
  `reasoning_tokens`, `audio_in_tokens`, `audio_out_tokens`,
  `accepted_prediction_tokens`, `rejected_prediction_tokens` and
  `tool_prompt_tokens`, -1 when the venue stated none. Read from OpenAI's
  `prompt_tokens_details` and `completion_tokens_details` on both backends, on a
  non-streamed body and on the usage chunk of a byte-forwarded stream, with the two
  `audio_tokens` keys kept to their own blocks; Gemini's `thoughtsTokenCount` and
  `toolUsePromptTokenCount` land in the same fields.
- **Gemini's translated response carries its cache read and thinking count** in the
  OpenAI shape, `prompt_tokens_details.cached_tokens` and
  `completion_tokens_details.reasoning_tokens`, emitted only when Gemini stated them.

### Changed

- **The usage scanner's cost, measured on a 1.6 KB body:** unchanged for an
  Anthropic usage block, +110 ns for an OpenAI block without details (one search of
  the last 700 bytes), +0.5 us for one with them (1,050 ns against 560). The first
  version searched the whole 2 KiB tail for nine absent keys and cost every
  response 0.5 us; the gate is what removed that.

### Fixed

- **`find_fast` no longer hands `memmem` a null haystack.** An absent details block
  reached it as an empty `string_view` whose data pointer is null, which glibc
  declares undefined even at length zero.

### Tests

- `ProxyForwardStream.TheSinkGetsEveryUsageDetailTheVenueStates`,
  `GeminisOwnCountsLandOnTheSameFields` and
  `AnUnstatedUsageDetailStaysUnstated`, both backends; `GeminiResp` gains the
  details round trip and the no-details case.

### Known gaps

- Cohere's billed units and Anthropic's `server_tool_use` request count are not
  token counts and are not recorded.

## [0.61.0]. 2026-10-03

### Fixed

- **Re-resolve address when they rotate or fail generally, for the kernel's SYN 
  retry schedule.**
  - **A connect deadline, `--connect-timeout SECONDS` and `timeouts.connect_s`,
    5 s by default.** A fresh upstream that is not wire-ready (TCP connected and,
    on a TLS venue, handshake done) by then is abandoned on the shared sweep: the
    request fails over when a policy allows it and answers `502 upstream connect
    timeout` otherwise. `Stats::connect_timeouts` counts them. 0 disables it.
  - **A failed connect moves the venue to its next resolved address and re-resolves
    its name.** `Upstream` carries every address the name resolved to (`ips`) and
    the name itself (`host`).

### Changed

- **io_uring dials the address stored on the connection, not a table entry.** The
  connect SQE used to point into `_upstream_addrs`, sized once at startup; a table
  that can now change under an in-flight submission is the wrong place for a
  kernel-held pointer, so each upstream `Connection` carries its own `sockaddr_in`.
- `RequestRecord::upstream_ip`: the address the serving venue was dialled at,
  network byte order, 0 when none was. One name is several addresses and they
  retire, so a record that named only the venue could not say which one it hit.
- `Connection::ts_accepted` is now set on an upstream connection at socket
  creation, which the field's comment had claimed since the stamp was introduced.

### Tests

- `ProxyRoute.AConnectThatNeverCompletesIsAbandonedAtTheDeadline`: a loopback
  listener with a full accept queue drops every further SYN, which reproduces the
  incident without root; the request gets its 502 at the deadline on both backends
  and reads nothing at all without it.
- `GatewayTls.ConnectDeadlineAbandonsAStalledHandshakeWith502`: connected but
  stalled before `SSL_accept`, the state neither the refusal path nor the idle
  timeout covers.
- `ProxyStream.TheConnectDeadlineEndsAtTheWire`: a provider that took the request
  and went quiet is the idle timeout's case; the deadline must stop counting at
  wire-ready or every slow prefill would be a connect failure.
- `ProxyRoute.AFailedConnectMovesTheVenueToItsNextAddress` and
  `AFailedConnectReresolvesTheVenueName`: a refused 127.0.0.2 moves the venue to
  127.0.0.1, by list and by resolving `localhost`.

### Known gaps

- The deadline and the rotation are exercised against a refusal and a full
  accept queue; the retired-address case itself is confirmed only on the hosted
  gateway, against the address the incident named.

## [0.60.0]. 2026-09-21

### Added

- **`net::Sha256`, FIPS 180-4, implemented in `net`.** A streaming `update`/`finish`,
  a one-shot `hash`, and `truncated`, the leading eight bytes as a big-endian integer,
  for a caller whose field is 64 bits wide.

### Changed

- **`RequestFacts::prefix_hash` is SHA-256 truncated to 64 bits. It was FNV-1.**
  Same contract, an identity of the leading `kPrefixHashBytes` of the body, 0 when
  the policy did not ask or the body is empty, and a different number for the same
  bytes.

### Tests

- `net_sha256_test`: the published vectors including the million-`a` long one,
  the same digest for any split of the input, every length around the padding
  boundaries, reset after `finish`, truncation, and empty updates.
- `net_sha256_ni_test`: `sha256_rounds.inc` included against a scalar model of
  `SHA256RNDS2`, `SHA256MSG1` and `SHA256MSG2` written from the operand tables in
  Intel's Software Developer's Manual.

### Known gaps

- **The intrinsics have not executed on real silicon in this tree.** The reference
  host and the CI runners have no SHA-NI, so the fast path has only run through the
  scalar model.

## [0.59.2]. 2026-09-19

We follow u/verstands suggestions to add CI tests that caputre silent degradation
we introduced in header walks. We are thankful for the useful suggestions:
https://www.reddit.com/r/LLMDevs/comments/1wi4uvq/comment/paq1asf/?utm_source=share&utm_medium=web3x&utm_name=web3xcss&utm_term=1&utm_content=share_button

### Tests

- **Two header/body scans had shipped with no test reaching them directly** 
  The one-pass credential scan that replaced six `find_header()` walks
  moved out of its anonymous namespace and declared in `request.hpp` so it
  is testable in isolation, same pattern as `request_without`.
  New: header order and duplicates must not change the resolved credential (a full
  permutation test), first-duplicate-wins holds across case, a stripped header
  stays invisible, a control byte in a value is refused, and one golden fixture
  pins the exact serialized bytes for a request with a mixed-case duplicated
  `x-api-key`, a duplicated `anthropic-version` and a bearer alongside.
- **Timing for the credential scan, split by looseness.** In the PR-blocking suite,
  one sanity bound: under 2,000 ns per call averaged over 200,000, an order of
  magnitude over the ~135 ns measured.

## [0.59.1]. 2026-09-16

### Fixed
- A request to a Bedrock venue whose bearer is not an AWS access key pair is now
  refused with `refuse::kBedrockCredential`, which says what the venue signs with. It
  used to share the "control characters are refused" string with the byte check, which
  was false for the case a caller actually hits: a Bedrock API key in the bearer slot.
  Same status, same fail-closed path, and the value is still never echoed.

## [0.59.0]. 2026-09-14

### Added
- `RequestFacts::seq`: the gateway's request sequencer.

## [0.58.0]. 2026-09-14

### Added
- `RequestFacts::prefix_hash`, **off by default**

## [0.57.0]. 2026-09-12

### Added
- `RequestRecord::venue_req_id`: the venue's own id for a request, read from its
  response head. Three spellings are tried in order, `x-amzn-RequestId`,
  `request-id`, `x-request-id`, because a request may reach any venue and a
  Claude-via-Bedrock response carries the AWS id and Anthropic's together. The AWS
  one wins, since it is what CloudTrail indexes.

## [0.56.0]. 2026-09-11

### Added

- **OpenAI's cache-write count is recorded.** The GPT-5.6 family began charging
  1.25x the uncached input rate to write a cache entry.

## [0.55.5]. 2026-09-06

### Performance

- **The ciphertext buffer keeps headroom and rides the warm list.**

## [0.55.4]. 2026-09-06

### Fixed

- **`cold_builds` and `warm_reuses` are in the profile every embedder prints.**

## [0.55.3]. 2026-09-06

### Fixed

- Tests only: check on kWarmBufs instead.

## [0.55.2]. 2026-09-06

### Performance

- **A new upstream connection no longer costs the next request a cold buffer.**

## [0.55.1]. 2026-09-05

### Fixed

- **`--prefault-mb` no longer touches a fresh upstream connection's send buffer.**
  v0.55.0 wrote through `prefault_mb` on every connection it opened, calling that
  off the request path. It is not: the touch runs inside the request that opened the
  connection.

## [0.55.0]. 2026-09-04

### Added

- **`--prefault-mb` (config: `runtime.prefault_mb`).** The retained buffers are still
  cold the first time each is written, and every new upstream connection brings a
  fresh one. With the knob set, the two scratch strings are reserved and written
  through before the loop starts and each upstream's send buffer when its connection
  opens, off the request path. A reserve alone maps nothing, so the pages are touched,
  and a test proves it with `mincore`. Memory is the size times the pooled upstream
  connections, so the default is 0.

## [0.54.2]. 2026-09-04

### Performance

- **Every upstream request is built in a buffer the gateway keeps.** The byte-forward
  rebuild, the translated body (`openai_to_anthropic_request` and the Bedrock form gain
  an overload that writes into a caller's string), the route-override rewrite
  (`apply_overrides`, same) and the HTTP framing each wrote into a fresh string of
  exactly the right size, which then replaced the pooled upstream's `wbuf` and freed the
  last one. This is fixed.

## [0.54.1]. 2026-09-04

### Changed

- **`provider/src/translate.cpp` is four files.** One per venue, `translate_anthropic.cpp`
  (tools, the Messages walk Bedrock shares, the response), `translate_gemini.cpp` and
  `translate_cohere.cpp`, plus `translate_body.cpp` for the edits that do not depend on
  the venue: the top-level walk behind `model_of`, the model and service-tier rewrites,
  and the upstream error envelope. The `content` helpers every request walk shares
  moved to `content.hpp`, private to `src/`, still inline.

- **`gateway/src/gateway.cpp` is `Gateway` and its event loops, and nothing else.** The
  1,500 lines of free helpers in front of it moved out, all under
  `gateway/src/` and none installed: `request.hpp/.cpp` (dialect resolution, the
  byte-forward rebuild, the translated request with its credentials, the response
  translated back), `response.hpp/.cpp` (error envelopes, relayed statuses, 
  the SSE head, timing and usage headers), `scan.hpp` (the bounded usage and 
  error scans) and `stream.hpp` (the streaming step both backends share).

- **The two backends are two files.** Every `ep_` method and `run_epoll()` are in
  `gateway_epoll.cpp`, every `ur_` method and `run_uring()` in `gateway_uring.cpp`
  (empty without `LLMBRIDGE_HAVE_URING`), and `gateway.cpp` keeps construction, the
  shared methods and `run()`. What both loops read moved to `loop.hpp`. The
  convention checker reads the three files as one text, so a call across the
  prefixes still fails the build, now naming the file it is in.

- **`gateway/gateway.hpp` is `Gateway` and `Stats`; the rest is `connection.hpp`.**
  The refusal strings, the dialect and backend enums, `TlsConfig`, `Upstream` and
  `Connection` moved to the new header.

## [0.54.0]. 2026-09-04

### Changed

- **OpenSSL writes ciphertext straight into the send buffer.** The write BIO is now
  the session's own method: each finished record is appended to a sink the gateway
  sets for the duration of a push, `Connection::tls_out`, instead of being held in a
  memory BIO and copied out again through a 16 KB stack buffer
  
- **The ciphertext buffer for an upstream request is reserved once.**

## [0.53.0]. 2026-09-03

### Added

- **`Expect: 100-continue` is answered.** The gateway already stripped the header on the
  way upstream, which is right since a 1xx cannot be relayed, but sent nothing back.

### Performance

- **A large request body was copied four times and its headers parsed a thousand
  times.** Extra two were `rbuf` reallocating as it grew toward the body in 4 KB 
  recv steps. Beside it, every one of those ~1,000 recvs re-ran `parse_request` 
  on headers whose answer could not change.

  `parse_request` already computes the total length before it reports `NeedMore`, so
  the framing loop now remembers it now. `rbuf` is reserved to it once, and the headers
  are not re-parsed until the bytes are in. A test covers the case the memo could break,
  which is a large request followed by a smaller one on the same socket.

## [0.52.2]. 2026-09-03

### Performance

- **A streamed request spent 65 us in substring searches.** Replaced find by memmem.

## [0.52.1]. 2026-09-03

### Fixed

- **`accept(TLS)` could not report its own tail.**

- **The test suite waited out a timeout on every clean stream.** Now that a clean stream
  ends with a chunk terminator instead, every streaming test sat until its receive
  timeout expired.

## [0.52.0]. 2026-09-03

### Changed

- **A streamed reply no longer closes the client's connection.** The client-facing stream 
  is now framed with `Transfer-Encoding: chunked`, so the terminating zero-length chunk
  ends the body and the connection survives.

  **Only for a caller that can use it.** `Message::http_1_1` is new: chunked encoding
  does not exist before HTTP/1.1, so a 1.0 caller still gets the close-delimited stream.

### Fixed

- **An HTTP/1.0 client asking for keep-alive was closed on anyway.**

## [0.51.0]. 2026-09-03

### Fixed

- **A request body carrying no `messages` array was translated into an empty
  conversation and forwarded.**

- **An unrecognised command-line argument was ignored instead of refused.** Every flag
  matched exactly, so `--listen=8080` fell through the parse loop in silence and the
  gateway served the default port against the default upstream.

### Changed

- **`--io auto` says so when it falls back to epoll.**

### Added

- **`SIGUSR1` prints the latency profile without stopping the gateway.**

- **Per-request visibility of client connection reuse:** `client_conn_reused` and
  `client_conn_setup_ns` on the sink record, set on both backends.

### Changed

- **The inbound and upstream legs are now named apart.**

### Performance

- **Two unbounded per-chunk scans on a byte-forwarded stream.** Neither affects a
  translated stream.

- **One allocation per SSE event removed.**

## [0.50.1]. 2026-09-02

### Fixed

- **A translated request never recorded the served tier.** Both capture calls sat in the
  passthrough branch. Three of the four corners of (streamed, translated) had tests and
  the fourth had none, which is the corner the fix moved into; all four are covered now,
  on both backends.

## [0.50.0]. 2026-09-02

### Fixed

- **The served-tier search was bounded on the wrong axis.**

- **The head window missed a venue that names the tier once.**

## [0.49.0]. 2026-09-01

### Added

- **`RequestRecord::served_tier`: the tier the venue says it served.** `asked_tier` and
  whatever a policy writes are both requests.

  **The two reply shapes put it at opposite ends, so one window cannot serve both.**

### Changed

- **`json_string_at` factored out of `scan_error_type`.** One reader for a JSON string
  field, used by the error type, the error code and the served tier. No behaviour
  change, and the error-type tests cover it.

## [0.48.0]. 2026-09-01

### Added

- **`RequestRecord::from_pool`: this request reused its upstream connection.**

## [0.47.0]. 2026-09-01

### Added

- **`RequestRecord::upstream_error`: the venue's own name for a failure.**

  **The finest name the venue offered.** Anthropic states a `type`
  (`invalid_request_error`, `overloaded_error`); OpenAI states a `type` and often a
  `code`.

  **Never the `message`.** It is free text the provider composes, it can quote the
  request back, and this view reaches a sink that keeps it. The type and code are
  fixed vocabularies the provider chose from.

## [0.46.0]. 2026-09-01

### Added

- **An authentication refusal names the address it came from.** This endpoint is
  public, so 401s arrive from people who are not customers, and a connection id names
  them only within one process lifetime.

  **Only 401 and 403.** Every other 4xx is a customer getting a request wrong, where
  the connection id is enough and an address on every line is noise.

## [0.45.1]. 2026-09-01

### Fixed

- **`memcpy` from a null pointer when the caller named no service tier.** The common
  case: an empty `string_view`'s `data()` is null, and `memcpy` is declared `nonnull`,
  so a zero length does not excuse it.

- **A test read a sink view after the connection holding it was freed.** `asked_tier`
  is valid only during `on_request`, like every other view on the record, and the
  recording sink in the tests copies `model` and the captures out for exactly that
  reason.

## [0.45.0]. 2026-09-01

### Added

- **A policy can set the request's `service_tier`.** `Decision::service_tier`, applied
  on the byte-forward path only, because a tier is an OpenAI-dialect field and every
  venue that reads one already speaks the caller's dialect.

- **`provider::upsert_string`**, which is what makes that possible. `rewrite_model` can
  only replace, since a request with no model is not one this gateway serves; almost no
  caller sends a tier, so the route has to be able to insert one.

- **`provider::apply_overrides`** does both splices in one parse, and the gateway calls
  only it. Measured, because the cost is not where it looks: on a 4 MB body (about a
  million tokens) parsing costs 4,313 us and copying the whole string 164 us, so the
  splice position is nearly free and the parse is everything. Two calls cost 9.6 ms
  against 4.7 ms for one, and a caller sending `model: "auto"` pays it on every
  request. Timed at 1 KB through 16 MB; the cost is linear in body size, as it must be.

- **`RequestRecord::asked_tier`** carries what the caller's own body said. The route
  wins, which is right because the route is what was priced.

### Changed

- Minor: `Decision` and `RequestRecord` each gain a field, so a policy or sink built
  against 0.44 keeps compiling and keeps behaving identically. Nothing sets a tier
  unless a policy asks.

## [0.44.2]. 2026-09-01

### Fixed

- **A null reasoning field is no longer read as reasoning.** `thinking_delta` and
  `redacted_thinking` are type names, so their presence is the signal;
  `reasoning_content` is a field and not a type, and an OpenAI-compatible server that supports
  reasoning sends it on every delta whether the model reasoned or not. DeepInfra sends
  `"reasoning_content":null` four times in a two-token Llama reply, so the scan matched
  the first chunk of every such stream and stamped the reasoning as beginning there.

## [0.44.1]. 2026-08-31

### Changed

- **A plaintext listener says so, loudly, at startup.** The upstream leg has always
  warned when it is plaintext; the inbound leg, which carries the client's own
  credential, said nothing.

- **A credential header can never be captured into a sink.** `set_request_sink` took
  whatever header names the integrator asked for, so asking for `authorization` would
  have copied the bearer token into every record, and a sink writes to disk.


## [0.44.0]. 2026-08-31

### Added

- **`max_chunk_gap_ns`: the longest silence in a stream after the first visible
  token.**

## [0.43.1]. 2026-08-31

### Fixed

- **TTFT stamped at the empty role-priming chunk on OpenAI-dialect byte-forward
  streams.** OpenAI opens a stream with `{"role":"assistant","content":""}`, and the
  first-token match keyed on the substring `"content":"`, which the empty value
  contains. So `ts_first_token` was stamped at the response head on every
  OpenAI-dialect stream, collapsing TTFT to TTFB and, since OpenAI reasoning models do
  not stream a reasoning marker, making the whole prefill/generation split wrong for
  the dialect.

- **`ts_first_thinking` never matched `redacted_thinking`.** Anthropic emits a
  `redacted_thinking` block instead of `thinking_delta` when it safety-filters the
  chain of thought.

### Known limitation

- A reasoning or content marker split exactly across two reads is missed by the
  per-read substring search and stamps late or not at all.


## [0.43.0]. 2026-08-29

### Added

- **`ts_first_thinking`, stamped on the first reasoning delta.** A thinking model
  streams its reasoning before anything the caller is shown, and `ts_first_token` is
  stamped on a text, content or tool delta, which a `thinking_delta` is not.

  **Both dialects.** Anthropic streams `thinking_delta` and an OpenAI-dialect upstream
  streams `reasoning_content`.

## [0.42.0]. 2026-08-27

### Added

- **A heartbeat line, every five minutes at Info: `clients`, `in_flight`,
  `pooled_upstreams`, `requests`.**

  It hangs off `sweep_idle`, which both backends already call on the same 200 ms tick,
  so there is no new timer and nothing to keep in step between the two loops. One
  timestamp comparison per sweep against one `write(2)` every 300 s. The first line
  lands a tick after boot instead of an interval in, so the mechanism proves itself
  without a five minute wait.

  `--heartbeat` is not a flag yet; `set_heartbeat_ns(0)` silences it.

## [0.41.0]. 2026-08-27

### Added

- **`RequestFacts::model`: the model the client asked for, at the policy seam.** A
  routing decision keyed on the model had no way to see one.

### Changed

- **`RequestFacts` gains a field, which is a source break for positional
  initialisation.** `RequestFacts{head, bytes}` still compiles; a build with
  `-Werror=missing-field-initializers` refuses it, and that is how the two in-tree
  consumers surfaced. Designated initialisers and `{}` are unaffected.

## [0.40.0]. 2026-08-27

### Added

- **`cache_write_5m_tokens` and `cache_write_1h_tokens`: the cache-creation write split
  by the entry's lifetime.** v0.39.0 reported the total, and the total cannot be costed:
  Anthropic bills a five-minute entry at 1.25x the input rate and a one-hour entry at
  2x.

  Read from `usage.cache_creation` (`ephemeral_5m_input_tokens`,
  `ephemeral_1h_input_tokens`) on all four paths: the SSE translator parses the nested
  object, `scan_usage` finds the keys in a byte-forwarded stream's tail, and both
  non-streaming paths read them per backend. Both stay -1 when the provider states a
  total and no breakdown, which is not a breakdown of zero.

  On a translated non-streaming response they come from the provider's own body.
  OpenAI defines no field for a cache-creation write, so scanning the translated body
  finds nothing and reports -1 without saying so.

### Fixed

- **v0.39.0's `cache_write_tokens` was never assigned on the non-streaming path.** The
  field was declared and read, and nothing set it, so every non-streamed request
  reported -1.

- **A keep-alive client's request could report the previous request's token counts.**
  `tok_in`, `tok_out` and `tok_cached` are assigned only where a response body is
  scanned and were missing from the per-request reset, so a second request that fails
  before the scan carried the first one's numbers. The streaming twins were reset for
  exactly this reason. A token count is a bill, not a log line.

## [0.39.0]. 2026-08-27

### Added

- **`cache_write_tokens` on the sink record: the input tokens written to the
  provider's prompt cache.**

## [0.38.0]. 2026-08-26

### Added

- **`request_bytes` and `client_encoded` on the sink record.**

### Changed

- **Breaking: a request body with a `Content-Encoding` other than `identity` is
  refused with 415, and the upstream is never contacted.**

## [0.37.0]. 2026-08-26

### Added

- **`client_upload_ns`: how long a request took to arrive.** Measured from the 
  first byte of the request this gateway saw to the last, on the read path so it 
  covers plaintext and TLS on both backends. Reported to a sink as
  `client_upload_ns` and as `x-llmbridge-client-upload-us`.

## [0.36.0]. 2026-08-26

### Added

- **`cache_control` survives the translation.** Anthropic caches what precedes a block
  marked `cache_control` and nothing at all without one.

### Changed

- **Breaking: the JSON DOM's nodes live in one arena per document, and a `Value`
  cannot be copied.** `Value::arr` and `Value::obj` are `Span`s over arena storage
  instead of `std::vector`s, object members are a `Member` struct and no longer a
  `std::pair`, and the copy constructor is deleted.

## [0.35.4]. 2026-08-26

### Changed

- **Two fewer passes over the request body.** Both are per-byte work on a path that
  a coding agent hits with a few hundred KB per turn.

  `request_without` inserted `Host` and `Content-Length` just after the request line
  **after** the body was already in the buffer, and an insert near the front memmoves
  everything behind it. The head is assembled first now and the body is appended once.

  The failover copy saved the whole request so a failed venue could be retried, and it
  ran the moment a deployment had more than one venue, which is every routed one. The
  read buffer is cleared on the next line either way, so it is handed over instead of
  copied.

- **Breaking: `openai_wants_stream_usage` is gone, and nothing scans a body to
  answer it any more.** `stream_options.include_usage` is now an optional output of
  `openai_to_anthropic_request`.

- **A byte-forwarded request no longer scans its own body for an OpenAI field it
  cannot contain.**

### Fixed

- **A tool call now counts as the first token on a byte-forwarded stream.** An agent
  turn whose entire output is a tool call emits no text delta ever, and the
  byte-forward path looked for `"content":"` or `"text_delta"` and nothing else.

## [0.35.3]. 2026-08-25

### Changed

- **The `stream` flag is read only where it can change the answer, cutting the
  request path 3x on an agent-sized prompt.**

  `wants_stream` also rejects cheaply before walking: a body with no `"stream"`
  anywhere cannot have a top-level one, and `find` runs at memchr speed. The nested
  case is still decided by the parser, which a test pins.

## [0.35.2]. 2026-08-25

### Fixed

- **A same-dialect stream was translated, so an Anthropic client got OpenAI chunks.**
  The SSE translator was installed by asking the venue's dialect alone, and the venue
  is Anthropic whether or not we translate into it.

## [0.35.1]. 2026-08-25

### Fixed

- **A byte-forwarded reply reported no tokens.** Only the translating branch scanned
  the response body, so a same-dialect request left `tokens_in`, `tokens_out` and
  `cached_tokens` at -1. The counts now come from the venue's own body on that path too,
  the same `scan_usage` the translated path has always used, on both backends.

  The streamed twin of this was fixed in v0.28.0 by scanning the stream tail; the
  non-streamed one was missed.

## [0.35.0]. 2026-08-25

Breaking, and deliberately loud: the field that says what a venue speaks was called
`translate`, and its OpenAI value was called `none`. Both named an action the gateway
often does not perform, and that misreading cost a day.

### Fixed

- **A failover test could fail in CI because its dead venue came alive.** The helper
  that picked an unreachable port bound an ephemeral one, read the number and closed
  the socket, so between that and the gateway's connect the port belonged to whoever
  asked first. Under `ctest -j` that is sometimes another test's backend: the venue
  answered, no failure fired, and the assertion read as a routing bug. It failed once
  in CI, on one backend, which is what a race looks like.

  `DeadPort` holds the socket bound and never listens. Linux answers a connect with
  ECONNREFUSED when there is no listen queue, and a second bind gets EADDRINUSE, so
  the venue stays dead and the port stays reserved. Both properties are measured in a
  test, and both fail if the helper goes back to closing the socket.

### Changed

- **`TranslateMode` is `UpstreamDialect`, and `None` is `OpenAI`.** The field is a
  property of the far end, like its URL and its auth scheme, and it decides three
  things: the target path, the credential header, and the body shape.

- **`--translate` is `--upstream-dialect`; the config key `translate` is `dialect`.**
  Both old spellings are refused by name, with the replacement in the message, rather
  than accepted as aliases.

- **An unknown dialect is refused instead of defaulted.** `--translate anthropc`
  silently became an OpenAI venue, so a typo turned an Anthropic upstream into a
  mistranslated request carrying a live credential.

- **`TranslationPlan` carries `translate` and `venue`, where it carried one enum.**
  This is the part that makes the rename honest instead of cosmetic. The old field
  answered two questions with one value: which dialect, and whether to translate at
  all.

## [0.34.0]. 2026-08-25

A gateway that refuses a request now says why, and stops signing one it knows the
venue cannot serve. Breaking: `resolve_translation` takes a third argument.

### Fixed

- **A streamed request to a Bedrock venue is refused instead of sent.** Bedrock
  streams from `/model/{id}/invoke-with-response-stream` in AWS event-stream framing,
  which is a different endpoint and a different wire protocol from the `/invoke` this
  builds.

- **Every refusal is logged, at WARN.** `error_respond` logged 4xx at `LB_DEBUG`, and
  `LB_DEBUG` sits below `LLMBRIDGE_LOG_COMPILE_LEVEL` in every build, so it was
  compiled out and `--log-level debug` could not bring it back.
  
- **A failover test could fail in CI because its dead venue came alive.** The helper
  that picked an unreachable port bound an ephemeral one, read the number and closed
  the socket, so between that and the gateway's connect the port belonged to whoever
  asked first.

### Added

- **`provider::wants_stream`**, whether a body asks for a streamed response. The
  gateway otherwise learns that a response streams from the response itself, which is
  too late to refuse a venue that cannot stream at all.

### Changed

- **Breaking: `resolve_translation(client, venue, stream)`.** The third argument has
  no default on purpose: a venue can speak the right dialect and still be unable to
  serve a shape of request, and every caller should have to say which it is asking
  about.

## [0.33.0]. 2026-08-25

### Changed

- **Anthropic token usage is normalized to the OpenAI convention, so `tokens_in` means
  the same thing whatever the venue.** OpenAI reports `prompt_tokens` as the whole prompt
  with `cached_tokens` a subset of it; Anthropic reports `input_tokens` as the fresh part
  only, with cache reads and the cache-creation write counted separately. The scanner now
  adds those back: `in` is the whole prompt (fresh + cache read + cache write) and `cached`
  is the read subset of it, so `in - cached` is the full-rate part on both dialects.

  The same normalization applies on the **translated** path (OpenAI-in mode): the
  Anthropic->OpenAI response and stream translators now emit `prompt_tokens` as the whole
  prompt, so a translated Anthropic response agrees with a byte-forwarded one and an
  OpenAI client sees the `prompt_tokens` its own convention expects (cached included).

  This changes the recorded value: a cache-heavy Anthropic request (every Claude Code turn)
  previously logged a tiny `tokens_in` (the fresh part) with thousands of cache reads
  reported only in `cached`; it now logs the whole prompt in `tokens_in`. A consumer that
  read `tokens_in` as "the billable fresh input" for Anthropic must switch to `in - cached`.
  The premium cache-creation write is folded into `tokens_in` but not separated (no field
  for it), so first-turn cost is still undercounted.


## [0.32.0]. 2026-08-24

### Added

- **The model the client asked for reaches the sink.** A sink knew which venue served
  a request and had no way to know what was bought, so an integrator recording a
  request could only name the product its own routing intended, never the one the
  body named. Those differ whenever a client picks its own model, which is what every
  coding agent does.

  `RequestRecord::model`, copied at framing like the captured headers, bounded to 64
  bytes and dropped instead of truncated: a sink compares this against configured
  names, and half a name matches none of them, so a truncation would be
  indistinguishable from a real disagreement.

- **`provider::model_of`**, the top-level `model` of a request body, as a view.

  A parser, not a search. A quote inside a JSON string is escaped, so prompt text
  cannot forge a key, but a nested key is unescaped and can: a tool schema with a
  `model` property, or a metadata block, puts a real `"model"` in the bytes ahead of
  the request's own, and the first textual match takes it. The tests attack exactly
  that, and a naive `find` fails six of them.

## [0.31.0]. 2026-08-24

### Added

- **Dialect resolution is wired into the request path (v0.30.0 added the resolver).** The
  gateway resolves the translation per request from the client dialect and the venue's,
  on both backends. The mode is resolved at forward time and re-resolved on a failover
  that lands on a venue of a different dialect, stored on the client, and read by both the
  request and response legs. An Anthropic client (Claude Code) to an Anthropic venue now
  byte-forwards and gets a native Anthropic response instead of a mistranslated OpenAI one;
  an unbuilt client/venue pair (the Anthropic-in direction) is refused with a 400 and the
  upstream is never contacted. OpenAI-client behaviour is unchanged, because the resolver
  returns the venue's own mode for an OpenAI caller. Tests cover both new paths on both
  backends, and the refutation reproduces the exact OpenAI-shaped reply the old code sent
  an Anthropic client.


## [0.30.0]. 2026-08-24

### Added

- **Dialect resolution: the translation to run is a function of the client dialect and
  the venue dialect, not the venue alone.** `TranslateMode` folded two questions into one
  field: what the venue speaks, and what to translate. `Anthropic` did not mean "this
  venue speaks Anthropic", it meant "translate an OpenAI client to Anthropic", so a caller
  that already speaks Anthropic was mistranslated into an OpenAI response it could not
  parse. A new pure resolver (`gateway/dialect.hpp`) separates the axes: the client
  dialect is read per request from the target (`/v1/messages` is Anthropic,
  `/v1/chat/completions` is OpenAI), the venue dialect is a static property, and
  `resolve_translation` maps the pair to a mode. Same dialect byte-forwards; an OpenAI
  client keeps the existing path; an unbuilt pair (the Anthropic-in direction) is refused,
  not mistranslated. Bedrock and Azure stay OpenAI-client-only because they bundle SigV4
  and URL rewrites over their body dialect. See DESIGN.md "Dialect resolution".

## [0.29.1]. 2026-08-24

### Fixed

- **A successful passthrough said `HTTP/1.1 200 Client Error`.** The reason-phrase
  table listed only failures, so a 200 fell to its `status < 500` default.
  Nothing broke, which is why it survived: RFC 9110 says clients ignore the reason
  phrase, and the existing tests read the status code.

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

- **`quota_exhausted` and `retry_after_s`: which limit refused a request.**

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
