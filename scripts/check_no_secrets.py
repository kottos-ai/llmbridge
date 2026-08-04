#!/usr/bin/env python3
# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Fail the build if anything credential-shaped is tracked in git.

This project's whole credibility rests on credential handling: it is a gateway that
carries other companies' provider keys, and `private/` (which holds the live-key demo
scripts and the price observatory) sits inside the same working tree as the public
repo, separated only by .gitignore. One mis-added file is unrecoverable once pushed —
rotating the key fixes the key, not the git history or the forks of it.

So this checks what is actually TRACKED, not what is on disk. A key sitting in
`private/` or `~/.anthropic_key` is fine and expected; the same bytes in `git
ls-files` are not.

Scope note: this is a cheap last line of defence, not a replacement for gitleaks or
GitHub push protection. It looks for the shapes this project actually handles.
"""

import re
import subprocess
import sys

# (name, compiled pattern). Kept narrow on purpose: a noisy check that fires on
# ordinary code gets disabled, and a disabled check protects nothing.
PATTERNS = [
    ("Anthropic API key",   re.compile(rb"sk-ant-[A-Za-z0-9_\-]{20,}")),
    ("OpenAI API key",      re.compile(rb"sk-(?:proj-)?[A-Za-z0-9]{32,}")),
    ("AWS access key id",   re.compile(rb"AKIA[0-9A-Z]{16}")),
    ("Google API key",      re.compile(rb"AIza[0-9A-Za-z_\-]{35}")),
    ("Slack token",         re.compile(rb"xox[abprs]-[0-9A-Za-z\-]{10,}")),
    ("private key block",   re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH |PGP )?PRIVATE KEY-----")),
    ("generic bearer",      re.compile(rb"(?i)authorization:\s*bearer\s+[A-Za-z0-9_\-\.]{24,}")),
]

# Files that legitimately contain key-SHAPED text: tests and docs that demonstrate
# the redaction/refusal behaviour. Each entry needs a reason, and the pattern must
# still not be a real key.
ALLOW = {
    # Placeholder credentials in the auth tests are the point of those tests.
    "gateway/tests/gateway_test.cpp",
    "gateway/tests/gateway_corpus_test.cpp",
    # SECURITY.md documents what a leaked key looks like.
    "SECURITY.md",
}

# Extensions worth scanning. Binary blobs and images cannot hide a key you would
# ever paste, and scanning them makes the check slow and noisy.
SKIP_SUFFIX = (".png", ".jpg", ".jpeg", ".gif", ".pdf", ".svg", ".ico", ".woff", ".woff2")


def main():
    try:
        files = subprocess.run(["git", "ls-files", "-z"], capture_output=True, check=True
                               ).stdout.split(b"\0")
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("check_no_secrets: not a git repo / git unavailable", file=sys.stderr)
        return 2

    hits, scanned = [], 0
    for raw in files:
        if not raw:
            continue
        path = raw.decode("utf-8", "replace")
        if path.endswith(SKIP_SUFFIX) or path in ALLOW:
            continue
        try:
            with open(path, "rb") as f:
                blob = f.read()
        except (FileNotFoundError, IsADirectoryError, PermissionError):
            continue
        scanned += 1
        for name, pat in PATTERNS:
            for m in pat.finditer(blob):
                line = blob[: m.start()].count(b"\n") + 1
                # Show the shape, never the value.
                hits.append(f"{path}:{line}: {name} ({len(m.group(0))} bytes, value withheld)")

    if hits:
        print(f"SECRET SCAN FAILED — {len(hits)} match(es) in TRACKED files:\n", file=sys.stderr)
        for h in hits:
            print(f"  {h}", file=sys.stderr)
        print("\nIf this is a placeholder, add the path to ALLOW with a reason.\n"
              "If it is real: rotate the credential FIRST, then remove it from history —\n"
              "deleting the file in a new commit does not help.", file=sys.stderr)
        return 1

    print(f"secret scan OK — {scanned} tracked files, no credential material")
    return 0


if __name__ == "__main__":
    sys.exit(main())
