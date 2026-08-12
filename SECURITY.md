# Security Policy

## Reporting a vulnerability

If you have discovered a security vulnerability in `llmbridge`, please report it to us privately. **Do not file a public GitHub issue.**

### How to report

Email: **security@kottos.ai**

Please include:

- A description of the vulnerability and its potential impact
- Steps to reproduce, or a proof-of-concept if you have one
- The version(s) of `llmbridge` affected
- Your name and how you would like to be credited in any public disclosure (or your preference to remain anonymous)

If you would like to encrypt your report, mention it in your initial email and we will exchange PGP keys on request.

### What to expect

- **Initial response:** within 2 business days, acknowledging receipt.
- **Triage and assessment:** within 7 business days, including our assessment of severity and an estimated timeline to a fix.
- **Fix and disclosure:** we will work with you on coordinated disclosure. Our default is to release the fix and publish an advisory within 90 days of the initial report, but we will adjust based on severity and complexity.

### Scope

This policy covers the `llmbridge` library itself, including:

- The C++ core
- Build and packaging infrastructure

Out of scope:

- Vulnerabilities in build-time or test-time tooling (please report those to the upstream maintainers, though we are happy to coordinate if it affects our users). Note that `llmbridge` has **zero runtime dependencies** by design, so this is rarely relevant.
- Vulnerabilities in user code that consumes `llmbridge`
- Issues in commercial Kottos AI products (those have a separate disclosure path; email `security@kottos.ai` and we will route appropriately)
- Denial of service via excessive resource consumption from well-formed inputs (please file as a performance issue instead)

### Recognition

With your consent, we will credit you publicly when we publish the fix, typically in the release notes and CHANGELOG entry for the patched version. We may set up a more formal security acknowledgments page (e.g., a hall of fame) as the project grows.

We do not currently offer a paid bug bounty. We might add one later as the project and company grow.

### Safe harbor

Kottos AI commits to:

- Not pursuing legal action against researchers who follow this policy in good faith
- Working with researchers to understand and validate reports
- Recognizing your contribution publicly (with your consent)
- Crediting you in release notes for the fix

We expect researchers to:

- Make a good-faith effort to avoid privacy violations, data destruction, and service disruption
- Only interact with accounts you own or have explicit permission to test
- Give us reasonable time to fix issues before public disclosure
- Not exploit vulnerabilities beyond what is necessary to demonstrate the issue

## Threat model and deployment constraints

**Read this before deploying.** `llmbridge` forwards *your* provider API key
upstream, so where you run it, and how you start it, determine whether that key is
exposed.

### The two network legs, and what protects each

| leg | protection | how to get it |
|---|---|---|
| client to gateway (**inbound**) | TLS 1.2+, terminated by the gateway | build `-DLLMBRIDGE_TLS=ON`, run `--listen-tls --tls-cert PATH --tls-key PATH` |
| gateway to provider (**outbound**) | TLS with certificate *and* hostname verification | build `-DLLMBRIDGE_TLS=ON`, use `--upstream https://...` |

**Neither is on by default.** The default build has no TLS at all, and without
`--listen-tls` the listener speaks plain HTTP, so a client's `Authorization` header
crosses the inbound hop in cleartext.

**One listener, one mode.** `--listen-tls` makes the single listener TLS-only. There
is no second plaintext port, so "am I exposed in the clear?" is answered by reading
the command line. Local plaintext debugging needs a second process, deliberately.

**The binary refuses half-configurations instead of downgrading.** `--listen-tls`
without a certificate, a certificate without `--listen-tls`, and `--listen-tls` on a
build without TLS are all startup errors. That last one matters most: it is the case
where the operator asked for encryption and would otherwise have got plaintext.

### Choosing a deployment

- **Loopback sidecar**, on `127.0.0.1`, in the same pod, host or container as the
  application calling it. No network segment to observe, so plaintext inbound is not
  an exposure and TLS is unnecessary. This is the simplest supported deployment.
- **Remote endpoint**, reachable from another machine. Requires `--listen-tls`.
  Understand before doing it that the gateway has **no client authentication**:
  anything that can reach the listener can use it and supply its own key. TLS keeps
  the credential off the wire; it does not decide who may connect. Put an
  authenticating layer in front, or restrict reachability at the network level.

The gateway cannot tell whether its listener is reachable from outside the host. It
warns at startup when the *upstream* is plaintext and not loopback; there is no
equivalent warning for a plaintext listener, so that constraint is yours to enforce.

### What the gateway does protect

- **Certificate and hostname verification on the upstream cannot be disabled.** There
  is no `insecure` flag, by design. Chain and hostname are both checked, derived from
  the same argument so they cannot disagree.
- **The inbound listener fails closed at startup**: a private key readable beyond its
  owner, a key that does not match the certificate, an already-expired certificate and
  an unreadable path are each a distinct startup error, not a warning.
- **ALPN advertises `http/1.1` only** and fails the handshake when a client offers
  ALPN without it, so a client speaking only `h2` gets a protocol error instead of a
  confusing parse failure. TLS renegotiation is disabled.
- **Credentials are never logged**, never placed in an error body, metric or stats
  output, and the pooled upstream request buffer is scrubbed on release instead of
  merely cleared.
- **Client headers are not echoed.** A translated request is rebuilt from an explicit
  whitelist, so cookies, tracing headers and anything else a client sends do not reach
  the provider.
- **Malformed input fails closed**: a malformed credential returns `400` without
  contacting the upstream at all.
- **An unfinished handshake cannot buffer without bound.** Measured, not assumed: a
  peer that dribbles a record or declares a 16 MB ClientHello is refused at roughly
  16 KiB, the TLS record ceiling. A connection that never completes setup is closed
  after 30 seconds.

### Known gaps, stated instead of implied

- **No client authentication.** Anything that can reach the listener can use it and
  supply its own key. There is no per-client key, quota or rate limit. This is the
  reason a remote deployment needs something in front of it.
- **No mutual TLS.** Client certificates are not requested or verified.
- **Certificate renewal requires a restart.** In-flight requests and streams are
  dropped when the process restarts. There is no reload-on-signal.
- **No OCSP/CRL revocation checking** on the upstream certificate. This is usual for
  non-browser TLS clients, but it is a deliberate choice instead of an oversight.
- **A provider EOF without `close_notify`** is treated as a normal end of a
  close-delimited stream. Strict truncation detection would break real providers.
- **Buffer scrubbing is targeted, not exhaustive**: the pooled request buffer is
  scrubbed because it can outlive its request; transient buffers are not, because they
  are overwritten within microseconds and doing so would put a `memset` on the hot path.
- **A slow client can hold memory.** Streaming output is bounded per connection (the
  upstream read is paused on one backend, the buffer capped at 8 MiB on the other),
  but that bound is per connection, not global.

## Security best practices for users

If you are deploying `llmbridge` in production, consider:

- **Pin to specific versions** instead of tracking `main` or `latest`. We use semantic versioning; pre-1.0 versions may include behavior changes across minor releases.
- **Subscribe to security advisories.** GitHub will notify you via the "Watch → Custom → Security alerts" setting on the repository.
- **Audit your build pipeline.** `llmbridge` itself has zero runtime dependencies, but your build environment and test/benchmark tooling do. Pin those versions and review them as you would any third-party tooling.
- **Validate untrusted input.** `llmbridge` translates LLM API payloads; if those payloads come from untrusted sources (end users), validate and sanitize before passing them through. Translation is fast but not a substitute for input validation.
- **Decide the listener's exposure deliberately.** Loopback needs nothing; anything
  reachable from another machine needs `--listen-tls` *and* an authenticating layer
  in front of it, because the gateway authenticates no one. See "Threat model and
  deployment constraints" above. This is the single most important deployment
  decision, because it is what keeps forwarded API keys off the network.

## Contact

- Security reports: `security@kottos.ai`
- General inquiries: `hello@kottos.ai`
