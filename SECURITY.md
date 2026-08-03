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

With your consent, we will credit you publicly when we publish the fix — typically in the release notes and CHANGELOG entry for the patched version. We may set up a more formal security acknowledgments page (e.g., a hall of fame) as the project grows.

We do not currently offer a paid bug bounty. We may add one in the future as the project and company grow.

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
upstream, so where you run it determines whether that key is exposed.

### The two network legs are not equally protected

| leg | protection | status |
|---|---|---|
| client → gateway (**inbound**) | **none — plain HTTP** | not implemented |
| gateway → provider (**outbound**) | TLS with certificate *and* hostname verification | build with `-DLLMBRIDGE_TLS=ON` |

There is no inbound TLS. A client's `Authorization` header therefore crosses the
client → gateway hop **in cleartext**.

### What this means in practice

- ✅ **Run it as a loopback sidecar** — on `127.0.0.1`, in the same pod/host/container
  as the application calling it. There is no network segment to observe, so plaintext
  is not an exposure. This is the supported deployment.
- ❌ **Do not expose the listener beyond localhost.** Binding it to a routable address,
  a shared Docker network, or a Kubernetes `Service` puts every client's API key on the
  wire in the clear. If you need a remote endpoint, terminate TLS in front of it with a
  reverse proxy you trust, or use a deployment where the key never crosses a network.

The gateway cannot detect this for you: it has no way to know whether its listener is
reachable from outside the host. It **does** warn at startup when the *upstream* is
plaintext and not loopback, but nothing warns about the inbound leg — that constraint
is yours to enforce.

### What the gateway does protect

- **Certificate and hostname verification cannot be disabled.** There is no `insecure`
  flag, by design. Both the chain and the hostname are checked, derived from the same
  argument so they cannot disagree.
- **Credentials are never logged**, never placed in an error body, metric or stats
  output, and the pooled upstream request buffer is scrubbed on release rather than
  merely cleared.
- **Client headers are not echoed.** A translated request is rebuilt from an explicit
  whitelist, so cookies, tracing headers and anything else a client sends do not reach
  the provider.
- **Malformed input fails closed** — a malformed credential returns `400` without
  contacting the upstream at all.

### Known gaps, stated rather than implied

- **No inbound TLS** (above).
- **No client authentication.** Anything that can reach the listener can use it and
  supply its own key. There is no per-client key, quota or rate limit — another reason
  the listener must not be exposed.
- **No OCSP/CRL revocation checking** on the upstream certificate. This is usual for
  non-browser TLS clients, but it is a deliberate choice rather than an oversight.
- **A provider EOF without `close_notify`** is treated as a normal end of a
  close-delimited stream. Strict truncation detection would break real providers.
- **Buffer scrubbing is targeted, not exhaustive** — the pooled request buffer is
  scrubbed because it can outlive its request; transient buffers are not, because they
  are overwritten within microseconds and doing so would put a `memset` on the hot path.

## Security best practices for users

If you are deploying `llmbridge` in production, consider:

- **Pin to specific versions** rather than tracking `main` or `latest`. We use semantic versioning; pre-1.0 versions may include behavior changes across minor releases.
- **Subscribe to security advisories.** GitHub will notify you via the "Watch → Custom → Security alerts" setting on the repository.
- **Audit your build pipeline.** `llmbridge` itself has zero runtime dependencies, but your build environment and test/benchmark tooling do — pin those versions and review them as you would any third-party tooling.
- **Validate untrusted input.** `llmbridge` translates LLM API payloads; if those payloads come from untrusted sources (end users), validate and sanitize before passing them through. Translation is fast but not a substitute for input validation.
- **Keep the listener on loopback** — see "Threat model and deployment constraints" above. This is the single most important deployment decision, because it is what keeps forwarded API keys off the network.

## Contact

- Security reports: `security@kottos.ai`
- General inquiries: `hello@kottos.ai`
