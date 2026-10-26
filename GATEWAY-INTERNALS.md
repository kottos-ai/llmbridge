# Gateway internals: one request lifecycle, two backends

How a request actually moves through `llmbridge`, on both event-loop backends.
`DESIGN.md` is the architecture summary and `LATENCY.md` defines every timing
number; this document is the mechanism: which buffer holds what, who owns each
piece of mutable state, when a connection may be freed, and where a credential
is scrubbed.

The lifecycle is written once, in `gateway/src/core/`, as methods of
`detail::Engine`, and reaches a socket only through `IoOps`. Each backend,
`EpollIo` (`gateway/src/io/epoll_io.cpp`) and `UringIo` (`gateway/src/io/uring_io.cpp`),
owns its loop and its syscalls and calls back into the shared methods as completions
arrive. `Gateway` is a facade over the `Engine`. What still differs between the
backends is how I/O is issued and how completion is discovered (sections 4 and 5),
and the threshold at which a slow streaming client pauses its upstream (6c).

## 1. The cast

Two connection kinds, and the vocabulary is used consistently everywhere:

| | |
|---|---|
| **client conn** | a customer's connection to us: `ClientConn` |
| **upstream conn** | our connection to the provider: `UpstreamConn` |

Both derive from `ConnBase` (`gateway/src/core/conn.hpp`), what a socket needs on
either leg: the fd, the buffers, the TLS Session and the backend's flags. The request
and its stream live only on the client; the venue, the response framer and the pool
links only on the upstream, which keeps a pooled upstream under 512 bytes (a
`static_assert` holds it). A backend learns the kind of an event from its tag, the
low bits of epoll's event data or of io_uring's `user_data`, and calls a typed
handler; shared code holding a `ConnBase` reaches the concrete type through `visit()`.

They are linked by `ClientConn::up` and `UpstreamConn::client` for the duration of one
request, and unlinked when it completes. An upstream with no client is either idle in
the pool or dying.

## 2. The buffers

Both are named from the **gateway's** point of view on that socket: `rbuf` is what
we read from the peer, `out` is what we write to it. The same field name
therefore holds the request on one connection and the response on the other, and
which is which depends on the kind. That is the most confusing thing about the
data model, so here is one request crossing all four:

```
   client                        gateway                        provider
     |                                                             |
     |  request bytes                                              |
     |------------------> c->rbuf ---[frame, translate]---> u->out |
     |                                                     |-------|--->
     |                                                             |
     |                                                    response |
     |  c->out  <---[translate back]--- u->rbuf <-------------------|
     |<------------------|                                         |
```

| buffer | on a client conn | on an upstream conn |
|---|---|---|
| `rbuf` | the request the customer sent | the response the provider sent |
| `out` | the response we send the customer | the request we send the provider |
| `tls_out` | ciphertext heading for the socket (TLS conns only) | same |

`out` and `tls_out` are `OutBuf`s (`gateway/src/core/outbuf.hpp`, 2b). Both `rbuf`
and `out` are **plaintext, always**, whether or not TLS is in use.
TLS interposes at the socket edge only, so HTTP framing, the SSE pump and the
stale-connection resend never know it exists.

### Sent is not always "on the wire"

What `out` counts as sent shifts with the transport, and this is a real trap:

- **plaintext**: bytes actually written to the socket
- **TLS**: bytes fed into the `Session`, which have not necessarily left the
  machine. Wire progress is `tls_out`'s

So on a TLS connection `out.idle()` means "fully encrypted", never "fully sent".
Ask `tls_out_flushed()` for the second question.

### 2b. `OutBuf`: stage, wire, pin

| | |
|---|---|
| `stage()` | where new bytes go. Never behind bytes already on the wire: a drained front is reset first, keeping its capacity. Every builder appends here |
| `wire()` | what the transport writes next, out of the front |
| `pin()` / `sent(n)` | an io_uring send holds the front from submit (`UringIo::submit_send`) to completion (`UringIo::on_send`); epoll never pins, so there it is one buffer |
| `take()` | everything staged, for a resend: moved, or copied while a send still reads it |

While the front is pinned, `stage()` is the back half, and `sent()` folds it in when
the send completes, so streaming output never moves memory the kernel is reading and
neither half loses its capacity.

The front keeps what was sent until it is staged over, cleared or taken. That is
what keeps an upstream request available for a resend when a pooled connection
turns out to be dead: `retry_upstream` `take()`s it, and
resend only while the request is `Dispatched` (3c).

## 3. Request lifecycle, common to both backends

```
 accept
   |
   v
 read client bytes ---> c->rbuf
   |
   v
 frame HTTP/1.1 (net/http.hpp)      <- refuses malformed input, never repairs it
   |
   v
 translate OpenAI -> provider dialect (provider/src/translate_*.cpp)
   |
   v
 acquire upstream: pooled keep-alive, or a fresh connect
   |
   v
 write u->out   (credential mapped: Bearer -> x-api-key / x-goog-api-key)
   |
   v
 read provider bytes ---> u->rbuf
   |
   +--- text/event-stream 200 ---> STREAMING PUMP (section 6)
   |
   v
 frame response, translate back, write c->out
   |
   v
 finish: release the upstream to the pool, or close it
```

Everything above is shared. What differs is **how the I/O is issued and how
completion is discovered**, which is section 4 and section 5.

### 3b. One reset per request

A request's state is `Connection::req` (`gateway/src/core/req.hpp`): `ReqState`, the
trivially copyable part (sequence number, t0-t4 and the streaming stamps, usage,
the policy's overrides and every bounded copy for the sink), and the decoders and
buffers kept across requests. `RequestCtx::begin()` is the only reset. It runs at
framing, as soon as the parser answers, and before any reply, a 400 included, so
nothing a request reports can belong to the one before it on the connection. It
constructs a fresh `ReqState` in place rather than assigning one, which writes only
the scalars and leaves the string buffers' bytes alone. Strings the request keeps
are `FixedStr` copies, never views: the policy's `model`, `service_tier`, `speed`,
`inference_geo` and `anthropic_beta` are copied at the decision (a longer one, or a
beta that fails `beta_token_ok`, is refused with a 500, never cut), and a failover
takes `Retry`'s. So is `credential_header`, the family header holding the
attempt's key, once `key_header_ok` passes it (a name outside the family is a 400);
the key itself stays in the head, found through the spans `HeadFacts` recorded.

### 3c. Phases

`ReqState::phase` says where a request stands, and the gates read it, not the buffers:

| phase | entered | what it allows |
|---|---|---|
| `Idle` | at framing (`begin()`), and when a reply finishes | a new request may be framed |
| `Dispatched` | `*_forward`, before the upstream is acquired | retry on a fresh connection, failover to another venue |
| `Responding` | the first response byte in the upstream's `rbuf` | neither: the venue may have acted on the request; a failure is reported to the policy, `retryable` false |
| `Streaming` | the SSE head is staged | the stream finishes when it ends |
| `Replying` | a whole reply is staged | the reply finishes once it is on the wire |

A write event with no reply staged, such as a drained interim `100 Continue`, finishes
nothing. An error once a reply has begun, or with bytes still owed to the client,
closes the client instead of staging a second reply into the first.

### 3d. The seam: `IoOps`

The shared lifecycle reaches a socket only through `detail::IoOps`
(`gateway/src/core/io.hpp`), which each backend implements (`EpollIo`, `UringIo`).
A backend's `run()` attaches itself before the first event, so a shared method such as `error_respond`,
`acquire_upstream`, `retry_upstream`, `release_upstream`, `upstream_failed`,
`abort_pair`, `begin_stream`, `stream_on_upstream_eof` or `finalize_stream` is
written once and runs on both.

| operation | epoll | io_uring |
|---|---|---|
| `send` | write now; `Queued` arms `EPOLLOUT` | one send SQE; always `Queued` unless nothing is owed |
| `connect` | non-blocking `connect()`, `EPOLLIN` and `EPOLLOUT`; an error known at once is its return | socket, a connect SQE; a full queue returns `EAGAIN` |
| `set_read` | `EPOLLIN` off or on | cancel the multishot recv; re-arm it on resume |
| `close` | `Engine::forget`, then `::close` | `Engine::forget`, then cancel; freed at `inflight == 0` |

`connect` returns 0, or the errno that stopped it with nothing open (an epoll set that
would not take the socket included), and `connect_error_is_local` sorts that errno: out of
descriptors or memory, or a full submission queue or epoll set is this host's, answered 503
with no failover and nothing told to the policy, its reason naming the errno
(`local_connect_why`) and counted in `upstream_local_failures`; anything else is the venue's, which moves
it to its next address, asks for its name again (section 8) and fails over. An error in a
connect's completion goes through `on_connect_error`, the same rule on both backends, and a
stale resend whose fresh connect fails for this host's reason is a 503 too.
A send that completes at once is a return value (`Sent::Done`), never a callback, so
no handler runs re-entered. `IoCaps` says how the backend bounds a stream for a slow
client: both pause its upstream, each at its own threshold, and io_uring keeps a cap
as the ceiling (6c). Nothing else of the lifecycle is written per backend: each loop reads or
learns of a completion and calls a shared handler.

The request side is shared outright. Bytes a backend reads go to `on_client_bytes`:
`try_frame` stamps t0, frames one request from `rbuf` and hands it to `dispatch`, and
a finished request re-frames whatever was pipelined from the same buffer, never
with another read. An `Expect: 100-continue` is answered by staging the interim line
in `out` and sending it like any reply; it finishes nothing, because no request is
framed yet. A send that fails closes the client, and `try_frame` stops there: a body
that would stream is never dispatched for a client already gone. Reading continues while a request is in flight, so a client may pipeline
behind it, up to `kMaxClientBuffered` (no legal request is larger); past that its
reads are paused through `set_read` (counted in `client_read_pauses`) until the
requests drain the buffer. On io_uring a pause cancels the multishot recv, whose
`-ECANCELED` completion re-arms it if the client was resumed meanwhile; bytes the
kernel had already placed still arrive, bounded by the shared buffer ring. A client
that disconnects with a request, reply or stream still owed counts as an error on
either backend (`client_gone`). `gateway/tests/core_test.cpp` drives the shared methods with
a `FakeIo`, so a send that does not complete happens exactly when a test says so.

## 4. The epoll backend (`gateway/src/io/epoll_io.cpp`)

`EpollIo` owns the epoll instance, its loop and its syscalls, and nothing else.
Level-triggered, with optimistic writes.

```
  epoll_wait
      |
      +-- listener readable -------> on_accept()             accept4() in a loop
      +-- client fd readable ------> on_client_readable()    read() -> on_client_bytes()
      +-- client fd writable ------> on_client_writable()    respond(), or stream_flush()
      +-- upstream fd readable ----> on_upstream_readable()  read() -> on_upstream_bytes()
      +-- upstream fd writable ----> on_upstream_writable()  connect completed, or drain out
      |
      v
  sweep_idle()          reap idle pooled upstreams, every timeout
      |
      v
  free everything on _doomed        <- see section 7
```

**Optimistic write.** A reply is written at once and `EPOLLOUT` is armed only on
`EAGAIN`, so the common case costs one syscall; `send()` disarms it again whenever
everything went. A fresh upstream is registered once, `EPOLLIN | EPOLLOUT` together:
its connect completing is its first writable event.

**One writer of the interest mask.** `sync_interest()` computes the mask from
`read_paused` and `write_armed`, so arming a write cannot undo a pause.

**Back-pressure.** `set_read` drops `EPOLLIN`; `EPOLLHUP` and `EPOLLERR` still fire, so a
close is never missed. Reads are bounded per event (`kMaxReadPerEvent`, 1 MiB): a drain
to `EAGAIN` once staged 33 MB for a client that never read.

**A close behind the answer.** One drain can read a venue's last bytes and its EOF
together: a reply of a multiple of the 16 KiB read with the FIN in the same segment. The
bytes are framed before the close is handled, the connection marked never to pool, so a
complete answer is the client's; handled first, the close made it a 502 and a failure
reported against a venue that had answered. A stream frames what is left at its own EOF.
io_uring completes the data before the close, so it never had the question.

## 5. The io_uring backend (`gateway/src/io/uring_io.cpp`)

`UringIo` owns the ring, the provided-buffer ring and its loop. Completion-driven,
`SINGLE_ISSUER | DEFER_TASKRUN`, with multishot accept and recv into the shared buffers.

```
  submit SQEs                          reap CQEs
      |                                    |
      +-- IORING_OP_ACCEPT (multishot) ----+--> on_accept()
      +-- IORING_OP_RECV   (multishot) ----+--> on_recv()     -> on_client_bytes() / on_upstream_bytes()
      +-- IORING_OP_SEND               ----+--> on_send()     -> finish() / stream_flush()
      +-- IORING_OP_CONNECT            ----+--> on_connect()
      +-- IORING_OP_TIMEOUT            ----+--> periodic tick: sweep_idle()
      +-- IORING_OP_ASYNC_CANCEL       ----+--> (control, not counted)
```

4096 buffers of 4096 bytes. Multishot recv lands data in the ring instead of a
per-connection buffer, so the kernel picks the buffer and it is recycled once copied
out. If the setup fails, `Engine::run` runs `EpollIo` instead.

Three constraints shape everything else here:

1. **An SQE is immutable once submitted.** The kernel reads the buffer the SQE
   points at, on its own schedule. Anything that could reallocate that buffer
   while an operation is in flight is a use-after-free.
2. **Two concurrent SENDs on one fd would interleave**, so sends are serialised.
3. **Completions arrive after the code has moved on**, so an object can be
   logically dead while the kernel still holds a reference to it.

A full submission queue is flushed once and retried. If it is still full, a send
or recv closes the pair (`drop_sq_full`), a connect fails the acquire, and a lost
timer is re-armed on the next loop.

### The pin: one owner

`send_inflight()` means exactly "an SQE referencing this connection's wire buffer
is outstanding": the `OutBuf` the socket reads (`tls_out` on TLS, else `out`) is
pinned.

| | |
|---|---|
| **pinned** | `UringIo::submit_send()` only, the sole place a send SQE is submitted |
| **unpinned** | `UringIo::on_send()` on completion, and `on_cqe()` for a doomed connection's |
| **read** | by anyone deciding whether to submit now or let the completion do it |

Callers must never pin. Two of them used to set the old flag, under different
rules, and the stream flush setting it before calling into the TLS flush, whose
first line refuses to run when it is already set, meant the first SSE flush on a
TLS connection did nothing and the stream hung forever, on io_uring only.

New stream output goes to `stage()`, the back half while a send reads the front,
and goes out with the next send.

### 5b. `inflight` and `recv_armed`

**`int inflight`**: submitted-but-uncompleted SQEs referencing this connection.
One protocol with `doomed` (section 7): doomed says we want the object gone,
inflight says whether the kernel agrees yet.

| | where |
|---|---|
| incremented | `arm_recv`, `submit_send` and `connect`, each only once its SQE is filled, so an early return cannot strand the object |
| decremented | exactly one place, `on_cqe()`, and only when the completion is not armed: a multishot op carrying `F_MORE` is still outstanding |
| read | `maybe_free()`, the only thing allowed to conclude that freeing is safe |

**`recv_armed`**: a multishot recv is live. A pause (`set_read(c, false)`) cancels it
by its `user_data`; its last completion, `-ECANCELED`, re-arms it if reads resumed
meanwhile, and a resume with no recv live arms one at once.

## 6. Streaming (SSE)

Entered when the provider answers `200` with `content-type: text/event-stream`.
The gateway then pumps instead of buffering a whole body: decode chunked,
translate Anthropic events to OpenAI chunks, write to the client.

```
  provider ---chunked SSE---> u->rbuf
                                |
                                v
                        stream_step()            shared by both backends
                       decode chunk -> translate
                                |
                     -> c->out.stage() -> stream_flush() -> IoOps::send
             epoll:  write now; Queued pauses the upstream
             uring:  SEND (pins the front); past kStreamPauseAbove owed, pause;
                     past kStreamBufCap the stream is dropped
                                |
                                v
                          client sees chunks
```

Three flags carry the state, and their meanings are precise:

- **`streaming`** latches in `begin_stream` and is cleared only when a stream
  finalizes on a connection that is kept; the next request's `begin()` resets the
  rest.
- **`stream_ended`** means the stream will produce no further event; a clean end's
  note and terminating chunk follow from `stream_terminate` (6d). It becomes true
  two ways: cleanly, when the translator emitted its terminal `[DONE]`; or by
  truncation via `stream_truncate()`, which emits **no** `[DONE]` on purpose so a
  client sees a cut-off stream instead of a fabricated clean finish. So
  `stream_ended && close_after_resp` is the truncated case and
  `stream_ended && !close_after_resp` the clean one.
- **`read_paused`** is written only by a backend's `set_read`: a stream's upstream,
  and a client past the pipelining cap, on either backend.

`stream_ended` also gates `finalize_stream`. Without it the gateway resumes
reading from an upstream that will never speak again, and the client socket is
held until the idle sweep.


### 6b. `stream_ended`, and the difference a client can see

It becomes true two ways, and only one of them is a finished answer:

| | what happened | `close_after_resp` |
|---|---|---|
| **clean** | `stream_step()` saw the end of the body and the SSE translator emitted its terminal `[DONE]`; `stream_terminate` stages the terminating chunk (6d) | false |
| **truncated** | the stream is being aborted, so no `[DONE]` is emitted, deliberately: a client must see a cut-off stream instead of a fabricated clean finish. Always via `stream_truncate()` | true |

So `stream_ended && close_after_resp` is the truncated case. That pairing is the
difference between a client believing it received the whole answer and knowing
it did not, so read it, never infer it.

`stream_truncate()` makes the three mutations together (`stream_ended`,
`close_after_resp`, `++errors`) because five call sites once wrote them by hand,
which is how a sixth ends up setting two of the three. The `stream_ended` latch is
load-bearing: `stream_flush` finalizes only when it is set, and without it
it resumes reading from an upstream that will never speak again, holding the client
until the idle sweep. `CorruptStreamFromAProviderThatHoldsTheConnectionStillClosesTheClient`
is the test that fails without it. The `TRUNCATED` warning is logged there too, in
the one shared place: the abort paths bypass the finalize, and a truncated stream is
the one outcome a client cannot tell from a clean finish, so if it is not recorded
on our side nobody can answer "did my stream complete?" afterwards.

### 6c. Back-pressure under a slow client

Both backends pause a stream's upstream for a slow client and resume it once the
client has drained what the stream owes it (`owed()`: plaintext, and ciphertext on
TLS). They differ in when, and `IoCaps` says which, read in `stream_pump` and
`stream_flush`:

| | pauses | ceiling |
|---|---|---|
| **epoll** | when a write is left short (`stream_pause_above` 0): the socket is full | none; a read is budgeted per event (`kMaxReadPerEvent`) |
| **io_uring** | when more than `kStreamPauseAbove` (1 MiB) is owed: every send is queued while it is in flight, so queued alone means nothing | `kStreamBufCap` (8 MiB): bytes the kernel placed before the cancel still land, and past the cap the stream is dropped |

An io_uring pause cancels the upstream's multishot recv; a resume re-arms it, or the
cancel's completion does if the resume came first. A paused upstream is not polled,
so a venue that resets meanwhile is noticed on resume. Before 0.87.0 io_uring did not
pause streams: the cap was its only bound, and a slow client's stream past 8 MiB was
dropped. The 32 MiB flood to a TLS client that never reads (`GatewayTls.
ClientThatNeverReadsCannotGrowUsWithoutBound`) peaks at 55 MB of process RSS paused,
69 MB when the cap dropped it; epoll's is 53 MB.

Either way the idle deadline (`--upstream-timeout`) measures `ts_progress`, which both
legs advance: a response byte read, a request byte sent (`note_sent`, never a part
merely staged), a byte the client took. A paused upstream reads nothing while its
client drains, and a finished stream reads nothing while its client catches up;
neither is idle. When neither leg moves and bytes are still owed to the client, the
sweep logs `client stalled` and counts `client_idle_timeouts`, not
`upstream_timeouts`. A body still arriving has its own rule (12).

`read_paused` is written only by a backend's `set_read`, which both guard on the
current value. A stream pauses its upstream only where `stream_backpressure` holds,
and so does the release to the pool, which resumes reads first (8).

### 6d. A sink's note at the clean end

`RequestSink::reply_note` lets a sink tell the client something only the finished record
knows, such as what the request cost. At a stream's clean end the gateway builds the
record as it stands (status 200, `streamed`), asks the sink, and writes a non-empty
answer as one SSE comment line, `: <note>` and a blank line, which a parser following the
SSE specification ignores. It goes after the last event (the venue's own `[DONE]` or
`message_stop` on a byte-forward, the translator's `[DONE]` on a translated stream), in a
chunk of its own, before the terminating chunk; on a close-delimited client stream it is
the last bytes before the close. `on_request` comes later, at the finalize, as before.

One path writes it for both stream kinds and both backends. `stream_step` sets
`stream_ended` and no longer stages the terminator. `stream_flush`, which every stream
passes on either backend, calls `stream_terminate` once (`stream_terminated`) when the
end is clean (`stream_ended` without `close_after_resp`) and a client will read the bytes
(no `shadow`). `stream_terminate` stages the note through `sink_note`, then the
terminator, and `sink_note` builds its record with `record_of`, as `sink_emit` does.

The call sits after the hedge block in `stream_flush` because that is where a leg learns
whether it won (13). A leg whose stream ended with no token while another was still live
lost, and never reaches it. A winner reaches it on the client, after `adopt_leg` swapped
its state in, so the record is the winner's, `hedge_won` set, whether its end came in the
read that made it win or later.

Nothing else gets a note: a truncated, corrupt or failed stream (6b), a whole reply, a
gateway with no sink, an empty answer. A note is dropped whole, never cut or cleaned, with
a WARN naming the reason and the length and never the text, when it is over
`kReplyNoteMax` (1 KiB) or holds a CR, LF or NUL, which would end the comment line and
let the rest parse as an event. It is dropped too when a byte-forwarded venue's bytes did
not end in a blank line, in any of the line endings the SSE specification allows (LF, CRLF,
CR, mixed; `stream_tail`, the last four bytes it sent): the note would join
the venue's last line, or its blank line would dispatch an event a parser would otherwise
have dropped. A close-delimited venue's EOF is always a clean end (6b), so this is the
venue that died mid-event. Translated output always ends on a whole event.

Without a sink the note costs one predicted branch per stream and the tail two stores per
byte-forwarded read; with a sink, one record and one virtual call per stream. The cap
check in `stream_pump` runs before the note and the terminator are staged, so those last
bytes, at most a KiB, are not counted against io_uring's 8 MiB cap.
`ProxyReplyNote.*` in `gateway/tests/gateway_test.cpp` covers both backends.

## 7. Connection lifetime, and why the two backends free differently

This is where a mistake is a use-after-free in a process holding customer
credentials, so the rules are worth stating exactly.

A close is two halves. `Engine::forget()` is the bookkeeping, the same on both
backends: unpair, deregister, drop from the pool, mark `doomed`, move to `_doomed`.
The backend's `close()` does the rest. **Both backends defer the free**, for
different reasons:

| | why deferred | freed when |
|---|---|---|
| **epoll** | one event can close a pair, and a later event in the same batch would dereference a freed pointer | end of the current event batch, unconditionally |
| **io_uring** | a submitted SQE references the object, and the kernel does not care that we decided to close | `inflight` reaches 0, possibly several loop iterations later |

```
  EpollIo::close()                       UringIo::close()
        Engine::forget()                       Engine::forget()
        ::close(fd)                            shutdown + IORING_OP_ASYNC_CANCEL
              |                                      |
              v                                      v
   end of batch: delete every _doomed      on_cqe: --inflight
                                                     |
                                                     v
                                          maybe_free(): free iff
                                          doomed && inflight == 0
```

Every connection is in exactly one `Registry` (`gateway/src/core/registry.hpp`):
`_clients`, `_upconns` (live upstreams, pooled ones too) or `_doomed`. Each member
holds its own index, so a close moves it in O(1), and the destructor and the
io_uring drain walk the registries instead of following `up` to find upstreams.
`Engine::free_conn` deletes a doomed one as its own type, keeping an upstream's
buffers warm for the next.

**The unconditional frees are safe for two different structural reasons.** epoll
never increments `inflight`, so it is always 0. `UringIo::run()` ends with a drain
that reaps until no SQE is outstanding, and its count moves in lockstep with every
connection's. A runtime check at those sites was added and removed: unreachable,
untestable, and therefore protection in appearance only.

## 8. Connection pooling

Upstream connections are keep-alive and reused. A streaming request would
otherwise pay a fresh connect every time, which measured as the dominant term in
time to first token.

```
  acquire_upstream
        |
        +-- UpstreamPool::acquire(venue): the newest, from_pool = true, ++upstream_reused
        |
        +-- else: new socket, connect (async on both backends; IoOps::connect)
        |         no address yet: refused, the name asked for off the loop
        |         an error known at once: this host's (503), or the venue's (next address)

  ...request completes...

  release_upstream -> UpstreamPool::release, the one reusability check;
  any refusal closes the connection:
        |
        +-- closed already, or not keep-alive (response, or a byte-forward's client)
        +-- no message boundary: close-delimited, or a stream cut before its end
        +-- the venue sent EOF (peer_eof)
        +-- bytes past the response in rbuf: a second message nobody asked for
        +-- request not fully on the wire: ++upstream_unsent  <- see below
        +-- kMaxIdleUpstreams (8192) connections pooled across all venues
        |
        +-- otherwise: SCRUB (section 9), stamp ts_pooled, link as the venue's newest

  sweep_idle(): reap what idled past --pool-idle (30 s), oldest first
```

The pool is one intrusive list per venue: `acquire` takes the newest, removal on
close is O(1), and the reaper visits only expired connections, which sit at the
oldest end of each list. `up` and `client` are written only by `pair()` and `unpair()`,
and every close unpairs, so neither leg keeps a pointer to a freed connection.

**Why the 30 s reap.** Providers drop idle keep-alives on their own schedule, and
discovering a corpse costs a request its retry. Reaping first is cheaper than
finding out the hard way.

**A response can beat our own request out the door, and such a connection must
not be pooled.** The upstream's recv is armed before the request is sent, so a
provider that answers early, a 413 or 401 on the headers of a large body, hands
us a complete response while the request is still half-written. Pooling that
connection scrubs and later overwrites a buffer the transport has not finished
with, and leaves a truncated request on the wire for the next client to inherit.
`upstream_request_sent()` is the predicate, checked in `UpstreamPool::release`
before the pool takes it, and the connection is closed instead. It fails closed
because the provider's view of that connection is not something we can
reconstruct. Reproduced on both backends by `ProxyEarlyResponse`, where the next
client's request was silently dropped, and mutation-guarded on each backend
separately.

**Stale-connection retry.** A pooled connection that fails before any response
byte arrives is retry-eligible (`from_pool && !retried`): the request is resent
on a fresh connection, and the client never sees the blip. The write path keeps
`out`'s front for exactly this reason, so the request survives to be resent (2b);
the gate is the phase (3c), so a request whose response has begun is never resent.

**Addresses and the resolver thread.** A venue's address is `Upstream::ip`, read by
`new_upstream` at every fresh connect, never cached. A failed connect moves it to the next
of `ips`, and a venue with a `host` asks for the name again (`request_reresolve`). The
lookup runs on a thread that shares only `Reresolver` with the engine, by `shared_ptr`:
the name travels with the job and the answer comes back in `done`, whose capacity is one
entry per venue, so a push never allocates; `apply_reresolved` installs answers from the
sweep, on the loop. Per venue, one job in flight (`_rr_inflight`), and none before
`_rr_next_ns`: a second after an answer, or a backoff that doubles with each failed lookup
up to `kReresolveMaxNs`. A venue given by name with no address yet is asked for by `run()`
and by every sweep the backoff allows, and refused meanwhile (`upstream_unresolved`). A
lookup that fails, or a job that cannot be queued, backs off the same way and logs once per
outage; one that finds the addresses already held is DEBUG. The engine's destructor joins
an idle resolver and detaches a busy one, saying so, whose lookup finishes on the shared
state: shutdown never takes the resolver's timeouts.


### 8b. Sizing `kMaxIdleUpstreams`, and why it is 8192

The pool was unbounded until streaming reuse landed. A streaming gateway pools
roughly one upstream per concurrent stream, so without a bound it is an fd leak
in slow motion: 4k streams means 4k idle descriptors pinned indefinitely.

**Size it generously.** The cap must exceed the number of upstreams in flight at
peak, or it stops being a bound and becomes a reuse killer: once the pool is
full, every release closes its connection and the next request must reconnect.

A first cut of 256 did exactly that, and cost the non-streaming path **2.4x its
throughput**:

| pool cap | RPS at a 90k target |
|---|---|
| 256 | 32,210 |
| 8192 | 77,282 |
| no pool at all | 78,445 |

The gateway was opening more upstream connections than it served requests.
git-bisected. **Do not lower this without re-running `./bench/saturate.sh` with
`BACKENDS=4`.** The real reclaim mechanism is the idle timeout, not the cap; the
cap only has to stop pathological growth, so err high.

## 9. Scrub and clear: what a pooled connection must not carry

A pooled connection idles for up to 30 seconds and is then handed to **whichever
client asks next**. Anything left in its buffers becomes the next customer's
problem, so release does two different things for two different reasons:

```
  UpstreamPool::release
        |
        +-- u->rbuf non-empty      RESIDUE. Bytes past the framed response would
        |   refuses                otherwise be read as the head of the NEXT
        |                          client's response: a cross-client desync
        |
        +-- u->framer.reset()      the cached head and chunked-decode state
        |
        +-- u->out.scrub()         CREDENTIAL. out held the rebuilt request
        |                          including the customer's API key
        |
        +-- msg = {}, tls_out.clear()
```

**`clear()` and `secure_clear()` are not the same operation.** `std::string::clear`
sets the size to zero and leaves the bytes in the allocation; the credential would
still be in memory, readable from a core dump or a later reallocation.
`secure_clear` overwrites the bytes through a `volatile` function pointer the
compiler cannot elide. A plain `memset` there emitted **zero instructions** at
`-O2` under dead-store elimination, which is why the barrier exists.

**The TLS `Session` is kept across pool cycles.** Only the per-request ciphertext
in `tls_out` is dropped, so a pooled reuse pays no second handshake.

Both of these are mutation-verified: deleting either one fails a test.

### 9b. How `secure_clear` erases (`net/include/net/secure.hpp`)

There is no standard way to do it: `std::secure_clear` (P1315) was proposed and never
adopted. So the platform primitive is chosen at CMake configure time by feature
detection, never by guessing from `#ifdef __linux__`:

| Primitive | Where | Detected as |
|---|---|---|
| `explicit_bzero` | glibc >= 2.25, the BSDs | `LLMBRIDGE_HAVE_EXPLICIT_BZERO` |
| `memset_s` | C11 Annex K, MSVC | `LLMBRIDGE_HAVE_MEMSET_S` |
| `SecureZeroMemory` | Windows | `_WIN32` |
| `volatile` memset | everywhere else | fallback |

The fallback is always correct, because `volatile` semantics are mandated by the
standard, but it comes last because it works for an indirect reason and the named
functions do not. To see the dead-store problem for yourself:
`g++ -O2 -S ... | awk '/secure_clear/,/ret/' | grep call`.

**Scope.** It is not a hot-path function and need not be: it runs once per request
when a pooled upstream is released (2.4 ns for a ~96 B buffer, about 0.02% of one
core at 84k RPS). Do not sprinkle it over transient buffers overwritten microseconds
later; that buys nothing and costs the hot path.

## 10. TLS on both legs

Two independent TLS sessions in opposite roles, never one pipe. The gateway
terminates the client's session and originates its own to the provider, which is
what lets it translate dialects and swap credentials in the middle.

```
  client ==TLS==> [ gateway ] ==TLS==> provider
           we are the SERVER      we are the CLIENT
           (--listen-tls)         (--upstream https://...)
```

OpenSSL is driven through **memory BIOs**, never `SSL_set_fd`, because on
io_uring the loop already owns the bytes: multishot recv has read them before
OpenSSL is involved. So the `Session` is a pure byte transform:

```
  socket --recv--> feed_ciphertext() -> [rbio] -> SSL -> read_plaintext() --> rbuf
  socket <-send--  pull_ciphertext() <- [wbio] <- SSL <- write_plaintext() <-- out
```

The same four calls serve both directions and both backends. Only the handshake
role differs (`SSL_set_accept_state` against `SSL_set_connect_state`) and what is
verified: the client leg presents a certificate and verifies nobody, the upstream
leg verifies the provider's chain and its hostname, with no way to disable it.


### 10b. Two offsets, two questions

For a TLS connection, `out` (plaintext) is not what gets written; the plaintext
stays intact there so a stale pooled connection can be retried.

| field | answers |
|---|---|
| `out` sent | plaintext handed to the transport. For TLS that means fed into the Session, which has not necessarily left the machine |
| `tls_out` sent | ciphertext that actually reached the socket |

So `out.idle()` means "fully encrypted", never "fully sent". Ask
`tls_out_flushed()` for the second question.

A transport-agnostic wrapper over the two was tried and reverted: every caller
already sits inside a TLS-only branch, so it resolved to `tls_out_flushed()` at
every site and read as a safety net while changing nothing.

### 10c. Why the TLS helpers take a `ConnBase`

They are no-op-safe building blocks shared by both backends, and since inbound TLS
landed they run on client connections too. What differs by direction lives in the two
`tls_feed` overloads, chosen by type: a client's failed handshake is counted as scanner
noise, and an upstream's completed one stamps t2 and pushes the request that waited
for it.

### 10d. Two TLS checks that look removable

`tls_invariant_ok()` refuses a write on a connection the configuration says must be
encrypted but that has no Session: on the client leg those bytes would answer a peer
that dialled TLS, on the upstream leg the next thing sent is the provider credential.
It cannot fire today, and a mutation sweep confirms no test can tell it from `return
true`. It stays because what makes it unreachable is six call sites that each close
the connection when `tls_attach_*` fails, an invariant re-established by hand, two of
them added the week inbound TLS landed. A guard whose precondition is one structural
fact is noise and gets deleted; the test is how many places must stay right for it to
stay unreachable.

`tls_feed()` logs `Session::last_error()` on a failed handshake. Before it did, wrong
CA, wrong hostname and a protocol mismatch all looked like a closed socket. A failure
on the client leg during the handshake is almost always an internet scanner speaking
junk at the listener, so it is logged at DEBUG, where an info-floor build stays quiet,
and counted in `client_tls_handshake_failures`: the line is noise, the rate is a
diagnostic, and a customer who cannot handshake shows up as a step in it. An upstream
handshake failure or a mid-session failure on either leg stays WARN.

## 11. Where each rule is enforced

Documentation drifts; these do not.

| rule | enforced by |
|---|---|
| `core/` includes no backend header (`<sys/epoll.h>`, `<linux/io_uring.h>`, `net/uring.hpp`, `io/*`) | `scripts/check_conventions.py`, CI |
| `core/` makes no socket call (`::read`, `::write`, `::send`, `::accept`, `::close`, `epoll_ctl`) | same |
| `io/epoll*` never mentions io_uring, and `io/uring*` never mentions epoll | same |
| each of those rules still catches a planted violation | same, before it checks the tree |
| one backend calls the other | the compiler: they are separate `final` classes |
| namespaces mirror directories | `scripts/check_conventions.py` |
| LATENCY.md's stamp table names real functions that assign the stamp | same |
| constants are `kPascalCase`, types `PascalCase`, `ALL_CAPS` is macros only | same |
| the shared lifecycle behaves under a send that does not complete | `gateway/tests/core_test.cpp`, with a `FakeIo` |

A green suite proves nothing it
checks regressed; it does not prove it checks anything in particular. The
mutation harness breaks each invariant on purpose and reports whether any test
notices. It has found guards that were decoration, and test gaps in code that
looked covered.

## 12. Request bodies forwarded as they arrive

A request is framed when its last byte is in `c->rbuf`, so without this section a
10 MB upload reaches the venue only after the client finished sending it: one full
upload time added to the request. `gateway/src/core/body.cpp` sends such a body on
as it arrives instead, when nothing about it needs the whole body:

- the head is complete, the body is at least 16 KiB and not yet all here, and it is
  not content-encoded;
- no policy is installed (a policy reads the body before it routes), or one that
  decides on the head (12c);
- the plan is a byte-forward: a translation reads the whole body;
- `--stream-body on`, the default, and for an OpenAI client `--stream-openai-body on`
  too. Off, a body is held to its last byte exactly as before;
- the worker streams fewer than `--stream-body-max` bodies already (below).

`try_frame` opens the request at the head (`open_request`: t0, the sequence number,
the sink's capture) and dispatches it with the body so far. `dispatch` writes the
head and that prefix to the upstream; `body_sent` counts what went. Each later read
calls `pump_body`, which appends the new bytes to the upstream's `out` and sends.
The body stays in `rbuf` (`body_in_rbuf`) until the first response byte, so a stale
pooled connection's retry rebuilds the head and resends every byte sent so far.

**Checked before it goes.** An OpenAI client's JSON body (`Content-Type` JSON) is
fed to `provider::BodyScan` before any byte is forwarded: RFC 8259 syntax, nesting,
and the top-level and `stream_options` keys that a held body would have been
refused for (repeated, escaped, too many). `safe()` holds back a key whose closing
quote has not arrived, so a repeated `"model"` never leaves, even when it is split
across reads or sits in the last one. On a refusal the gateway stops sending, sets
`SO_LINGER` 0 on the upstream and closes it: the venue sees a reset and a body
short of its `Content-Length`, which it never runs, and the connection is never
pooled. The client gets the 400 a held body would have got. Other clients' bodies
are not scanned; their translators would refuse a bad one, and here none runs. The
exceptions are a body a model substitution (12d) or service_tier, speed or
inference_geo (12e) is spliced into, whose JSON is scanned whatever the client's dialect.

**A response before the last byte.** A venue may answer early (an error, usually).
`end_body_stream` stops forwarding, marks the upstream not poolable, drops the copy
in `rbuf` and counts the rest of the body as `discard`: those bytes are read and
thrown away as they arrive, so the client connection stays in step for its next
request. A reply that closes the connection waits for that drain
(`close_on_drain`). It takes a response byte: a TLS venue's session tickets arrive
after its handshake as records with no plaintext, and the body goes on past them.

**Stamps.** t0 is the head's framing and t3 the first send, the head and the body
there at dispatch, in the kernel. `mark_first_send` records where it ends in the
upstream's `sent_bytes`; on TLS the Session encrypts it alone first, at dispatch or,
on a cold connection, once the handshake is done, so parts staged meanwhile come
after it. `note_sent`, on every send that moves bytes, stamps t3 when the mark is
passed, so parts queued behind a backed-up first send do not move it. Each part
pumped after t3 adds its scan, copy and send to `body_work_ns`, which request-path
counts; one pumped before is inside connect or upwrite already. The read that brings
the last byte stamps `ts_body_end`, where `client_upload_ns` ends, and the last send
completion `ts_body_sent`, where the provider's wait begins. So the connection's
first-byte stamp runs on past the head: `pump_body` restarts it for the next request
at the last byte, or `drop_discard` once a cut-short body's rest has gone.
LATENCY.md 2b.

### 12b. What a slow upload may hold

A held body costs one descriptor while it arrives. A streamed one also holds a
connection to the venue, a TLS handshake when cold, and a second descriptor until its
last byte, and before 0.94.0 nothing bounded how long: every part refreshed the idle
clock and there was no upload deadline, so a client sending a byte a minute held all
three with no bandwidth at all. (A held first request is cut by the 30 s setup
deadline; a streamed one is past it at its head, where `dispatch` sets `ever_framed`.)
Three bounds and one rule close that.

**How many at once.** `body_streams` admits a body only while the worker streams fewer
than `--stream-body-max` (256 by default; 0 for no cap). Past it the body is held to its
last byte exactly as before 0.90.0, counted in `streamed_body_capped`, and bounded as a
held body is: on a connection's first request it must arrive within the 30 s setup
deadline. So while the slots are full, an upload slower than that on a fresh connection
is closed with no reply, where streamed it would have been served (SECURITY.md).
`_bodies_streaming` counts a slot from the head to whatever ends the body, through one
function, `stop_body`: its last byte, a response byte, a refusal, an error reply, a
timeout, or the client leaving (`forget`).

**How slowly.** A streamed body gets `--body-grace` (10 s), and one second more for
every `--body-min-rate` bytes (1024) it has delivered; `--body-timeout`, off by default,
caps the whole upload from its head (`body_deadline`). The rate is earned, not
sampled: a client that sent 1 MB quickly has 17 minutes of credit for the rest, while
one sending a byte now and then is cut once its grace runs out. A pause spends that
credit only up to `--upstream-timeout` (120 s): a body that sends nothing for longer is
a client stall (below) whatever it earned, as it was the venue's 504 before 0.94.0.
1 KiB a second is 8 kbit/s, below a 2G data link, so the default cuts no client whose
link is merely slow, however large its body; a total limit cuts every such client once
the body is large enough, which is why it is off. A slowloris now pays for its time in
bandwidth, and holds at most `--stream-body-max` venue connections per worker doing it.

The deadline is checked in two places. `sweep_client` cuts a body that sends nothing,
within one tick of it; `pump_body` checks before a part goes, against the deadline the
bytes before it earned, so bytes that arrive late never reach the venue, the body's
last included. A body that misses it gets `body_timeout`: `streamed_body_timeouts`, a
`TIMEOUT streamed body too slow` line with the bytes in and the body's length, and a
408 whose text is fixed (`kSlowBodyMessage`). `error_respond` resets the venue, and the
reply closes the client connection once it is out, without waiting for the rest. The
rest of a body cut short by an early response, read only to be dropped
(`drop_discard`), stays on the same deadline: past it the connection closes, where a
client trickling it held the connection to `--client-idle`.

**Who stopped.** When neither leg moves for `--upstream-timeout`, a body still
arriving is attributed by what the venue has. All of what came, with nothing staged,
nothing in flight and nothing the kernel holds unacknowledged (`net::unacked_bytes`,
`SIOCOUTQ`): the client stopped sending, so it is a client stall, a 408 and
`client_idle_timeouts`. Bytes the venue has not taken: it stopped reading, so it is the
venue's, a 504 and `upstream_timeouts`. Only a send that moves bytes is the venue's
progress; before 0.94.0 staging a part was, so a venue that stopped reading timed out
only once the client finished.

**Never resent as whole.** A stalled body is not failed over and not retried. The stall
path replies itself, never through `fail_request`, so not even a policy that decides on
the head (12c) is asked to fail it over. The stale-connection retry runs only on a venue
failure inside a second of taking a pooled connection, and resends the head and the
bytes sent so far under the declared `Content-Length`, the rest following as it
arrives: a venue that loses the client again holds a body short of its length.

**A reset, every time.** A venue whose body will not be finished is reset, whichever
path ends it: a refusal, a deadline, a stall, an error reply, the client leaving
(`cut_body`, `reset_venue`). SO_LINGER 0 makes the close an RST, and the connection is
never pooled. On io_uring a close shuts the socket down first, so its operations
complete; a reset shuts only the read side (`reset_on_close`), so the venue gets the
RST with no FIN ahead of it.

### 12c. A policy that decides on the head

A policy routes on the head, and with `wants_prefix_hash()` on the body's leading 4 KiB:
nothing past them chooses a venue or a key. One that returns true from
`decides_on_head()`, read once at construction, is asked on the head of a body that
qualifies under 12, as long as one venue in the table byte-forwards this client's dialect
(`any_venue_streams`) and the worker streams fewer than `--stream-body-max` bodies. Every
other request is decided whole, as before, and so is every request of a policy without the
opt-in: `body_streamable` refuses those first, and they take the path they always did.

**The decision.** With the hash wanted, `try_frame` waits for `decide_want`: the head and
the first min(body, 4 KiB) bytes. It opens the request there, t0, and calls `decide()` with
`body_whole` false: the head, the Content-Length, a top-level `model` that has closed in the
bytes so far (`model_of` on a prefix gives nothing or the whole body's own model, never a
cut name: `ModelOf.EveryPrefixYieldsNothingOrTheWholeBodysModel`), `stream` and
`include_usage` false, and the prefix hash a held body would carry. Then one of three:

- **Refused.** The reply goes at once, while the client is still uploading, and no venue is
  contacted. It says `Connection: keep-alive`: the rest of the body is read and dropped
  (`discard`, on the body's deadline), and the connection carries the next request.
- **Allowed as is.** No `model` override, no `hold`, and the venue it names byte-forwards:
  the body streams there as in 12, in a `--stream-body-max` slot. A model substitution
  (`substitute_model`, 12d) is spliced in as the body streams, and so are `service_tier`,
  `speed` and `inference_geo` into a JSON body (12e).
- **Allowed otherwise.** A `model` override, `hold`, a venue that translates, or a member
  override on a body not sent as JSON needs the whole body. It is **parked** (`body_parked`): kept in rbuf with nothing sent, holding no venue
  and no slot, an OpenAI client's JSON still checked by BodyScan as it arrives, and the
  upload deadline of 12b still running, in `pump_body` and the sweep alike. At its last
  byte it is sent as a held body.

**The last byte waits for the confirm.** No byte that lets a venue run the request leaves
before `confirm()`: while `confirm_pending`, `dispatch` and `pump_body` stage at most
`body_len - 1` bytes, and the read that brings the last byte stages nothing until the
confirm has answered. At that read `end_upload` takes the whole body's facts exactly as a
held body's (`capture_model`, whose key check refuses a repeated, escaped or excess
top-level key there, the venue reset) and calls `confirm(whole, early)`, `early` being the
decision in force (`decision_in_effect`: the venue serving now, after any failover, and
never -1). Its answer:

- **Keep**: the same venue, model, service tier and key; the tag may change, and is kept.
  The rest of the body goes, or a parked body is sent.
- **Refuse**: as a BodyScan refusal. `cut_body` resets a venue holding part of the body, so
  it holds one short of its Content-Length and never runs it, and the client gets the
  policy's status and message, checked as decide's are.
- **Redo**: any other allowed decision. The venue is reset, the decision applied by
  `apply_decision`, the one decide uses, and the whole body sent as a held body under it.

So a venue may see a prompt the policy then refuses or sends elsewhere, but never a
request it can run: the cost of a change of mind is a partial upload, dropped.

**Failover and retry.** A body decided on its head stays in rbuf (`body_in_rbuf`) until the
first response byte, so a failover resends it from there, never from `saved`, and a request
pipelined behind it keeps its place. While it is still arriving, a failover to a venue that
takes it as it is streams it there from its first byte; with a `model` override, a member
override the head decision did not set (12e), or to a venue that translates, it parks. Once whole it goes as a held body. A stale connection's retry
rebuilds a body that went in parts (`body_parts`) from rbuf, with the key the policy named
(`named_key`), and resends one that went whole from its staged copy.

**Stamps.** A streamed body is timed as in 12: t0 is its head, the decision falls between
t0 and t1, and the confirm is part of `body_work_ns`. A parked body, and a redone one, is
timed as a held body: `dispatch_whole` moves t0 to its last byte and zeroes
`body_work_ns`, so the upload stays the client's and nothing done for an abandoned attempt
is counted (LATENCY.md 2b).

**Counted.** `policy_head_decisions`; `streamed_body_parked`, failover parks included;
`streamed_body_redone`, redos of a body a venue held part of; and
`streamed_body_confirm_refusals`, which `policy_denied` counts too. A sink record carries
`decided_on_head` and `confirm_outcome`; with `body_streamed`, which a redo leaves set, a
refusal or a redo of a body a venue had part of is a wasted partial upload.

### 12d. A model substitution spliced into the stream

The OpenAI and Anthropic SDKs serialize `model` after `messages`, so on a head decision
(12c) the model of nearly every request is still to come. A policy that routes a sentinel
model (kottos' `"auto"`) to a real one would have to rewrite it, and an override parks the
body: no gain from deciding on the head. Or let it stream and redo the request at the
confirm: a wasted partial upload per request. `Decision::substitute_model` and
`substitute_for` avoid both. They are conditional: when the body's top-level `model` is
the plain JSON string `substitute_for`, it goes as `substitute_model`; any other model, or
none, goes as the client sent it. Generic, not kottos': the sentinel is the policy's.

**Where it applies.** On a body sent whole (decided whole, parked, redone, or failed over
once whole) the model is known, so `resolve_substitution` turns it into an override at
`dispatch`, the one rewrite a held body gets: `f.model`, the plain string `capture_model`
read, equal to the sentinel. On a body streamed on its head decision it is spliced into the
stream, by `ModelSplice` (request.hpp), so the body streams as one with no override does.

**The declared length.** The head goes before the model is seen, so its Content-Length is
computed by us, not taken from the client: the client's plus `grow()`, what the replacement
adds over the sentinel (0 for a shorter one). Both are held to `substitute_ok` (printable
ASCII, no `"` or `\`, at most 255 and 64 bytes), so the replacement goes between quotes
with no escaping and its size is its wire size; one that fails it is a 500, as an override
too long is, at the decision or, for a `Retry`, a failover given up.

**Finding the model.** Every streamed JSON body under a substitution is scanned, whatever
the client's dialect: `start_body` resets BodyScan with `track_model` (`subst_tracked`), an
OpenAI body under its usual rules, another dialect's with only `model` refused if repeated
and any escaped top-level key refused, so the model the gateway matched is the one the venue
reads. BodyScan holds the model's value back from `safe()` from its first byte until it
closes, then tells its span (`model_at`, `model_end`), so a forwarded range never splits
it. A body not sent as JSON is not scanned and goes as it is.

**What goes.** `ModelSplice::append` maps each range of the client's bytes to the venue's:

- the value is exactly `"` sentinel `"`, raw: it goes as `"` replacement `"`, then
  `shrink()` spaces when the replacement is shorter, whitespace after a value being JSON's;
- any other value (a real model, an escaped spelling of the sentinel, a non-string): it goes
  as sent, and `grow()` spaces follow the body's last byte;
- no top-level `model`: the same spaces after the last byte;
- a repeated top-level `model`: refused by BodyScan before it leaves, as in 12.

Every other byte passes through unchanged, and the venue gets exactly the declared length,
valid JSON when the client's was. The sentinel is matched raw: `"\u0061uto"` reads as
`auto` to a venue, but it is not the plain string `model_of` returns either, so the gateway
never substitutes a model it would not report as the sentinel; it goes as sent, and
`confirm()` sees no model and no substitution (`fuzz_body_stream` checks the match and
`model_of` agree). `body_sent` stays a count of the client's bytes; the last byte, and with
it any trailing spaces, still waits for the confirm.

**The confirm.** `confirm(whole, early)` sees the client's own model (`"auto"`) and
`whole.model_substituted`, true when the splice replaced it; `early` carries the
substitution in force. A keep is judged by the model the venue gets, not the fields that
name it: a confirm returning `early`, or naming the replacement as `Decision::model`, keeps
the request, with no redo. Any other answer redoes it as in 12c, the whole body rewritten
as a held one. For a body that streamed without the splice (not JSON) the venue got the
client's model, so keeping a substitution that applies to it is a redo.

**Failover and retry.** `Retry::substitute_model` and `substitute_for` are the new venue's,
as its `model` is: empty substitutes nothing. A failover while the body streams resends it
from its first byte under the retry's substitution, spliced again (`forward_parts`); one
whose head decision named none, so the model was not tracked from the first byte, parks.
Once whole, the retry's substitution is an override as above. A stale connection's retry
rebuilds the spliced head and bytes from rbuf with `forward_parts` too.

**Counted.** At a confirm that keeps a spliced body, `streamed_model_substituted` when the
model was the sentinel, `streamed_model_padded` when it was not and spaces followed the
body. A sink record's `model_substituted` says the venue got the replacement, spliced or
rewritten whole; `model` stays the client's, `served_model` the venue's.

### 12e. service_tier, speed and inference_geo spliced into the stream

`Decision::service_tier`, `speed` and `inference_geo` set top-level string members. A held
body gets them from `apply_overrides`: a member the client sent has its value replaced
where it stands, and absent ones go in together right after the root's `{`, as
`"service_tier":"flex","speed":"fast",` in that order, without the comma for an empty
object. On a head decision (12c) that sets any of them, with no `model` override and no
`hold`, a JSON body whose venue byte-forwards now streams with them spliced in
(`MemberSplice`, request.hpp) instead of parking; a model substitution (12d) rides in the
same stream. kottos sets all three on its head decisions, so its flex and standard mixes
stream.

**The insertion.** The members go right after the root's `{`, in the text the held rewrite
writes for absent keys, then one separator byte: `,` when the object's first member stays,
a space when the object is empty or its first member is one the splice blanks (below).
Whitespace before the `{` stays where it is. So the declared Content-Length is the client's
plus `MemberSplice::grow()`, the members and their one separator, whatever the client sent,
plus the substitution's growth (12d). Each value must pass `MemberSplice::plain()`, the held
rewrite's own rule (no control byte, DEL, quote or backslash), so it goes between quotes
unescaped and its size is its wire size. One that fails parks the body, as every override
did, and the held rewrite then refuses it with the same 400.

**The client's own member: the rule.** The policy's value wins and the venue never reads
two. The splice treats our members as the object's first member, so every member the
client sent has a separator before it: the one after our members, or the client's own
comma. A client member the splice replaces goes as spaces from that separator through its
value, whatever whitespace surrounds it:

- first: `{"service_tier":"flex" ` then spaces for `"service_tier":"auto"`, then `,"messages":...`;
- in the middle: `{"service_tier":"flex","messages":[...]` then spaces for
  `,"service_tier":"auto"`, then `,"model":...`;
- last: `...,"model":"gpt-4o"` then spaces for `,"service_tier":"auto"`, then `}`.

Every placement is fixed in the stream, so no client member ever costs a redo. A client
member equal to the policy's goes the same way. The venue gets the held rewrite's members
and values, in another order and with spaces where the client's member stood
(`fuzz_body_stream` holds the splice to exactly that); for a body that names none of them,
in a non-empty object, it gets the held rewrite byte for byte, and for an empty one that
plus a space.

**What the scan holds.** `start_body` resets BodyScan with `members`, the mask of members
set (`members_tracked`), whatever the client's dialect. BodyScan then refuses a root that is
not an object, a value that is not a string under a member it replaces (the held rewrite
refuses both: a 400 either way, the venue reset short of the body), and a repeat of one,
in another dialect's body too. It holds the bytes after the root's `{` until the first key
or the empty object's `}`, since the separator depends on them; each top-level member from
the separator before it until its key closes; and a member the mask names until its value
closes, then tells its span (`members()`: from the separator, the value's quote, its end).
So a forwarded range never splits a span, and `Splice::append` (request.hpp) maps each
range in one pass: the insertion when the range covers the byte after the `{`, spaces for
each member replaced, the model as in 12d.

**The confirm.** `confirm(whole, early)` gets `whole.service_tier_spliced`, `speed_spliced`
and `inference_geo_spliced`. A keep is judged on what the venue gets: on a spliced body,
the override, or with none, the client's own member, read off the scan's span. So a
confirm returning `early` keeps, and so does one naming no tier when the client's own
equals the tier spliced; one changing a value, or dropping it where the client sent
another, redoes the request as in 12c, the whole body rewritten as a held one.

**Failover and retry.** `Retry::service_tier`, `speed` and `inference_geo` are the new
venue's. While the body streams, a retry setting only members the head decision set too
(the scan has held the client's own back from the first byte) resends it from its first
byte with the retry's values spliced, under its own declared length; one setting a member
the head decision did not parks, as for `model`. A stale connection's retry rebuilds the
spliced head and bytes with `forward_parts`. A venue that translates still parks the body:
its translation is built whole. A body not sent as JSON is never scanned, so it parks.

**Counted.** At a confirm that keeps a spliced body, `streamed_override_spliced`; at one
that redoes it while a venue holds part of it, `streamed_override_redone`, which
`streamed_body_redone` counts too. A sink record's `service_tier_spliced`, `speed_spliced`
and `inference_geo_spliced` say what went spliced, and its `asked_tier` is the client's
own tier the splice blanked, as a held rewrite records the one it replaced.

**Not done.** The held rewrite parses with unique keys at every depth, so it refuses a body
that repeats a key inside a nested object; spliced, as substituted (12d), that body streams.

## 13. Hedged requests (`Decision::hedge`)

A decision may name up to `kMaxHedgeLegs` more venues. The request goes to all of them and to
the decision's own at once, each leg a request of its own: its own seq, venue, overrides,
key and record. The first leg with an answer serves the client; the rest are reset.

**A leg is a client connection with no socket.** `spawn_hedge` (`core/hedge.cpp`) makes a
`ClientConn` per leg with `shadow` set, leg 0 the decision's own. `new_leg` copies the
framed request into its `rbuf` and the client's `ReqState` into its own (the same arrival:
stamps, head facts, captured headers, model) with the leg's seq, venue, overrides and key,
copied from values `apply_hedge` already checked. Every leg is made and linked before any is
dispatched, so one failing at once is never taken for the last one standing. From there a
leg is an ordinary request: its venue's translation, its overrides, `named_key` for its own
key header and no other, the pool, the stale-connection retry, the sweep's timeouts,
`on_reply` and `on_failure`. The cost is one copy of the request per leg, besides the send
buffer every request is built into.

**The client waits.** The client sends no request of its own: its request leaves its `rbuf`
(a pipelined one behind it stays), its phase is `Dispatched` with no venue, and
`hedge_open` is set. The sweep skips it while it waits, since it has no venue to time out;
each leg keeps its own deadlines.

**The seam.** `Engine::_ops` is a `ShadowIo` (`io/shadow_io.cpp`), which hands every call
to the running backend except a shadow's: its send marks the staged bytes sent and returns
`Done`, its `set_read` does nothing, and its close is `close_shadow`. So no backend ever
sees a leg. A closed leg is unlinked from its client and freed at the next `sweep_idle`,
between events: a caller up the stack may still hold it, and no completion will ever arrive
to free it.

**Which leg wins.** A leg judges itself at the two points where a reply would go out, while
its client waits:

- `stream_flush`: a stream holds what it staged, its head included, and sends nothing. Its
  first token or thinking delta wins; a head alone never does, since a 200 head can precede
  an error event. A stream that ends without a token has lost, unless it is the last leg.
- `respond`: a whole reply wins if it is a 2xx. Any other status loses, unless it is the
  last leg, and then it is what the client gets.

A loser is recorded as it ends and closed. The winner is adopted (`adopt_leg`): its
`RequestCtx` is swapped with the client's (the connection keeps its own `discard` and
`close_on_drain`), its SSE translator moves mid-stream, its staged bytes are appended to
the client's `out`, and its venue connection is paired with the client. Then
`settle_hedge` cuts every leg still open, and the flush or reply continues on the client.
A stream's note (6d) is asked for there, after the adoption, so a leg that lost is never
asked and the client gets one note, built from the winner's record.

**The last leg gone with no answer.** A leg can also end with nothing to show the client:
aborted mid-stream, or closed by the sweep. When that leaves no live leg and the client is
still waiting, `close_shadow` answers it with the gateway's 502 and records nothing for
it: every leg left its own record.

**A cut.** `cut_leg` marks the leg cut, resets its venue (`reset_venue`: `SO_LINGER` 0,
never pooled), records it and closes it. A connection mid-request cannot be pooled, and a
reset is the only way HTTP/1.1 tells a venue to stop generating. The client closing while
it waits cuts every leg (`forget`).

**Records.** One per leg, each `hedged`: its own seq, `hedge_group` the client request's
seq, which no leg carries, and `hedge_leg` 0 for the decision's own. The first request a
process frames has seq 0, so `hedged` marks a hedge and never a group of 0; failover is
turned off by the same flag. `hedge_won` marks the leg the client got, success or the last
failure; `hedge_cut` a cut one, with status 0 and no token counts, since a cut stream never
reaches its usage. A leg that ends with no record, a stream cut short, is recorded by
`close_shadow`.

**Refused, a 500 with no venue contacted.** More than `kMaxHedgeLegs` legs, or a count with
no legs; a decision with no `credential_header`, since one client key would reach every
leg's venue; a substitution; a leg past the table (no leg falls back to venue 0); a leg
with no usable key header, an override longer than the gateway holds, or a beta that is not
a token. A leg to a SigV4 venue, which signs with a key pair and takes no named key, fails
alone at its own dispatch with the 400 a single request gets there, reaching nothing.

**Held.** A hedge holds the body to its last byte: `decide_on_head` sets `held`, and a
confirm that adds a hedge to a body already streaming redoes it held (12c). `spawn_hedge`
refuses a body that reached dispatch incomplete with a 500; nothing reaches that.

**Never failed over.** `failover_target` offers no leg a retry: the policy hears each
failure with `retryable` false, counts it, and its answer is ignored.

**Counted.** `hedged_requests`, `hedge_legs` (every leg, the decision's own included) and
`hedge_legs_cut`. Only the leg a client got counts in `requests` or `errors`: a leg's
failure is held as `error_owed` and counted if the leg is adopted.

**Not done.** The body streamed to every leg as it arrives.
