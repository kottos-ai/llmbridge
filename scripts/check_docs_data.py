#!/usr/bin/env python3
# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
"""Every file a document points at must exist.

    python3 scripts/check_docs_data.py

A benchmark claim is only as good as the data under it, and the cheapest way for that
to rot is a pointer that no longer resolves. It happened here: the concurrent-stream
capacity table cited `bench/results/stream-steadystate.csv`, which is the 4,096-stream
latency run and says so on its own first line. The table stood for weeks with a
citation to someone else's data.

Backtick-quoted paths only, and only ones that look like a repository path (they
contain a slash and start with a directory that exists). A prose mention of `--flag`
or `Content-Length` is not a path, and neither is `foo.cpp` on its own.
"""
import os
import re
import sys

DOCS = ["README.md", "BENCHMARKS.md", "DESIGN.md", "LATENCY.md", "SECURITY.md",
        "CONTRIBUTING.md", "GATEWAY-INTERNALS.md"]
# A path with a directory component whose first segment is a real directory here.
PATH = re.compile(r"`([A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+)`")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tracked = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in {".git", "_deps"} and not d.startswith("build")]
        for fn in filenames:
            tracked.append(os.path.relpath(os.path.join(dirpath, fn), root))
    missing = []
    checked = 0
    for doc in DOCS:
        full = os.path.join(root, doc)
        if not os.path.exists(full):
            continue
        with open(full) as f:
            text = f.read()
        for m in PATH.finditer(text):
            path = m.group(1).rstrip("/")
            top = path.split("/")[0]
            # Only paths rooted in a directory this repository actually has, so a
            # URL fragment or an example path from another machine is not a finding.
            if not os.path.isdir(os.path.join(root, top)):
                continue
            if "..." in path:
                continue  # an elided path in prose, not a pointer
            checked += 1
            if os.path.exists(os.path.join(root, path)):
                continue
            # Header shorthand is deliberate in the design docs: `net/http.hpp` is how
            # the code includes it, and the file lives under net/include/net/. Accept a
            # unique suffix match, and only a unique one: two matches means the
            # reference is ambiguous and the reader has to guess.
            hits = [f for f in tracked if f.endswith("/" + path)]
            if len(hits) == 1:
                continue
            line = text[:m.start()].count("\n") + 1
            missing.append(f"{doc}:{line}: `{path}` does not exist"
                           + (f" ({len(hits)} ambiguous matches)" if hits else ""))
    if missing:
        for m in missing:
            print(f"  {m}", file=sys.stderr)
        print(f"docs data check: {len(missing)} broken pointer(s)", file=sys.stderr)
        return 1
    print(f"docs data check OK: {checked} in-repo path(s) referenced, all present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
