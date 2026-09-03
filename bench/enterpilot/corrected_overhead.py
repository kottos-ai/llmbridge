#!/usr/bin/env python3
# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
"""Overhead corrected for the extra network hop every gateway pays.

The harness reports added latency as (gateway - baseline). The baseline is
loadgen -> mock, one bridge traversal; every gateway arm is loadgen -> gateway ->
mock, two. So its overhead column contains one hop that belongs to the topology
to the topology and not to any gateway, and it inflates every row equally.

The tcprelay arm is a socat byte pipe: two hops, no HTTP parsing, no decisions. Its
overhead is that hop. Subtracting it gives what a gateway costs above the floor of
merely being in the path.

  bench/enterpilot/corrected_overhead.py bench/.enterpilot/remote/results
"""
import json
import sys
from pathlib import Path

FLOOR = "tcprelay"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    path = Path(sys.argv[1]) / "summary.json"
    try:
        data = json.loads(path.read_text())
    except OSError as e:
        print(f"cannot read {path}: {e}", file=sys.stderr)
        return 1

    lat = data.get("latency", {})
    floor = lat.get(FLOOR)
    if not floor:
        print(f"no {FLOOR} arm in this run, so the hop cannot be subtracted.")
        print(f"Re-run with {FLOOR} included; it is in the default gateway list.")
        return 1

    variants = [v for v in floor if floor[v].get("overhead_p50") is not None]
    print()
    print("OVERHEAD ABOVE THE IN-PATH FLOOR  (ms, p50)")
    print(f"the {FLOOR} row is a byte pipe: its cost is the extra bridge hop itself")
    print("-" * 78)
    print(f"{'gateway':<22}" + "".join(f"{v:>18}" for v in variants))
    print("-" * 78)
    for name, arms in lat.items():
        if name in ("baseline", FLOOR):
            continue
        cells = ""
        for v in variants:
            own = arms.get(v, {}).get("overhead_p50")
            hop = floor.get(v, {}).get("overhead_p50")
            if own is None or hop is None:
                cells += f"{'-':>18}"
            else:
                cells += f"{own - hop:>18.3f}"
        print(f"{name:<22}{cells}")
    print("-" * 78)
    hops = ", ".join(
        f"{v}={floor[v]['overhead_p50']:.3f}" for v in variants
        if floor[v].get("overhead_p50") is not None)
    print(f"hop subtracted: {hops}")
    print("A negative number means the gateway measured faster than the byte pipe,")
    print("which is noise, not a result: read it as zero and widen the sample.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
