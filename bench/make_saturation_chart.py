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
LLMBRIDGE = [(1000, 1000), (5000, 5000), (10000, 10000), (20000, 20000),
          (30000, 30001), (40000, 40000), (50000, 50000), (60000, 57692),
          (80000, 60476), (100000, 58716)]
LITELLM = [(100, 100), (500, 246), (1000, 248), (5000, 228)]

W, H = 900, 600
ML, MR, MT, MB = 90, 40, 80, 70
PW, PH = W - ML - MR, H - MT - MB
LO, HI = 100.0, 200000.0


def lx(v): return ML + (math.log10(v) - math.log10(LO)) / (math.log10(HI) - math.log10(LO)) * PW


def ly(v): return MT + (math.log10(HI) - math.log10(max(v, LO))) / (math.log10(HI) - math.log10(LO)) * PH


INK, SUB, GRID = "#111827", "#6b7280", "#e5e7eb"
CG, CR, CD = "#16a34a", "#dc2626", "#9ca3af"

s = []
s.append(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">')
s.append(f'<rect width="{W}" height="{H}" fill="white"/>')
s.append(
    f'<text x="{ML}" y="34" font-size="22" font-weight="700" fill="{INK}">Throughput saturation — offered vs achieved RPS</text>')
s.append(
    f'<text x="{ML}" y="56" font-size="13" fill="{SUB}">One thread / one worker, instant backend, log-log. On the dashed line = keeping up; below it = dropping load.</text>')

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
    f'<text x="{lx(100000):.1f}" y="{ly(58000) - 10:.1f}" font-size="12" fill="{CG}" text-anchor="end" font-weight="700">llmbridge ceiling ≈ 58,000 RPS</text>')
s.append(
    f'<text x="{lx(5000):.1f}" y="{ly(250) - 10:.1f}" font-size="12" fill="{CR}" text-anchor="middle" font-weight="700">LiteLLM ceiling ≈ 250 RPS</text>')

# legend
lgy = MT + 12
s.append(f'<rect x="{ML + PW - 250}" y="{lgy - 12}" width="240" height="56" fill="white" stroke="{GRID}" rx="4"/>')
s.append(
    f'<line x1="{ML + PW - 238}" y1="{lgy + 2}" x2="{ML + PW - 214}" y2="{lgy + 2}" stroke="{CG}" stroke-width="2.5"/>')
s.append(f'<text x="{ML + PW - 208}" y="{lgy + 6}" font-size="12" fill="{INK}">llmbridge (C++/epoll, 1 thread)</text>')
s.append(
    f'<line x1="{ML + PW - 238}" y1="{lgy + 24}" x2="{ML + PW - 214}" y2="{lgy + 24}" stroke="{CR}" stroke-width="2.5"/>')
s.append(f'<text x="{ML + PW - 208}" y="{lgy + 28}" font-size="12" fill="{INK}">LiteLLM (Python, 1 worker)</text>')

s.append('</svg>')
with open(OUT, "w") as f:
    f.write("\n".join(s))
print(f"wrote {OUT}")
