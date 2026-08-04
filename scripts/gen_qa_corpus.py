#!/usr/bin/env python3
# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Generate the question/answer corpus used by the concurrency regression test.

Run ONCE, offline; the output is committed as a fixture. The test itself is
hermetic and never touches the network — see CLAUDE.md, "Never write tests that
depend on live LLM provider APIs". This is the "record once, replay in tests"
half of that rule.

Why real answers rather than synthetic strings: the translator's job is to move
text across a JSON boundary intact, and real model output is what actually
contains the hostile characters — embedded double quotes, newlines, backslashes,
em-dashes, accented letters, emoji, code fences. A corpus of "answer_0001" would
exercise none of it. Every byte of these answers has to survive
Anthropic-JSON -> parse -> OpenAI-JSON re-serialisation unchanged.

Credential handling (see CLAUDE.md, private/demo): the key is read from a
mode-600 file, passed only in a request header, and never printed, logged, put in
argv, or written to the output. Use a throwaway key.

    python3 scripts/gen_qa_corpus.py --out gateway/tests/data/qa_corpus.jsonl \
        -n 2000 --max-answer-bytes 6144

The cap is part of the committed fixture, not a tuning knob: without it the
backend_stress answers come back at ~15 KB and the file is 4.25 MB. Re-running
WITHOUT the flag on an existing corpus will not restore the full text (it is
gone from the file), but a fresh generation would — so keep the flag.

Resumable: an existing output file is loaded and only missing ids are fetched, so
an interrupted run costs nothing.
"""

import argparse
import json
import os
import random
import re
import stat
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

API = "https://api.anthropic.com/v1/messages"
# Haiku on purpose: this is fixture generation, so latency and cost dominate and
# answer quality is irrelevant to what the test asserts.
MODEL = "claude-haiku-4-5-20251001"

TOPICS = [
    "geography", "mathematics", "house chores", "politics", "cooking", "world history",
    "biology", "sports", "music theory", "film", "chemistry", "personal finance",
    "gardening", "car maintenance", "first aid", "linguistics", "astronomy",
    "computing", "law", "etiquette", "travel logistics", "pet care", "weather",
    "art history", "mythology", "economics", "psychology", "nutrition", "home DIY",
    "photography", "board games", "statistics", "geology", "medicine", "architecture",
    "textiles and sewing", "agriculture", "public transport", "energy", "philosophy",
]

# ---------------------------------------------------------------------------
# Corpus categories. Each entry carries a `kind` saying what it stresses, so a
# failure points at a character class rather than at "one of 2000 answers".
# (The old boolean field was called `adversarial`, which wrongly suggested an
# attacker; these are formatting-stress cases, not attacks.)
#
#   plain          general knowledge, ordinary prose answers
#   escape_stress  answers forced to contain JSON-hostile characters
#   json_hostile   answers ABOUT JSON escaping, so the text is full of
#                  backslashes, quotes and \uXXXX sequences the model wrote
#                  literally — the reply is itself near-JSON. It does NOT
#                  produce raw control bytes: asked about \u0000 the model
#                  writes the six characters, not the byte. Do not add prompts
#                  hoping to change that; that byte class is covered by a
#                  deterministic gateway test instead.
#   tricky_text    Unicode edge cases: RTL, ZWJ emoji, combining marks,
#                  surrogate-pair astral chars, CJK, Thai, maths alphabets
#   long           multi-paragraph answers (~1-3 KB)
#   backend_stress deliberately large answers (~4-16 KB). io_uring reads into a
#                  provided-buffer ring of kBufSize=4096, so anything past 4 KB
#                  spans multiple buffers and must be reassembled; epoll just
#                  grows rbuf. This is the axis on which the two backends differ,
#                  so it is where a one-sided bug would hide.

ESCAPE_STRESS = [
    'Answer with a sentence that contains a double quote, like He said "hello" to me.',
    "Answer with two short lines separated by a newline.",
    "Answer including a Windows file path with backslashes.",
    "Answer using an em-dash, an ellipsis, and a non-breaking hyphen.",
    "Answer including the accented words: café, naïve, Ø, ß, žluťoučký.",
    "Answer including two emoji.",
    "Answer with a tiny inline code snippet in backticks containing braces {}.",
    "Answer including a tab character between two words.",
    "Answer with a string containing a literal backslash-n sequence, not a newline.",
    "Answer including CJK text: 東京 and 北京.",
]

# Answers that are *about* escaping end up containing the escapes themselves.
JSON_HOSTILE = [
    "Which characters must be escaped inside a JSON string? Show each one literally.",
    "Show a JSON object whose string value contains an escaped double quote, and explain it.",
    "What does the escape sequence \\u0000 mean in JSON, and why is a raw control character illegal?",
    "Write one example of INVALID JSON and say exactly why it is invalid.",
    "How do you represent a Windows path C:\\Users\\me inside a JSON string?",
    "Explain the difference between a literal newline and the two characters backslash-n in JSON.",
    "How are characters outside the Basic Multilingual Plane encoded in JSON strings?",
    "Show a JSON string containing a tab, a form feed and a backspace escape.",
    "What is a UTF-16 surrogate pair and how does JSON write one?",
    "Show a JSON string value that itself contains a complete JSON object as text.",
    "Why is a trailing comma invalid in JSON, and which parsers accept it anyway?",
    "Show how to escape a forward slash in JSON and say whether it is required.",
]

TRICKY_TEXT = [
    "Answer including one Arabic phrase and one Hebrew phrase (right-to-left text).",
    "Answer including a family emoji made of joined emoji, and a flag emoji.",
    "Answer including letters with stacked combining diacritics.",
    "Answer including mathematical script capitals and a few maths operators.",
    "Answer including Thai and Devanagari words.",
    "Answer including Greek, Cyrillic and Armenian words.",
    "Answer including box-drawing characters arranged in a tiny table.",
    "Answer including musical symbols and chess piece characters.",
    "Answer including a word with a soft hyphen and one with a zero-width joiner.",
    "Answer including Japanese, Korean and Traditional Chinese in one sentence.",
    "Answer including currency symbols from five different countries.",
    "Answer including an astral-plane character such as an ancient script glyph.",
]

# max_tokens per kind — `long` and `backend_stress` are the point, so let them run.
KIND_TOKENS = {
    "plain": 400, "escape_stress": 400, "json_hostile": 700,
    "tricky_text": 500, "long": 1600, "backend_stress": 4000,
}
KIND_SUFFIX = {
    "plain": " Answer in at most 3 sentences.",
    "escape_stress": " Answer in at most 3 sentences.",
    "json_hostile": " Answer in at most 5 sentences, and show the literal characters.",
    "tricky_text": " Answer in at most 3 sentences.",
    "long": " Give a thorough, well-structured answer of several paragraphs.",
    "backend_stress": (" Give an exhaustive answer: at least 1500 words, with headings, "
                       "a numbered list of at least 25 items, and a short table."),
}

_key_lock = threading.Lock()
_printed = 0


def utf8_trim(text, limit):
    """Truncate to `limit` BYTES without splitting a UTF-8 sequence.

    Drops trailing continuation bytes, then a dangling lead byte, so the result is
    always decodable. Safe with respect to JSON escaping because the corpus stores
    DECODED text — escaping happens at serialisation, so there is no escape to split.
    """
    b = text.encode()
    if len(b) <= limit:
        return text, False
    b = b[:limit]
    while b and (b[-1] & 0xC0) == 0x80:
        b = b[:-1]
    if b and (b[-1] & 0x80):
        b = b[:-1]
    return b.decode("utf-8"), True


def char_classes(a):
    """The byte/character classes a corpus answer exercises."""
    s = set()
    if "\n" in a: s.add("newline")
    if '"' in a: s.add("dquote")
    if "\\" in a: s.add("backslash")
    if "\t" in a: s.add("tab")
    if any(ord(c) > 127 for c in a): s.add("non-ascii")
    if any(ord(c) > 0xFFFF for c in a): s.add("astral")
    if "\\u" in a: s.add("literal-u")
    if len(a.encode()) > 4096: s.add("over-4kib")
    return s


def curate(records, target, seed):
    """Keep `target` records, preserving every rare class at 100%.

    Order matters: rare classes first (they are irreplaceable), then the dedicated
    text-stress kinds, then enough large answers to cross the io_uring buffer, then
    ordinary prose to fill. Abundant classes (newline, non-ascii) survive the cut
    regardless, so nothing is spent protecting them.
    """
    rng = random.Random(seed)
    by_id = {r["id"]: r for r in records}
    for r in records:
        r["_cls"] = char_classes(r["a"])
    chosen = {}
    for cl in ("tab", "astral", "literal-u", "backslash"):
        for r in records:
            if cl in r["_cls"]:
                chosen[r["id"]] = r
    for r in records:
        if r["kind"] in ("escape_stress", "json_hostile", "tricky_text"):
            chosen[r["id"]] = r
    big = [r for r in records if "over-4kib" in r["_cls"] and r["id"] not in chosen]
    have_big = sum(1 for r in chosen.values() if "over-4kib" in r["_cls"])
    for r in big[: max(0, 90 - have_big)]:
        chosen[r["id"]] = r
    for kind, cap in (("plain", 250), ("long", target)):
        pool = [r for r in records if r["kind"] == kind and r["id"] not in chosen]
        rng.shuffle(pool)
        for r in pool[: max(0, min(cap, target - len(chosen)))]:
            chosen[r["id"]] = r
    out = sorted(chosen.values(), key=lambda r: r["id"])[:target]
    for r in out:
        r.pop("_cls", None)
    for r in records:
        r.pop("_cls", None)
    # Renumber so ids stay dense 0..N-1 — the test strides over the corpus by index.
    for i, r in enumerate(out):
        r["id"] = i
    return out


def load_key(path):
    p = os.path.expanduser(path)
    st = os.stat(p)
    if st.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        sys.exit(f"refusing to read {path}: mode is {oct(st.st_mode & 0o777)}, want 600")
    with open(p) as f:
        return f.read().strip()


def call(key, prompt, max_tokens, retries=6):
    """One Messages API call. Returns the assistant text."""
    body = json.dumps({
        "model": MODEL,
        "max_tokens": max_tokens,
        "messages": [{"role": "user", "content": prompt}],
    }).encode()
    for attempt in range(retries):
        req = urllib.request.Request(API, data=body, method="POST")
        req.add_header("content-type", "application/json")
        req.add_header("anthropic-version", "2023-06-01")
        req.add_header("x-api-key", key)  # header only — never argv, never logged
        try:
            with urllib.request.urlopen(req, timeout=90) as r:
                out = json.loads(r.read())
            return "".join(b.get("text", "") for b in out.get("content", []) if b.get("type") == "text")
        except urllib.error.HTTPError as e:
            # 429 rate limit / 529 overloaded / 5xx — back off and retry.
            if e.code in (429, 500, 502, 503, 529) and attempt < retries - 1:
                time.sleep(min(2 ** attempt, 30) + random.random())
                continue
            raise SystemExit(f"API error {e.code} (body withheld — may echo request)")
        except (urllib.error.URLError, TimeoutError):
            if attempt < retries - 1:
                time.sleep(min(2 ** attempt, 30) + random.random())
                continue
            raise
    raise SystemExit("exhausted retries")


def make_questions(key, n, seed, exclude):
    """Ask for varied questions in batches. `exclude` holds question text already
    in the corpus, so a top-up run cannot reintroduce a duplicate — the test
    asserts every question is unique, because two clients asking the same thing
    would make a swapped response undetectable."""
    rng = random.Random(seed)
    # Over-ask: dedup and length filtering discard some, and a short pool would
    # silently reduce the corpus rather than fail loudly.
    want_per_topic = max(3, (n * 2) // len(TOPICS) + 2)
    questions, seen = [], set(exclude)
    lock = threading.Lock()

    def batch(topic):
        prompt = (
            f"Write {want_per_topic} short, factual, self-contained questions about {topic}. "
            "Vary the phrasing and difficulty. Everyday questions are fine. "
            "Return ONLY a JSON array of strings, no prose, no markdown fence."
        )
        txt = call(key, prompt, 2000)
        txt = re.sub(r"^```(?:json)?|```$", "", txt.strip(), flags=re.M).strip()
        try:
            got = json.loads(txt)
        except json.JSONDecodeError:
            got = [ln.strip(" -\t\"") for ln in txt.splitlines() if ln.strip().endswith("?")]
        with lock:
            for q in got:
                if isinstance(q, str) and 8 < len(q) < 300 and q not in seen:
                    seen.add(q)
                    questions.append((topic, q))

    with ThreadPoolExecutor(max_workers=8) as ex:
        list(ex.map(batch, TOPICS))

    rng.shuffle(questions)
    if len(questions) < n:
        sys.exit(f"only produced {len(questions)} new unique questions, wanted {n}")
    return questions[:n]


# How the NEW half of the corpus is composed. `plain`/`escape_stress` already
# exist from the first run; these are the categories added on top.
NEW_MIX = [("long", 330), ("json_hostile", 260), ("tricky_text", 260), ("backend_stress", 150)]
BANKS = {"escape_stress": ESCAPE_STRESS, "json_hostile": JSON_HOSTILE, "tricky_text": TRICKY_TEXT}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("-n", type=int, default=2000)
    ap.add_argument("--key", default="~/.anthropic_key")
    ap.add_argument("--seed", type=int, default=20260803)
    ap.add_argument("--workers", type=int, default=12)
    # The backend_stress answers come back at ~15 KB and are 55% of the fixture, so
    # the committed corpus caps them. 6144 = 1.5x the io_uring provided-buffer size
    # (kBufSize = 4096), which is what those entries exist to cross — the property
    # survives the cap. Applied at WRITE time to every record, so re-running this
    # script reproduces the committed file instead of silently restoring 4.25 MB.
    ap.add_argument("--max-answer-bytes", type=int, default=0,
                    help="cap answer length in bytes (0 = no cap); marks records truncated")
    # Curation. Generating 2000 and keeping the best 1000 beats generating 1000:
    # the selector can then guarantee EVERY rare character class survives, which a
    # proportional sample does not — a naive stride-2 halving of this corpus drops
    # `tab` from 18 entries to zero. Deterministic, so the committed file is
    # reproducible from the same inputs.
    ap.add_argument("--curate", type=int, default=0,
                    help="keep the best N entries by character-class coverage (0 = keep all)")
    args = ap.parse_args()

    key = load_key(args.key)
    results = {}
    if os.path.exists(args.out):
        with open(args.out) as f:
            for line in f:
                if not line.strip():
                    continue
                rec = json.loads(line)
                # Migrate the first-run schema: a bare `adversarial` boolean becomes
                # a named kind. The field was misleading — those entries are
                # formatting stress, not attacks.
                if "kind" not in rec:
                    rec["kind"] = "escape_stress" if rec.pop("adversarial", False) else "plain"
                rec.pop("adversarial", None)
                results[rec["id"]] = rec
        print(f"resuming: {len(results)} records already present")

    missing = [i for i in range(args.n) if i not in results]
    if not missing:
        print("all answers present; applying options and rewriting")

    if missing:
        # Assign a kind to every missing slot, then fetch.
        plan = []
        for kind, count in NEW_MIX:
            plan += [kind] * count
        rng = random.Random(args.seed)
        if len(plan) < len(missing):
            plan += ["plain"] * (len(missing) - len(plan))
        plan = plan[:len(missing)]
        rng.shuffle(plan)

        print(f"fetching {len(missing)} new entries: " +
              ", ".join(f"{k}x{plan.count(k)}" for k in sorted(set(plan))))
        have_q = {r["q"] for r in results.values()}
        base = make_questions(key, len(missing), args.seed + 1, have_q)

        lock, done = threading.Lock(), [0]

        def fetch(slot):
            idx, kind = slot
            topic, q = base[idx]
            # For bank-backed kinds the stress instruction goes INTO the question, so
            # the corpus is self-describing and the lookup key stays unique.
            bank = BANKS.get(kind)
            if bank:
                q = f"{q} {bank[idx % len(bank)]}"
            ans = call(key, q + KIND_SUFFIX[kind], KIND_TOKENS[kind]).strip()
            if not ans:
                ans = "(no answer)"
            with lock:
                results[missing[idx]] = {"id": missing[idx], "topic": topic, "q": q,
                                         "a": ans, "kind": kind}
                done[0] += 1
                if done[0] % 50 == 0:
                    print(f"  {done[0]}/{len(missing)} answered", flush=True)

        with ThreadPoolExecutor(max_workers=args.workers) as ex:
            list(ex.map(fetch, list(enumerate(plan))))

    if args.max_answer_bytes:
        n_cut = 0
        for r in results.values():
            r["a"], cut = utf8_trim(r["a"], args.max_answer_bytes)
            r["truncated"] = cut
            n_cut += cut
        print(f"capped {n_cut} answers at {args.max_answer_bytes} B (UTF-8 safe)")

    ordered = [results[i] for i in range(args.n)]
    if args.curate:
        before = len(ordered)
        ordered = curate(ordered, args.curate, args.seed)
        print(f"curated {before} -> {len(ordered)} entries by character-class coverage")
    results = {r["id"]: r for r in ordered}

    tmp = args.out + ".tmp"
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(tmp, "w") as f:
        for r in ordered:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    os.replace(tmp, args.out)

    with open(args.out, "rb") as f:
        blob = f.read()
    if key.encode() in blob or b"sk-ant-" in blob:
        os.remove(args.out)
        sys.exit("ABORT: credential material found in the corpus; file deleted")

    qs = [r["q"] for r in results.values()]
    if len(set(qs)) != len(qs):
        sys.exit("ABORT: duplicate questions — the correlation check would be blind")

    counts = {}
    for r in results.values():
        counts[r["kind"]] = counts.get(r["kind"], 0) + 1
    print(f"wrote {args.out}: {len(results)} pairs, {len(blob)} bytes, all questions unique")
    for k in sorted(counts):
        sizes = [len(r["a"]) for r in results.values() if r["kind"] == k]
        print(f"  {k:<15} {counts[k]:>5}   answer bytes mean {sum(sizes)//len(sizes):>6} "
              f"max {max(sizes):>6}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
