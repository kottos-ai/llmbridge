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

# (offered, achieved). llmbridge from the Linux phase-a + saturation sweeps
# (i7-9750H, all processes co-located); LiteLLM from the head-to-head run (1
# uvicorn worker).
# offered -> achieved RPS, single thread, instant C++ backend, passthrough (no
# translate). Body = request size; three curves show how the per-request byte
# cost lowers the ceiling. Optimistic-upstream-write binary, perf mode, i7-9750H.
# (achieved capped at offered; plateaus are the saturation ceilings.)
LLMBRIDGE_64B = [(10000, 10000), (20000, 20000), (30000, 30000), (50000, 50000),
                 (70000, 70000), (90000, 70010), (120000, 69795)]
LLMBRIDGE_1KB = [(10000, 10000), (20000, 20000), (30000, 30000), (50000, 50000),
                 (70000, 69054), (90000, 68338), (120000, 68142)]
LLMBRIDGE_8KB = [(10000, 10000), (20000, 20000), (30000, 30000), (50000, 50000),
                 (70000, 54584), (90000, 56121), (120000, 55759)]
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
    f'<text x="{ML}" y="34" font-size="22" font-weight="700" fill="{INK}">Throughput saturation — offered vs achieved RPS</text>')
s.append(
    f'<text x="{ML}" y="56" font-size="13" fill="{SUB}">One thread / one worker, instant backend, log-log. llmbridge swept at 64B / 1KB / 8KB request bodies. On the dashed line = keeping up; below it = dropping load.</text>')

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


poly(LLMBRIDGE_8KB, C8K)
poly(LLMBRIDGE_1KB, C1K)
poly(LLMBRIDGE_64B, C64)
poly(LITELLM, CR)

# ceiling annotations — one per body-size plateau, plus LiteLLM
s.append(
    f'<text x="{lx(120000):.1f}" y="{ly(70000) - 9:.1f}" font-size="11" fill="{C64}" text-anchor="end" font-weight="700">64B ≈ 70,000 RPS</text>')
s.append(
    f'<text x="{lx(120000):.1f}" y="{ly(68000) + 18:.1f}" font-size="11" fill="{C1K}" text-anchor="end" font-weight="700">1KB ≈ 68,000</text>')
s.append(
    f'<text x="{lx(120000):.1f}" y="{ly(55000) + 16:.1f}" font-size="11" fill="{C8K}" text-anchor="end" font-weight="700">8KB ≈ 55,000</text>')
s.append(
    f'<text x="{lx(5000):.1f}" y="{ly(250) - 10:.1f}" font-size="12" fill="{CR}" text-anchor="middle" font-weight="700">LiteLLM ceiling ≈ 250 RPS</text>')

# legend
lgy = MT + 12
s.append(f'<rect x="{ML + PW - 260}" y="{lgy - 12}" width="250" height="92" fill="white" stroke="{GRID}" rx="4"/>')
rows = [(C64, "llmbridge — 64B body"), (C1K, "llmbridge — 1KB body"),
        (C8K, "llmbridge — 8KB body"), (CR, "LiteLLM (Python, 1 worker)")]
for i, (col, label) in enumerate(rows):
    yy = lgy + 2 + i * 22
    s.append(f'<line x1="{ML + PW - 248}" y1="{yy}" x2="{ML + PW - 224}" y2="{yy}" stroke="{col}" stroke-width="2.5"/>')
    s.append(f'<text x="{ML + PW - 218}" y="{yy + 4}" font-size="12" fill="{INK}">{label}</text>')

s.append('</svg>')
with open(OUT, "w") as f:
    f.write("\n".join(s))
print(f"wrote {OUT}")
