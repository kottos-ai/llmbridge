# Contributing to llmbridge

Thanks for your interest in `llmbridge`. Before you spend time preparing a contribution, please read this document — our current contribution policy may differ from what you expect.

## Current policy: no external code contributions

`llmbridge` is developed and maintained by [Kottos AI, Inc.](https://kottos.ai) At this time, **we do not accept external pull requests for code, features, or bug fixes.** Incoming code-modifying PRs will be politely closed with a pointer to this document.

This is not personal, and it is not a reflection on your work. It reflects two things:

1. **Capacity.** We're a small team focused on building. Reviewing external code contributions thoroughly takes real engineering time, and we would rather invest that time in the project's direction.
2. **IP clarity.** Keeping all code authored by Kottos AI employees and contractors keeps copyright and license terms unambiguous, which matters for downstream users who need clean license terms (and for our own ability to evolve the license over time).

This policy is not permanent. As the project and the company grow, we may open up to contributions under a Contributor License Agreement. Watch this file for updates.

## What you *can* do (and we genuinely appreciate)

### Report bugs

Open an issue on GitHub. Please include:

- `llmbridge` version (`llmbridge --version` or the package version).
- OS / platform / compiler version.
- A minimal reproducer (the smallest input that demonstrates the bug).
- Expected vs. actual behavior.
- Any error messages or stack traces.

We read every bug report and triage them within a few business days. We prioritize bugs by impact, not order of submission.

### Suggest features

Open an issue and label it `feature-request`. Please describe:

- The use case you are trying to support.
- Why existing features do not cover it.
- What the ideal API or behavior would look like, if you have a view.

We do not promise to implement every suggestion, but we do read all of them, and many of our roadmap items come from user feature requests.

### Ask questions

Use [GitHub Discussions](https://github.com/kottosai/llmbridge/discussions) for usage questions, integration help, performance questions, and general conversation. Discussions are public and searchable, so your question helps the next person too.

For commercial inquiries or anything sensitive, email `hello@kottos.ai`.

### Share what you're building

We love hearing about projects using `llmbridge`. Drop a note in [Discussions](https://github.com/kottosai/llmbridge/discussions) under the "Show and tell" category. We are especially interested in latency-sensitive use cases (voice agents, agentic loops, trading agents, real-time systems).

### Fork it

The Apache 2.0 license gives you the right to fork, modify, and redistribute. If our policy or direction does not fit your needs, fork freely — that is what open source is for. We will continue to maintain the canonical repository.

## What we currently accept

The only exception to the no-contributions policy:

- **Documentation typos, broken links, and obvious factual errors.** Please keep PRs to single, focused fixes. Anything that is "a few words in the README" is welcome; anything that rewrites paragraphs of documentation is not.

If you submit a documentation fix PR, we will review and merge it (or close it with a note) within a few business days.

## What we do not accept

To set expectations clearly:

- ❌ Pull requests adding features
- ❌ Pull requests fixing bugs (please open an issue instead and we'll fix it)
- ❌ Pull requests refactoring code
- ❌ Pull requests adding new provider integrations
- ❌ Pull requests adding tests for existing functionality
- ❌ Pull requests changing build configuration, CI, or tooling
- ❌ Pull requests changing license terms or copyright headers
- ❌ "Drive-by" PRs that touch many unrelated files

We will close these with a brief, polite note pointing here. Please do not take it personally.

## Code of conduct

This project follows the [Contributor Covenant v2.1](./CODE_OF_CONDUCT.md). Be respectful in issues, discussions, and any other interaction in the project's spaces. Harassment, personal attacks, and discrimination are not tolerated and will result in a ban.

## Reporting security issues

**Do not file security issues in public GitHub issues.** See [SECURITY.md](./SECURITY.md) for our responsible disclosure process.

## License

By interacting with this project (filing issues, participating in discussions, submitting documentation fixes), you affirm that you have read and accept the [Apache License 2.0](./LICENSE) under which `llmbridge` is distributed.

Documentation contributions are **licensed, not assigned**. Under Section 5 of the Apache License 2.0, by submitting a documentation PR you license your contribution under the same Apache 2.0 terms (inbound = outbound) to Kottos AI, Inc. and to all downstream users. You retain copyright in your contribution. (We do not require a CLA or copyright assignment today; if that changes, this document will say so before any such terms apply.)

---

Thanks for understanding. We know this policy is more restrictive than typical OSS projects. The tradeoff buys us focus and clarity now, and we may open it up as the project matures.

— The Kottos AI team
