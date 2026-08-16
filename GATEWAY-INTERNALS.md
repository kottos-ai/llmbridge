# Gateway internals: two backends, one request lifecycle

How a request actually moves through `llmbridge`, on both event-loop backends.
`DESIGN.md` is the architecture summary and `LATENCY.md` defines every timing
number; this document is the mechanism: which buffer holds what, who owns each
piece of mutable state, when a connection may be freed, and where a credential
is scrubbed.

It exists because the gateway is implemented **twice**. `Gateway` runs the whole
lifecycle on epoll and again on io_uring, fourteen method pairs at 60 to 81
percent token similarity. That is the single most likely place for a fix to land
on one side only, and io_uring is the default on Ubuntu 24.04, so a one-sided fix
usually breaks the shipped path. Reading one backend and assuming the other
matches is the mistake this document exists to prevent.

## 1. The cast

Two connection kinds, and the vocabulary is used consistently everywhere:

| | |
|---|---|
| **client conn** | a customer's connection to us. `is_client == true` |
| **upstream conn** | our connection to the provider. `is_client == false` |

They are linked by `peer` for the duration of one request, and unlinked when it
completes. An upstream with no peer is either idle in the pool or dying.

## 2. The buffers

`r` and `w` are named from the **gateway's** point of view on that socket: `rbuf`
is what we read from the peer, `wbuf` is what we write to it. The same field name
therefore holds the REQUEST on one connection and the RESPONSE on the other, and
which is which depends on `is_client`. That is the most confusing thing about the
data model, so here is one request crossing all four:

```
   client                        gateway                        provider
     |                                                             |
     |  request bytes                                              |
     |------------------> c->rbuf ---[frame, translate]---> u->wbuf|
     |                                                     |-------|--->
     |                                                             |
     |                                                    response |
     |  c->wbuf <---[translate back]--- u->rbuf <-------------------|
     |<------------------|                                         |
```

| buffer | on a client conn | on an upstream conn |
|---|---|---|
| `rbuf` | the request the customer sent | the response the provider sent |
| `wbuf` | the response we send the customer | the request we send the provider |
| `woff` | how much of `wbuf` has been handed to the transport | same |
| `tls_out` | ciphertext heading for the socket (TLS conns only) | same |
| `wpending` | io_uring streaming staging area | unused |

Both `rbuf` and `wbuf` are **plaintext, always**, whether or not TLS is in use.
TLS interposes at the socket edge only, so HTTP framing, the SSE pump and the
stale-connection resend never know it exists.

### `woff` is not "bytes sent"

Its meaning shifts with the transport, and this is a real trap:

- **plaintext**: bytes actually written to the socket
- **TLS**: bytes fed into the `Session`, which have NOT necessarily left the
  machine. Wire progress is `tls_out_off`

So on a TLS connection `woff >= wbuf.size()` means "fully encrypted", never
"fully sent". Ask `tls_wbuf_flushed()` for the second question.


### 2b. `woff`, whose meaning shifts with the transport

| transport | what `woff` counts |
|---|---|
| plaintext | bytes actually written to the socket |
| TLS | bytes fed into the Session, which is NOT bytes on the wire. Wire progress is `tls_out_off`, and `woff` can reach `wbuf.size()` while nothing has left the machine |

The write path deliberately does **not** clear `wbuf` when `woff` catches up.
Callers do, at points they choose, and that is what keeps an upstream request
available for a resend when a pooled connection turns out to be dead. See
`ep_retry_upstream` / `ur_retry_upstream`, which resend only when the connection
came from the pool and no response byte has arrived.

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
 translate OpenAI -> provider dialect (provider/translate.cpp)
   |
   v
 acquire upstream: pooled keep-alive, or a fresh connect
   |
   v
 write u->wbuf  (credential mapped: Bearer -> x-api-key / x-goog-api-key)
   |
   v
 read provider bytes ---> u->rbuf
   |
   +--- text/event-stream 200 ---> STREAMING PUMP (section 6)
   |
   v
 frame response, translate back, write c->wbuf
   |
   v
 finish: release the upstream to the pool, or close it
```

Everything above is shared. What differs is **how the I/O is issued and how
completion is discovered**, which is section 4 and section 5.

## 4. The epoll backend

Level-triggered epoll with optimistic writes. The loop owns the syscalls.

```
  epoll_wait
      |
      +-- listen fd readable ------> ep_on_accept()      accept() in a loop
      |
      +-- client fd readable ------> ep_on_client_readable()
      |                                read() -> rbuf -> frame -> ep_forward()
      |
      +-- client fd writable ------> ep_on_client_writable()
      |                                drain wbuf, or ep_stream_flush()
      |
      +-- upstream fd readable ----> ep_on_upstream_readable()
      |                                read() -> rbuf -> frame -> respond
      |
      +-- upstream fd writable ----> ep_on_upstream_writable()
      |                                connect completed, or drain wbuf
      |
      v
  sweep_idle()          reap idle pooled upstreams, drop half-open clients
      |
      v
  free everything on _doomed        <- see section 7
```

**Optimistic write.** A response is written immediately and `EPOLLOUT` is armed
only on `EAGAIN`, so the common case costs one syscall. Arming unconditionally
would cost two `epoll_ctl` calls and an extra wakeup per request.

**Back-pressure.** When a client cannot keep up mid-stream, epoll **pauses the
upstream's `EPOLLIN`** (`ep_pause_read`), so the provider stops sending and the
kernel applies flow control for us. `read_paused` tracks it, and the
`ep_` prefix is what keeps io_uring from touching it.

## 5. The io_uring backend

Completion-driven, `SINGLE_ISSUER | DEFER_TASKRUN`, with multishot accept and
recv into a shared provided-buffer ring.

```
  submit SQEs                          reap CQEs
      |                                    |
      +-- IORING_OP_ACCEPT (multishot) ----+--> ur_on_accept()
      +-- IORING_OP_RECV   (multishot) ----+--> ur_on_recv()
      +-- IORING_OP_SEND               ----+--> ur_on_send()
      +-- IORING_OP_CONNECT            ----+--> ur_on_connect()
      +-- IORING_OP_TIMEOUT            ----+--> periodic tick
      +-- IORING_OP_ASYNC_CANCEL       ----+--> (control, not counted)
```

Buffer ring: `kUrBufCount = 4096` buffers of `kUrBufSize = 4096` bytes. Multishot
recv lands data in the ring instead of a per-connection buffer, so the kernel
picks the buffer and we recycle it after copying out.

Three constraints shape everything else here:

1. **An SQE is immutable once submitted.** The kernel reads the buffer the SQE
   points at, on its own schedule. Anything that could reallocate that buffer
   while an operation is in flight is a use-after-free.
2. **Two concurrent SENDs on one fd would interleave**, so sends are serialised.
3. **Completions arrive after the code has moved on**, so an object can be
   logically dead while the kernel still holds a reference to it.

### `send_inflight`: one owner

Means exactly "an SQE referencing this connection's send buffer is outstanding".

| | |
|---|---|
| **set** | `ur_submit_send()` only, the sole place an SQE is submitted |
| **cleared** | `ur_on_send()` on completion, and `ur_release_upstream()` |
| **read** | by anyone about to touch a send buffer, to decide whether to wait |

Callers must never set it. Two of them used to, under different rules, and
`ur_stream_flush()` setting it before calling into `ur_tls_flush()`, whose first
line refuses to run when it is already set, meant the first SSE flush on a TLS
connection did nothing and the stream hung forever, on io_uring only.

### `wpending`: why streaming needs a second buffer

`wbuf` is what a SEND SQE points at, so it must not move while that send is in
flight. Newly translated stream output therefore accumulates in `wpending` and is
moved into `wbuf` only when no send is outstanding. Past `kUrStreamBufCap` (8 MiB)
the stream is dropped.

**This is the one place the two backends deliberately differ.** Under a slow
client, epoll applies back-pressure by pausing the upstream read; io_uring bounds
`wpending` and drops the stream past the cap. Know which one you are reasoning
about before quoting streaming behaviour to a customer.


### 5b. The two `inflight` flags, and who owns each

Both live on `Connection` and both are io_uring-only. They are not the same
thing and confusing them has cost real bugs.

**`int inflight`**: submitted-but-uncompleted SQEs referencing this connection.
One protocol with `doomed` (section 7): doomed says we want the object gone,
inflight says whether the kernel agrees yet.

| | where |
|---|---|
| incremented | the three `ur_submit_*` functions, each at its tail immediately before `return true`, so an early return cannot leak a slot and strand the object forever |
| decremented | exactly one place, `ur_on_cqe()`, and only when the completion is NOT armed: a multishot op carrying `F_MORE` is still outstanding and has not released its slot |
| read | `ur_maybe_free()`, the only thing allowed to conclude that freeing is safe |

Multishot recv lands data in a shared provided-buffer pool, so there is no
per-connection recv buffer to account for.

**`bool send_inflight`**: an SQE referencing this connection's *send* buffer is
outstanding. Two concurrent SENDs on one fd would interleave, and an SQE is
immutable once submitted, so the buffer it points at must not move while this is
set.

| | where |
|---|---|
| set | only `ur_submit_send()`, the only place a send SQE is submitted |
| cleared | `ur_on_send()` on completion, and `ur_release_upstream()` when per-request state is reset |
| read | anyone about to touch a send buffer, to decide whether to wait |

**Callers must never set it.** Two of them used to, under two different rules,
and `ur_stream_flush()` setting it before calling `ur_tls_flush()` (whose first
line refuses to run when it is already set) meant the first SSE flush on a TLS
connection did nothing at all. The stream then hung forever, on io_uring only.

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
             epoll:  -> c->wbuf -> ep_stream_flush()
             uring:  -> c->wpending -> ur_stream_flush() -> c->wbuf -> SEND
                                |
                                v
                          client sees chunks
```

Three flags carry the state, and their meanings are precise:

- **`streaming`** is a one-way latch, never cleared. A streamed client response
  is close-delimited, so the connection ends with the stream.
- **`stream_ended`** means no further output will be produced. It becomes true
  two ways: cleanly, when the translator emitted its terminal `[DONE]`; or by
  truncation via `stream_truncate()`, which emits **no** `[DONE]` on purpose so a
  client sees a cut-off stream instead of a fabricated clean finish. So
  `stream_ended && close_after_resp` is the truncated case and
  `stream_ended && !close_after_resp` the clean one.
- **`read_paused`** is epoll-only back-pressure, described above.

`stream_ended` also gates `finalize_stream`. Without it the gateway resumes
reading from an upstream that will never speak again, and the client socket is
held until the idle sweep.


### 6b. `stream_ended`, and the difference a client can see

It becomes true two ways, and only one of them is a finished answer:

| | what happened | `close_after_resp` |
|---|---|---|
| **clean** | `stream_step()` saw the end of the body and the SSE translator emitted its terminal `[DONE]` | false |
| **truncated** | the stream is being aborted, so NO `[DONE]` is emitted, deliberately: a client must see a cut-off stream instead of a fabricated clean finish. Always via `stream_truncate()` | true |

So `stream_ended && close_after_resp` is the truncated case. That pairing is the
difference between a client believing it received the whole answer and knowing
it did not, so read it, never infer it.

### 6c. The one place the backends deliberately differ

Under a slow client, **epoll** pauses upstream `EPOLLIN` and applies
back-pressure. **io_uring** instead bounds `wpending` with `kUrStreamBufCap` and
drops the stream past the cap. Know which you are reasoning about before quoting
streaming behaviour to a customer.

`read_paused` is epoll's half and is owned entirely by `ep_pause_read` /
`ep_resume_read`, the only writers, both guarding on the current value. io_uring
never touches it, and the `ep_` prefix is what keeps that true.

## 7. Connection lifetime, and why the two backends free differently

This is where a mistake is a use-after-free in a process holding customer
credentials, so the rules are worth stating exactly.

**Both backends defer the free**, for different reasons:

| | why deferred | freed when |
|---|---|---|
| **epoll** | one event can close a pair, and a later event in the SAME batch would dereference a freed pointer | end of the current event batch, unconditionally |
| **io_uring** | a submitted SQE references the object, and the kernel does not care that we decided to close | `inflight` reaches 0, possibly several loop iterations later |

```
  ep_close_client / ep_close_upstream        ur_close()
        doomed = true                          doomed = true
        push to _doomed                        push to _doomed
              |                                shutdown + IORING_OP_ASYNC_CANCEL
              v                                      |
   end of batch: delete every _doomed                v
                                          ur_on_cqe: --inflight
                                                     |
                                                     v
                                          ur_maybe_free(): free iff
                                          doomed && inflight == 0
```

`inflight` counts submitted-but-uncompleted SQEs for one connection:

- **incremented** by the three `ur_submit_*` functions, each at its tail
  immediately before `return true`, so an early return cannot strand the object
- **decremented** in exactly one place, `ur_on_cqe()`, and only when the
  completion is not armed: a multishot op carrying `F_MORE` is still outstanding
- **read** by `ur_maybe_free()`, the only thing allowed to conclude freeing is safe

**The unconditional frees are safe for two different structural reasons.** epoll
never increments `inflight`, so it is always 0. io_uring ends `run_uring()` with
`while (_uring_inflight > 0) { submit_and_wait(1); }`, draining to zero before it
returns, and `_uring_inflight` moves in lockstep with every connection's count.
A runtime check at those sites was added and removed: unreachable, untestable,
and therefore protection in appearance only.

## 8. Connection pooling

Upstream connections are keep-alive and reused. A streaming request would
otherwise pay a fresh connect every time, which measured as the dominant term in
time to first token.

```
  ur_acquire_upstream / ep_acquire_upstream
        |
        +-- pool non-empty? take the back, from_pool = true, ++upstream_reused
        |
        +-- else: new socket, connect (async on both backends)

  ...request completes...

  release: pool it ONLY if the response said keep-alive
        |
        +-- pool at kMaxIdleUpstreams (8192)? close instead of accumulate
        |
        +-- request not fully on the wire? close, ++upstream_unsent  <- see below
        |
        +-- otherwise: SCRUB (section 9), stamp ts_pooled, push to _idle_upstreams

  sweep_idle(): evict anything idle past kIdleUpstreamNs (30 s)
```

**Why the 30 s reap.** Providers drop idle keep-alives on their own schedule, and
discovering a corpse costs a request its retry. Reaping first is cheaper than
finding out the hard way.

**A response can beat our own request out the door, and such a connection must
not be pooled.** The upstream's recv is armed before the request is sent, so a
provider that answers early, a 413 or 401 on the headers of a large body, hands
us a complete response while the request is still half-written. Pooling that
connection scrubs and later overwrites a buffer the transport has not finished
with, and leaves a truncated request on the wire for the next client to inherit.
`upstream_request_sent()` is the predicate, checked by both backends before
anything else in release, and the connection is closed instead. It fails closed
because the provider's view of that connection is not something we can
reconstruct. Reproduced on both backends by `ProxyEarlyResponse`, where the next
client's request was silently dropped, and mutation-guarded on each backend
separately.

**Stale-connection retry.** A pooled connection that fails before any response
byte arrives is retry-eligible (`from_pool && !retried`): the request is resent
on a fresh connection, and the client never sees the blip. `wbuf` is deliberately
NOT cleared by the write path for exactly this reason, so the request survives to
be resent.


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
  ep_release_upstream / ur_release_upstream
        |
        +-- u->rbuf.clear()        RESIDUE. Bytes past the framed response would
        |                          otherwise be read as the head of the NEXT
        |                          client's response: a cross-client desync
        |
        +-- u->rdec.reset()        chunked-decode state from the last response
        |
        +-- secure_clear(u->wbuf)  CREDENTIAL. wbuf held the rebuilt request
        |                          including the customer's API key
        |
        +-- u->woff = 0, msg = {}, tls_out.clear()
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
  socket <-send--  pull_ciphertext() <- [wbio] <- SSL <- write_plaintext() <-- wbuf
```

The same four calls serve both directions and both backends. Only the handshake
role differs (`SSL_set_accept_state` against `SSL_set_connect_state`) and what is
verified: the client leg presents a certificate and verifies nobody, the upstream
leg verifies the provider's chain AND its hostname, with no way to disable it.


### 10b. Two offsets, two questions

For a TLS connection, `wbuf` (plaintext) is not what gets written; the plaintext
stays intact there so a stale pooled connection can be retried.

| field | answers |
|---|---|
| `woff` | plaintext handed to the TRANSPORT. For TLS that means fed into the Session, which has not necessarily left the machine |
| `tls_out_off` | ciphertext that actually reached the socket |

So `woff >= wbuf.size()` means "fully encrypted", never "fully sent". Ask
`tls_wbuf_flushed()` for the second question.

A transport-agnostic wrapper over the two was tried and reverted: every caller
already sits inside a TLS-only branch, so it resolved to `tls_wbuf_flushed()` at
every site and read as a safety net while changing nothing.

### 10c. Why the TLS helpers take `c` and not `u`

They are no-op-safe building blocks shared by both backends. Since inbound TLS
landed they run on client connections too, and `u` means upstream everywhere else
in the file. Direction is read from `c->is_client`, never assumed.

## 11. Where each rule is enforced

Documentation drifts; these do not.

| rule | enforced by |
|---|---|
| no call crosses the `ep_`/`ur_` prefixes | `scripts/check_conventions.py`, CI |
| an unprefixed method really is shared | same |
| namespaces mirror directories | same |
| LATENCY.md's stamp table names real functions | same |
| constants are `kPascalCase`, types `PascalCase`, `ALL_CAPS` is macros only | same |
| the invariants above are actually tested | `private/scripts/mutate_check.py` |

The last one matters most and is the newest. A green suite proves nothing it
checks regressed; it does not prove it checks anything in particular. The
mutation harness breaks each invariant on purpose and reports whether any test
notices. It has found guards that were decoration, and test gaps in code that
looked covered.
