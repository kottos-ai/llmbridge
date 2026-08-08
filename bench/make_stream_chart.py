#!/usr/bin/env python3

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Render the STREAMING (SSE) llmbridge-vs-LiteLLM charts as standalone SVGs.

Pure stdlib (no matplotlib), same geometry/palette as make_chart.py so the
streaming and non-streaming figures sit together on a page without clashing.

Two figures, deliberately chosen:

  stream-comparison.svg  time to first token vs concurrent streams (log y)
  stream-saturation.svg  chunks delivered as % of the achievable floor

Why THESE two metrics and not per-token added latency: TTFT and delivery-rate are
measured reliably for BOTH systems at every level. Per-token latency for LiteLLM
exceeds the load generator's 2 s histogram range at >=256 streams, so plotting it
would imply a precision we do not have; that number belongs in BENCHMARKS.md as
">2 s", with the caveat attached. Charts should not launder an overflow into a
confident-looking bar.

  python3 bench/make_stream_chart.py   # -> bench/results/stream-*.svg
"""
import csv
import math
import os
import statistics as st
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "results", "stream-comparison.csv")


def chart_outputs(name):
    """bench/results/ always (the OSS README embeds these); private/website/assets/
    too when that tree exists, so README and deployed site never drift."""
    outs = [os.path.join(HERE, "results", name)]
    site = os.path.join(os.path.dirname(HERE), "private", "website")
    if os.path.isdir(site):
        assets = os.path.join(site, "assets")
        os.makedirs(assets, exist_ok=True)
        outs.append(os.path.join(assets, name))
    return outs


# ── load + median across repetitions ────────────────────────────────────────
runs = defaultdict(list)
with open(CSV) as f:
    for r in csv.DictReader(f):
        if r["label"] == "discard":
            continue
        runs[(r["label"], int(r["streams"]))].append(r)

LEVELS = sorted({k[1] for k in runs})
REPS = min(len(v) for v in runs.values())


def med(label, streams, field):
    return st.median(float(x[field]) for x in runs[(label, streams)])


# Geometry / palette: identical to make_chart.py.
W, H = 1000, 620
ML, MR, MT, MB = 96, 44, 116, 116
PW = W - ML - MR
PH = H - MT - MB
F_TITLE, F_SUB, F_AXIS, F_VAL, F_XLBL, F_LEG, F_ANNOT = 27, 15, 15, 16, 18, 15, 13

LLMBRIDGE = "#16a34a"   # green: semantic pass/fail against red, NOT brand colour
LITELLM = "#dc2626"
FLOOR = "#2563eb"
GRID = "#e5e7eb"
INK = "#111827"
SUB = "#6b7280"


def head(svg, title, sub1, sub2):
    svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
               f'viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">')
    svg.append(f'<rect width="{W}" height="{H}" fill="white"/>')
    svg.append(f'<text x="{ML}" y="44" font-size="{F_TITLE}" font-weight="700" fill="{INK}">{title}</text>')
    svg.append(f'<text x="{ML}" y="74" font-size="{F_SUB}" fill="{SUB}">{sub1}</text>')
    svg.append(f'<text x="{ML}" y="95" font-size="{F_SUB}" fill="{SUB}">{sub2}</text>')


def legend(svg, items):
    x = ML
    y = H - 34
    for colour, label in items:
        svg.append(f'<rect x="{x}" y="{y - 11}" width="14" height="14" fill="{colour}" rx="2"/>')
        svg.append(f'<text x="{x + 21}" y="{y + 1}" font-size="{F_LEG}" fill="{SUB}">{label}</text>')
        x += 26 + len(label) * 8


def xband(i):
    """Left edge + width of the i-th concurrency band."""
    bw = PW / len(LEVELS)
    return ML + i * bw, bw


# ════════════════════════════════════════════════════════════════════════════
# 1. Time to first token (log y): the metric a voice/agent workload feels.
# ════════════════════════════════════════════════════════════════════════════
YMIN, YMAX = 10.0, 100000.0  # ms


def ly(v):
    v = max(v, YMIN)
    t = (math.log10(YMAX) - math.log10(v)) / (math.log10(YMAX) - math.log10(YMIN))
    return MT + t * PH


svg = []
head(svg, "Streaming: time to first token vs concurrent streams",
     "Both gateways translate the same Anthropic SSE stream to OpenAI chunks (equal work), one worker each.",
     f"50 tok/s per stream &#183; C++ provider &#183; median of {REPS} runs &#183; log y-axis &#183; lower is better.")

for v in (10, 100, 1000, 10000, 100000):
    y = ly(v)
    svg.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{ML + PW}" y2="{y:.1f}" stroke="{GRID}" stroke-width="1"/>')
    lbl = f"{v/1000:g} s" if v >= 1000 else f"{v:g} ms"
    svg.append(f'<text x="{ML - 10}" y="{y + 5:.1f}" font-size="{F_AXIS}" fill="{SUB}" text-anchor="end">{lbl}</text>')

for i, s in enumerate(LEVELS):
    x0, bw = xband(i)
    kb = med("llmbridge", s, "ttft_p50_ms")
    ll = med("litellm", s, "ttft_p50_ms")
    fl = med("direct", s, "ttft_p50_ms")
    barw = bw * 0.3
    for j, (val, colour) in enumerate(((kb, LLMBRIDGE), (ll, LITELLM))):
        bx = x0 + bw * 0.18 + j * (barw + 8)
        # A TTFT of 0 does NOT mean "instant": the generator records no TTFT sample when
        # a path completes too few streams inside the window, and it writes 0. Plotting
        # that as a bar renders the WORST result as the BEST one (LiteLLM at 256 streams
        # completed 64 of 4096 and would have shown "0 ms"). Draw "n/a" instead.
        if val <= 0:
            svg.append(f'<text x="{bx + barw/2:.1f}" y="{MT + PH - 12:.1f}" font-size="{F_VAL}" '
                       f'font-weight="600" fill="{colour}" text-anchor="middle" '
                       f'opacity="0.75">n/a</text>')
            svg.append(f'<text x="{bx + barw/2:.1f}" y="{MT + PH - 30:.1f}" font-size="{F_ANNOT}" '
                       f'fill="{SUB}" text-anchor="middle">too few</text>')
            svg.append(f'<text x="{bx + barw/2:.1f}" y="{MT + PH - 18:.1f}" font-size="{F_ANNOT}" '
                       f'fill="{SUB}" text-anchor="middle">completed</text>')
            continue
        y = ly(val)
        svg.append(f'<rect x="{bx:.1f}" y="{y:.1f}" width="{barw:.1f}" height="{MT + PH - y:.1f}" fill="{colour}" rx="3"/>')
        txt = f"{val/1000:.1f} s" if val >= 1000 else f"{val:.0f} ms"
        svg.append(f'<text x="{bx + barw/2:.1f}" y="{y - 8:.1f}" font-size="{F_VAL}" font-weight="600" '
                   f'fill="{colour}" text-anchor="middle">{txt}</text>')
    # the achievable floor (no gateway at all) at this level
    yf = ly(fl)
    svg.append(f'<line x1="{x0 + 6:.1f}" y1="{yf:.1f}" x2="{x0 + bw - 6:.1f}" y2="{yf:.1f}" '
               f'stroke="{FLOOR}" stroke-width="1.6" stroke-dasharray="6 4"/>')
    svg.append(f'<text x="{x0 + bw/2:.1f}" y="{MT + PH + 30:.1f}" font-size="{F_XLBL}" fill="{INK}" '
               f'text-anchor="middle">{s}</text>')

svg.append(f'<text x="{ML + PW/2:.1f}" y="{MT + PH + 62:.1f}" font-size="{F_SUB}" fill="{SUB}" '
           f'text-anchor="middle">concurrent streams</text>')
legend(svg, [(LLMBRIDGE, "llmbridge"), (LITELLM, "LiteLLM"), (FLOOR, "no-gateway floor")])
svg.append("</svg>")
for out in chart_outputs("stream-comparison.svg"):
    with open(out, "w") as f:
        f.write("\n".join(svg))
    print("wrote", out)

# ════════════════════════════════════════════════════════════════════════════
# 2. Delivery: chunks received as % of what the provider could actually supply.
#    This is the capacity story, and it is loss and not latency, so a linear axis
#    is the honest one.
# ════════════════════════════════════════════════════════════════════════════
svg = []
head(svg, "Streaming: tokens delivered vs concurrent streams",
     "Share of the achievable token stream each gateway actually delivers (100% = the no-gateway floor).",
     f"Same runs as the latency figure &#183; median of {REPS} &#183; higher is better.")

for pct in (0, 25, 50, 75, 100):
    y = MT + PH - (pct / 100.0) * PH
    svg.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{ML + PW}" y2="{y:.1f}" stroke="{GRID}" stroke-width="1"/>')
    svg.append(f'<text x="{ML - 10}" y="{y + 5:.1f}" font-size="{F_AXIS}" fill="{SUB}" text-anchor="end">{pct}%</text>')

for i, s in enumerate(LEVELS):
    x0, bw = xband(i)
    floor = med("direct", s, "chunks")
    kb = 100.0 * med("llmbridge", s, "chunks") / floor
    ll = 100.0 * med("litellm", s, "chunks") / floor
    barw = bw * 0.3
    for j, (val, colour) in enumerate(((kb, LLMBRIDGE), (ll, LITELLM))):
        bx = x0 + bw * 0.18 + j * (barw + 8)
        y = MT + PH - (min(val, 100.0) / 100.0) * PH
        svg.append(f'<rect x="{bx:.1f}" y="{y:.1f}" width="{barw:.1f}" height="{MT + PH - y:.1f}" fill="{colour}" rx="3"/>')
        # clamp the value label for full-height bars: at 100% the bar top is MT,
        # and y-8 would push the label into the subtitle block above the plot
        lbl_y = max(y - 8, MT - 4)
        # NEVER let a sub-100 value render as "100%". `.0f` rounded 99.93 up, which
        # put the exact claim we removed from the prose back into the chart. Values
        # in the last percent get two decimals so 99.93% reads as 99.93%.
        lbl = "100%" if val >= 99.995 else (f"{val:.2f}%" if val >= 99 else f"{val:.0f}%")
        svg.append(f'<text x="{bx + barw/2:.1f}" y="{lbl_y:.1f}" font-size="{F_VAL}" font-weight="600" '
                   f'fill="{colour}" text-anchor="middle">{lbl}</text>')
    svg.append(f'<text x="{x0 + bw/2:.1f}" y="{MT + PH + 30:.1f}" font-size="{F_XLBL}" fill="{INK}" '
               f'text-anchor="middle">{s}</text>')

svg.append(f'<text x="{ML + PW/2:.1f}" y="{MT + PH + 62:.1f}" font-size="{F_SUB}" fill="{SUB}" '
           f'text-anchor="middle">concurrent streams</text>')
legend(svg, [(LLMBRIDGE, "llmbridge"), (LITELLM, "LiteLLM")])
svg.append("</svg>")
for out in chart_outputs("stream-saturation.svg"):
    with open(out, "w") as f:
        f.write("\n".join(svg))
    print("wrote", out)
