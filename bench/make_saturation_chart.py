#!/usr/bin/env python3

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Render the throughput-saturation chart (offered vs achieved RPS) as SVG.

Log-log axes so both ceilings fit on one plot: llmbridge tracks the ideal y=x line
up to ~90k RPS then flattens (single-thread / loopback bottleneck); LiteLLM
flattens at ~250 RPS/worker. The gap between each curve and the diagonal is
dropped load.

  python3 bench/make_saturation_chart.py   # -> bench/results/saturation.svg
"""
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def chart_outputs(name):
    """Always bench/results/ (the public OSS README embeds these); also
    private/website/assets/ when that site tree is present, so the README copy
    and the deployed website never drift. Public OSS clones just get results/."""
    outs = [os.path.join(HERE, "results", name)]
    site = os.path.join(os.path.dirname(HERE), "private", "website")
    if os.path.isdir(site):
        assets = os.path.join(site, "assets")
        os.makedirs(assets, exist_ok=True)
        outs.append(os.path.join(assets, name))
    return outs


OUTS = chart_outputs("saturation.svg")

# (offered, achieved) at a fixed 1 KB request size, BOTH systems single-worker.
# llmbridge: io_uring, 1 worker, perf mode, i7-9750H (all processes co-located on
# one box, so the absolute ceiling is a dev-box upper bound). LiteLLM: 1 uvicorn
# worker. achieved capped at offered; the plateau is the saturation ceiling.
LLMBRIDGE = [(20000, 20000), (40000, 40000), (60000, 60000), (80000, 80000),
             (100000, 87501), (120000, 91313)]
LITELLM = [(100, 100), (500, 246), (1000, 248), (5000, 228)]

# Geometry — W/H kept IDENTICAL to make_chart.py so the two figures render at
# exactly the same size on the page.
W, H = 1000, 620
ML, MR, MT, MB = 96, 56, 116, 100
PW, PH = W - ML - MR, H - MT - MB
LO, HI = 100.0, 200000.0

# Font sizes (matched to make_chart.py for a consistent, legible pair).
F_TITLE, F_SUB, F_AXIS, F_AXTITLE, F_ANNOT, F_LEG = 27, 15, 15, 16, 14, 15


def lx(v): return ML + (math.log10(v) - math.log10(LO)) / (math.log10(HI) - math.log10(LO)) * PW


def ly(v): return MT + (math.log10(HI) - math.log10(max(v, LO))) / (math.log10(HI) - math.log10(LO)) * PH


INK, SUB, GRID = "#111827", "#6b7280", "#e5e7eb"
CG, CR, CD = "#16a34a", "#dc2626", "#9ca3af"

s = []
s.append(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">')
s.append(f'<rect width="{W}" height="{H}" fill="white"/>')
s.append(
    f'<text x="{ML}" y="44" font-size="{F_TITLE}" font-weight="700" fill="{INK}">Throughput saturation — offered vs achieved RPS (1 KB)</text>')
s.append(
    f'<text x="{ML}" y="74" font-size="{F_SUB}" fill="{SUB}">Both single-worker, 1 KB requests, instant backend &#183; log&#8211;log axes.</text>')
s.append(
    f'<text x="{ML}" y="95" font-size="{F_SUB}" fill="{SUB}">On the dashed line = keeping up &#183; below it = dropping load.</text>')

# grid + ticks at decades
for t in [100, 1000, 10000, 100000]:
    x = lx(t)
    y = ly(t)
    s.append(f'<line x1="{x:.1f}" y1="{MT}" x2="{x:.1f}" y2="{MT + PH}" stroke="{GRID}"/>')
    s.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{ML + PW}" y2="{y:.1f}" stroke="{GRID}"/>')
    s.append(f'<text x="{x:.1f}" y="{MT + PH + 26:.1f}" font-size="{F_AXIS}" fill="{SUB}" text-anchor="middle">{t:,}</text>')
    s.append(f'<text x="{ML - 10}" y="{y + 5:.1f}" font-size="{F_AXIS}" fill="{SUB}" text-anchor="end">{t:,}</text>')

# axis titles
s.append(
    f'<text x="{ML + PW / 2:.1f}" y="{H - 24}" font-size="{F_AXTITLE}" fill="{INK}" text-anchor="middle" font-weight="600">offered RPS</text>')
s.append(
    f'<text x="30" y="{MT + PH / 2:.1f}" font-size="{F_AXTITLE}" fill="{INK}" text-anchor="middle" transform="rotate(-90 30 {MT + PH / 2:.1f})" font-weight="600">achieved RPS</text>')

# ideal y=x line + a label placed mid-diagonal (clear of the ceiling notes)
s.append(
    f'<line x1="{lx(LO):.1f}" y1="{ly(LO):.1f}" x2="{lx(HI):.1f}" y2="{ly(HI):.1f}" stroke="{CD}" stroke-width="1.5" stroke-dasharray="6 4"/>')
s.append(
    f'<text x="{lx(2500) + 8:.1f}" y="{ly(2500) - 9:.1f}" font-size="{F_ANNOT}" fill="{CD}" text-anchor="start">ideal: achieved = offered</text>')


def poly(pts, color):
    d = " ".join(f"{lx(o):.1f},{ly(a):.1f}" for o, a in pts)
    s.append(f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="2.5"/>')
    for o, a in pts:
        s.append(f'<circle cx="{lx(o):.1f}" cy="{ly(a):.1f}" r="4" fill="{color}"/>')


poly(LLMBRIDGE, CG)
poly(LITELLM, CR)

# ceiling annotations
s.append(
    f'<text x="{lx(120000):.1f}" y="{ly(91000) - 12:.1f}" font-size="{F_ANNOT + 1}" fill="{CG}" text-anchor="end" font-weight="700">llmbridge ceiling &#8776; 90,000 RPS</text>')
s.append(
    f'<text x="{lx(5000):.1f}" y="{ly(250) - 12:.1f}" font-size="{F_ANNOT + 1}" fill="{CR}" text-anchor="middle" font-weight="700">LiteLLM ceiling &#8776; 250 RPS</text>')

# legend — bottom-right corner (empty region, below the diagonal)
bx, by, bw, bh = ML + PW - 300, MT + PH - 66, 290, 60
s.append(f'<rect x="{bx}" y="{by}" width="{bw}" height="{bh}" fill="white" stroke="{GRID}" rx="5"/>')
for i, (col, label) in enumerate([(CG, "llmbridge (C++/io_uring, 1 worker)"),
                                  (CR, "LiteLLM (Python, 1 worker)")]):
    yy = by + 22 + i * 24
    s.append(f'<line x1="{bx + 14}" y1="{yy}" x2="{bx + 40}" y2="{yy}" stroke="{col}" stroke-width="3"/>')
    s.append(f'<circle cx="{bx + 27}" cy="{yy}" r="4" fill="{col}"/>')
    s.append(f'<text x="{bx + 50}" y="{yy + 5}" font-size="{F_LEG}" fill="{INK}">{label}</text>')

s.append('</svg>')
for out in OUTS:
    with open(out, "w") as f:
        f.write("\n".join(s))
    print(f"wrote {out}")
