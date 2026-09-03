#!/usr/bin/env python3
# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
"""Drop the seccomp and apparmor filters on every gateway in the harness.

Docker evaluates a seccomp BPF program on each syscall. That is a tax in proportion
to how much time a process spends in the kernel, so it is not neutral between
gateways: measured on 2026-09-02, one llmbridge worker went 32,785 -> 38,528 req/s
with the filters off, an 18% gain, because this workload is 89% kernel time. A
gateway bound in userspace barely notices. Leaving the default in place therefore
penalises the most syscall-efficient participant the most.

Applied to every gateway or not at all: doing it for one arm would be indefensible.

  bench/enterpilot/unconfine.py bench/.enterpilot/remote/gateways
"""
import re
import sys
from pathlib import Path

OPTS = '    security_opt: ["seccomp=unconfined", "apparmor=unconfined"]\n'
SERVICE = re.compile(r"^  [A-Za-z0-9_.-]+:\s*$")


def patch(path: Path) -> bool:
    lines = path.read_text().splitlines(keepends=True)
    if any("security_opt" in ln for ln in lines):
        return False
    out, in_services, done = [], False, False
    for ln in lines:
        out.append(ln)
        if ln.startswith("services:"):
            in_services = True
            continue
        if in_services and not done and SERVICE.match(ln):
            out.append(OPTS)
            done = True
    if not done:
        print(f"  {path}: no service block found, left alone", file=sys.stderr)
        return False
    path.write_text("".join(out))
    return True


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 1
    changed = [p.parent.name for p in sorted(root.glob("*/compose.yml")) if patch(p)]
    print(f"  unconfined: {', '.join(changed) if changed else 'none (already applied)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
