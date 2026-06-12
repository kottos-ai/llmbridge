#!/usr/bin/env python3

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Render the throughput-saturation chart (offered vs achieved RPS) as SVG.

Log-log axes so both ceilings fit on one plot: llmbridge tracks the ideal y=x line
up to ~58k RPS then flattens (single-thread bottleneck); LiteLLM flattens at
~250 RPS/worker. The gap between each curve and the diagonal is dropped load.

  python3 bench/make_saturation_chart.py   # -> bench/results/saturation.svg
"""
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "results", "saturation.svg")

# (offered, achieved) at a fixed 1 KB request size, BOTH systems single-worker.
# llmbridge: io_uring, 1 worker, perf mode, i7-9750H (all processes co-located on
# one box, so the absolute ceiling is a dev-box upper bound). LiteLLM: 1 uvicorn
# worker. achieved capped at offered; the plateau is the saturation ceiling.
LLMBRIDGE = [(20000, 20000), (40000, 40000), (60000, 60000), (80000, 80000),
             (100000, 87501), (120000, 91313)]
LITELLM = [(100, 100), (500, 246), (1000, 248), (5000, 228)]

W, H = 900, 600
ML, MR, MT, MB = 90, 40, 80, 70
PW, PH = W - ML - MR, H - MT - MB
LO, HI = 100.0, 200000.0


def lx(v): return ML + (math.log10(v) - math.log10(LO)) / (math.log10(HI) - math.log10(LO)) * PW


def ly(v): return MT + (math.log10(HI) - math.log10(max(v, LO))) / (math.log10(HI) - math.log10(LO)) * PH


INK, SUB, GRID = "#111827", "#6b7280", "#e5e7eb"
CG, CR, CD = "#16a34a", "#dc2626", "#9ca3af"
C64, C1K, C8K = "#16a34a", "#2563eb", "#9333ea"  # 64B / 1KB / 8KB request bodies

s = []
s.append(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">')
s.append(f'<rect width="{W}" height="{H}" fill="white"/>')
s.append(
    f'<text x="{ML}" y="34" font-size="22" font-weight="700" fill="{INK}">Throughput saturation — offered vs achieved RPS (1 KB requests)</text>')
s.append(
    f'<text x="{ML}" y="56" font-size="13" fill="{SUB}">Both single worker (1 thread), 1 KB requests, instant backend, log-log. On the dashed line = keeping up; below it = dropping load.</text>')

# grid + ticks at decades and helpful values
ticks = [100, 1000, 10000, 100000]
for t in ticks:
    x = lx(t);
    y = ly(t)
    s.append(f'<line x1="{x:.1f}" y1="{MT}" x2="{x:.1f}" y2="{MT + PH}" stroke="{GRID}"/>')
    s.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{ML + PW}" y2="{y:.1f}" stroke="{GRID}"/>')
    lbl = f"{t:,}"
    s.append(f'<text x="{x:.1f}" y="{MT + PH + 20:.1f}" font-size="11" fill="{SUB}" text-anchor="middle">{lbl}</text>')
    s.append(f'<text x="{ML - 8}" y="{y + 4:.1f}" font-size="11" fill="{SUB}" text-anchor="end">{lbl}</text>')

s.append(
    f'<text x="{ML + PW / 2:.1f}" y="{H - 22}" font-size="13" fill="{INK}" text-anchor="middle" font-weight="600">offered RPS</text>')
s.append(
    f'<text x="22" y="{MT + PH / 2:.1f}" font-size="13" fill="{INK}" text-anchor="middle" transform="rotate(-90 22 {MT + PH / 2:.1f})" font-weight="600">achieved RPS</text>')

# ideal y=x
s.append(
    f'<line x1="{lx(LO):.1f}" y1="{ly(LO):.1f}" x2="{lx(HI):.1f}" y2="{ly(HI):.1f}" stroke="{CD}" stroke-width="1.5" stroke-dasharray="6 4"/>')
s.append(
    f'<text x="{lx(HI) - 6:.1f}" y="{ly(HI) + 16:.1f}" font-size="11" fill="{CD}" text-anchor="end">ideal (achieved = offered)</text>')


def poly(pts, color):
    d = " ".join(f"{lx(o):.1f},{ly(a):.1f}" for o, a in pts)
    s.append(f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="2.5"/>')
    for o, a in pts:
        s.append(f'<circle cx="{lx(o):.1f}" cy="{ly(a):.1f}" r="3.5" fill="{color}"/>')


poly(LLMBRIDGE, CG)
poly(LITELLM, CR)

# ceiling annotations
s.append(
    f'<text x="{lx(120000):.1f}" y="{ly(91000) - 10:.1f}" font-size="12" fill="{CG}" text-anchor="end" font-weight="700">llmbridge ceiling ≈ 90,000 RPS</text>')
s.append(
    f'<text x="{lx(5000):.1f}" y="{ly(250) - 10:.1f}" font-size="12" fill="{CR}" text-anchor="middle" font-weight="700">LiteLLM ceiling ≈ 250 RPS</text>')

# legend
lgy = MT + 12
s.append(f'<rect x="{ML + PW - 270}" y="{lgy - 12}" width="260" height="52" fill="white" stroke="{GRID}" rx="4"/>')
rows = [(CG, "llmbridge (C++/io_uring, 1 worker)"), (CR, "LiteLLM (Python, 1 worker)")]
for i, (col, label) in enumerate(rows):
    yy = lgy + 2 + i * 22
    s.append(f'<line x1="{ML + PW - 258}" y1="{yy}" x2="{ML + PW - 234}" y2="{yy}" stroke="{col}" stroke-width="2.5"/>')
    s.append(f'<text x="{ML + PW - 228}" y="{yy + 4}" font-size="12" fill="{INK}">{label}</text>')

s.append('</svg>')
with open(OUT, "w") as f:
    f.write("\n".join(s))
print(f"wrote {OUT}")
