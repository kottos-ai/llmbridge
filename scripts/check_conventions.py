#!/usr/bin/env python3
# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Enforce the naming conventions that keep the two event-loop backends apart.

Gateway implements the whole request lifecycle twice: once on epoll, once on
io_uring, and the two halves are 60-81% similar. Neither backend's teardown,
write-arming or completion handling is valid in the other, so a call that crosses
from one into the other is a bug. The ep_/ur_ prefixes exist to make that
mechanically checkable; this script is what makes them an invariant instead of a
habit. See DESIGN.md "Naming conventions".

Three checks, each of which has caught a real defect:

  1. CROSSING, a ep_* function calling a ur_* one, or vice versa.
                  Caught ur_forward() calling the epoll error responder, which had
                  been invisible for as long as the epoll half was unprefixed.

  2. UNMARKED, an unprefixed Gateway method reachable from only ONE backend.
                  Unprefixed is supposed to mean "shared", so a one-sided unprefixed
                  method is a mislabelled backend-specific one. This is the check
                  that matters most: a crossing grep alone CANNOT catch it, because
                  the offending name has no prefix to grep for. Caught abort_pair,
                  which a prefix-only check had missed.

  3. NAMESPACE, a header whose namespace does not mirror its directory.
                  Caught llmbridge::http (should nest under net) and llmbridge::json
                  (should nest under provider).

Pure stdlib, no build required. Exits non-zero on any violation.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GATEWAY = ROOT / "gateway" / "src" / "gateway.cpp"

# Methods that are legitimately unprefixed despite not being called from both
# backends: entry points, accessors, and the ctor. Anything else that is
# one-sided is a naming bug, not an exception. Extend this list only with a
# reason, never to silence check 2.
UNPREFIXED_ALLOWED = {
    "Gateway",       # constructor
    "run",           # dispatches to run_epoll / run_uring
    "run_epoll",     # names its own backend
    "run_uring",     # names its own backend
    "request_stop",
    "stats",
    "bound_port",
}


def strip_comments(text):
    """Blank out comments while preserving line numbering."""
    out = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", out)


def method_spans(text):
    """{name: (start_line, first_line_idx, last_line_idx)} for Gateway:: definitions.

    Brace-matched, so a free function defined between two methods is NOT charged
    to whichever method happens to precede it, an error that produced a false
    finding when this analysis was first done by hand.
    """
    spans = {}
    for m in re.finditer(r"^[ \t]*(?:[\w:<>,\*&\s]+?)\bGateway::(\w+)\s*\(", text, re.M):
        open_brace = text.find("{", m.end() - 1)
        if open_brace < 0:
            continue
        depth, i = 0, open_brace
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        spans[m.group(1)] = (
            text[: m.start()].count("\n") + 1,
            text[:open_brace].count("\n"),
            text[:i].count("\n"),
        )
    return spans


def backend_strict(name):
    """Backend by PREFIX only, used for the crossing check.

    Deliberately excludes run_epoll/run_uring: they are the dispatch layer, and
    run_uring() legitimately tails into run_epoll() when io_uring init or the
    provided-buffer ring is unavailable. Treating them as crossing endpoints
    flags that documented fallback.
    """
    if name.startswith("ep_"):
        return "ep"
    if name.startswith("ur_"):
        return "ur"
    return None


def backend_reach(name):
    """Backend for REACHABILITY, including the two loop entry points.

    sweep_idle is called from run_epoll and from ur_on_cqe and is genuinely
    shared; without run_epoll counting as an epoll-side caller it would look
    one-sided and be reported as unmarked.
    """
    if name == "run_epoll":
        return "ep"
    if name == "run_uring":
        return "ur"
    return backend_strict(name)


def main():
    failures = []

    # ---------------------------------------------------------------- 1 & 2
    raw = GATEWAY.read_text()
    text = strip_comments(raw)
    lines = text.splitlines()
    spans = method_spans(text)
    if len(spans) < 40:
        print(f"error: only {len(spans)} Gateway methods parsed: the extractor is broken, "
              f"not the code. Refusing to report a clean run.", file=sys.stderr)
        return 2

    callees = {name: set() for name in spans}
    for name, (_, first, last) in spans.items():
        for idx in range(first, min(last + 1, len(lines))):
            for called in re.findall(r"\b(\w+)\s*\(", lines[idx]):
                if called != name and called in spans:
                    callees[name].add((called, idx + 1))

    # 1. crossings: prefixed functions only (see backend_strict)
    for caller, calls in callees.items():
        cb = backend_strict(caller)
        if cb is None:
            continue
        for called, line in sorted(calls):
            tb = backend_strict(called)
            if tb is not None and tb != cb:
                failures.append(
                    f"{GATEWAY.relative_to(ROOT)}:{line}: CROSSING: {caller}() calls "
                    f"{called}(); {cb} and {tb} teardown/IO are not interchangeable")

    # 2. unprefixed methods reachable from only one backend
    reached_by = {name: set() for name in spans}
    for caller, calls in callees.items():
        cb = backend_reach(caller)
        for called, _ in calls:
            if cb:
                reached_by[called].add(cb)
    for name, (decl_line, _, _) in sorted(spans.items()):
        if backend_reach(name) or name in UNPREFIXED_ALLOWED:
            continue
        sides = reached_by[name]
        if len(sides) == 1:
            only = "epoll" if "ep" in sides else "io_uring"
            pfx = "ep_" if "ep" in sides else "ur_"
            failures.append(
                f"{GATEWAY.relative_to(ROOT)}:{decl_line}: UNMARKED: {name}() is reachable "
                f"only from {only}, but an unprefixed name means shared. Rename to "
                f"{pfx}{name}, or add it to UNPREFIXED_ALLOWED with a reason")

    # ---------------------------------------------------------------- 3
    for header in sorted(ROOT.glob("*/include/**/*.hpp")):
        module = header.relative_to(ROOT).parts[0]
        body = header.read_text()
        m = re.search(r"^namespace\s+([\w:]+)", body, re.M)
        if not m:
            continue
        ns = m.group(1)
        ns_line = body[: m.start()].count("\n") + 1
        # gateway/ is the top-level namespace `llmbridge` by design; everything
        # else must contain its directory as a namespace segment.
        if module == "gateway":
            continue
        if module not in ns.split("::"):
            failures.append(
                f"{header.relative_to(ROOT)}:{ns_line}: NAMESPACE: `{ns}` does not mirror "
                f"its directory; expected a `{module}` segment (e.g. llmbridge::{module}::...)")

    # ---------------------------------------------------------------- 4
    # LATENCY.md section 2 attributes every timing stamp to the function that
    # assigns it. Verify each attribution still holds: the named method must exist
    # and must actually assign that stamp. Deliberately NOT line numbers -- those
    # rot on any edit above them, so every commit would have to re-check a doc, and
    # a reference that needs re-checking every commit is one nobody re-checks.
    # Function names move only when someone renames a function, which is exactly
    # when this table should be revisited anyway.
    latency = ROOT / "LATENCY.md"
    if latency.exists():
        doc = latency.read_text()
        lines = text.splitlines()
        row = re.compile(r"^\|\s*\*\*(t\d)\*\*\s*\|\s*`(\w+)`\s*\|.*\|([^|]*)\|\s*$", re.M)
        seen = 0
        for m in row.finditer(doc):
            stamp, ident, sites = m.group(1), m.group(2), m.group(3)
            doc_line = doc[: m.start()].count("\n") + 1
            fns = re.findall(r"`(\w+)`", sites)
            if not fns:
                failures.append(f"LATENCY.md:{doc_line}: STAMP: {stamp} (`{ident}`) names "
                                f"no assigning function")
                continue
            seen += 1
            for f in fns:
                if f not in spans:
                    failures.append(
                        f"LATENCY.md:{doc_line}: STAMP: {stamp} cites `{f}()`, which is "
                        f"not a Gateway method (renamed or removed?)")
                    continue
                _, lo, hi = spans[f]
                body = "\n".join(lines[lo:hi + 1])
                if not re.search(rf"\b{ident}\s*=", body):
                    failures.append(
                        f"LATENCY.md:{doc_line}: STAMP: {stamp} says `{f}()` assigns "
                        f"`{ident}`, but it does not")
        if seen < 7:
            failures.append(f"LATENCY.md: STAMP: parsed only {seen} of 7 stamp rows; "
                            f"the table or this check is broken")

    # ---------------------------------------------------------------- 5
    # Identifier casing (DESIGN.md "Naming conventions"). Documented and followed
    # by hand until now, which by this project's own standard means unenforced.
    #
    #   kPascalCase  compile-time constants
    #   PascalCase   types and enum values
    #   ALL_CAPS     PREPROCESSOR MACROS ONLY
    #
    # The last one is the rule with teeth. C++ constants obey scope, so they do
    # not need the shouting that warns about a macro; and a constant in ALL_CAPS
    # collides with any system header that defines the same name (ERROR, min and
    # max are all real examples). Reserving the shape for macros keeps that
    # collision impossible instead of unlikely.
    n_consts = n_types = 0
    src = []
    for d in ("gateway", "net", "provider", "app"):
        src += sorted((ROOT / d).rglob("*.hpp")) + sorted((ROOT / d).rglob("*.cpp"))

    # `constexpr <type> NAME =` / `[` / `{`, but NOT `constexpr <type> name(` which
    # is a constexpr FUNCTION and correctly snake_case.
    const_re = re.compile(r"\bconstexpr\b[\w:<>,\s\*&]*?\b(\w+)\s*(?:=|\[|\{)")
    type_re = re.compile(r"^\s*(?:struct|class)\s+(\w+)\s*(?:[:{]|$)", re.M)
    for f in src:
        rel = f.relative_to(ROOT)
        for n, line in enumerate(f.read_text().splitlines(), 1):
            code = line.split("//", 1)[0]
            if "constexpr" in code:
                for m in const_re.finditer(code):
                    name = m.group(1)
                    if name in ("constexpr", "static", "inline", "auto"):
                        continue
                    n_consts += 1
                    if re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
                        failures.append(
                            f"{rel}:{n}: CASING - constant `{name}` is ALL_CAPS, which is "
                            f"reserved for macros; use k{name.title().replace('_','')}")
                    elif not re.fullmatch(r"k[A-Z]\w*", name):
                        failures.append(
                            f"{rel}:{n}: CASING - constant `{name}` should be kPascalCase")
            for m in type_re.finditer(code):
                name = m.group(1)
                n_types += 1
                if not re.fullmatch(r"[A-Z]\w*", name):
                    failures.append(f"{rel}:{n}: CASING - type `{name}` should be PascalCase")

    # Same guard as the method extractor: a regex that silently stops matching
    # would otherwise report a clean run over nothing at all.
    if n_consts < 30 or n_types < 20:
        print(f"error: casing check inspected only {n_consts} constants and {n_types} "
              f"types; the extractor is broken, not the code.", file=sys.stderr)
        return 2

    # ---------------------------------------------------------------- report
    if failures:
        print(f"convention check FAILED ({len(failures)} violation"
              f"{'s' if len(failures) != 1 else ''}):\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        print("\nSee DESIGN.md \"Naming conventions\".", file=sys.stderr)
        return 1

    twins = sum(1 for n in spans if n.startswith("ep_") and "ur_" + n[3:] in spans)
    print(f"convention check OK: {len(spans)} Gateway methods, {twins} ep_/ur_ twin pairs, "
          f"0 crossings, 0 unmarked, namespaces mirror directories, "
          f"LATENCY.md stamp refs resolve, "
          f"{n_consts} constants + {n_types} types correctly cased")
    return 0


if __name__ == "__main__":
    sys.exit(main())
