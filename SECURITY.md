# Security Policy

## Reporting a vulnerability

If you have discovered a security vulnerability in `llmbridge`, please report it to us privately. **Do not file a public GitHub issue.**

### How to report

Email: **security@kottosai.com**

Please include:

- A description of the vulnerability and its potential impact
- Steps to reproduce, or a proof-of-concept if you have one
- The version(s) of `llmbridge` affected
- Your name and how you would like to be credited in any public disclosure (or your preference to remain anonymous)

If you would like to encrypt your report, mention it in your initial email and we will exchange PGP keys. We will publish a static PGP key at `https://kottosai.com/.well-known/pgp-key.asc` once we have stood up that infrastructure.

### What to expect

- **Initial response:** within 2 business days, acknowledging receipt.
- **Triage and assessment:** within 7 business days, including our assessment of severity and an estimated timeline to a fix.
- **Fix and disclosure:** we will work with you on coordinated disclosure. Our default is to release the fix and publish an advisory within 90 days of the initial report, but we will adjust based on severity and complexity.

### Scope

This policy covers the `llmbridge` library itself, including:

- The C++ core
- Official language bindings (Python, Go, Rust) published by Kottos AI
- Build and packaging infrastructure
- Official Docker images published under `kottosai/*`

Out of scope:

- Vulnerabilities in build-time or test-time tooling (please report those to the upstream maintainers, though we are happy to coordinate if it affects our users). Note that `llmbridge` has **zero runtime dependencies** by design, so this is rarely relevant.
- Vulnerabilities in user code that consumes `llmbridge`
- Issues in commercial Kottos AI products (those have a separate disclosure path; email `security@kottosai.com` and we will route appropriately)
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

## Security best practices for users

If you are deploying `llmbridge` in production, consider:

- **Pin to specific versions** rather than tracking `main` or `latest`. We use semantic versioning; pre-1.0 versions may include behavior changes across minor releases.
- **Subscribe to security advisories.** GitHub will notify you via the "Watch → Custom → Security alerts" setting on the repository.
- **Audit your build pipeline.** `llmbridge` itself has zero runtime dependencies, but your build environment and test/benchmark tooling do — pin those versions and review them as you would any third-party tooling.
- **Validate untrusted input.** `llmbridge` translates LLM API payloads; if those payloads come from untrusted sources (end users), validate and sanitize before passing them through. Translation is fast but not a substitute for input validation.

## Contact

- Security reports: `security@kottosai.com`
- General inquiries: `hello@kottosai.com`
