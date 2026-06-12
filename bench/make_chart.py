#!/usr/bin/env python3

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Render the llmbridge-vs-LiteLLM added-latency chart as a standalone SVG.

Pure stdlib — emits SVG text directly (no matplotlib), so it runs anywhere and
produces a crisp vector artifact for the pitch deck / README. Log y-axis,
because the two systems differ by ~6 orders of magnitude and a linear axis
would render llmbridge as a flat zero.

  python3 bench/make_chart.py            # -> bench/results/comparison.svg
"""
import csv
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "results", "phase-a-comparison.csv")
OUT = os.path.join(HERE, "results", "comparison.svg")

rows = []
with open(CSV) as f:
    for line in f:
        if line.startswith("#") or line.startswith("rps,"):
            continue
        if not line.strip():
            continue
        c = line.strip().split(",")
        rows.append({
            "rps": int(c[0]),
            "llmbridge_p99": float(c[2]),
            "litellm_p99": float(c[7]),
            "litellm_sat": c[9].strip() == "yes",
        })

# Geometry
W, H = 920, 560
ML, MR, MT, MB = 80, 30, 90, 90
PW = W - ML - MR
PH = H - MT - MB
YMIN, YMAX = 0.01, 100000.0  # ms, log scale


def ly(v):
    v = max(v, YMIN)
    t = (math.log10(YMAX) - math.log10(v)) / (math.log10(YMAX) - math.log10(YMIN))
    return MT + t * PH


LLMBRIDGE = "#16a34a"  # green
LITELLM = "#dc2626"  # red
GRID = "#e5e7eb"
INK = "#111827"
SUB = "#6b7280"

svg = []
svg.append(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">')
svg.append(f'<rect width="{W}" height="{H}" fill="white"/>')
svg.append(
    f'<text x="{ML}" y="34" font-size="22" font-weight="700" fill="{INK}">llmbridge vs LiteLLM — gateway added latency (p99), 1 KB requests</text>')
svg.append(
    f'<text x="{ML}" y="56" font-size="13" fill="{SUB}">Both gateways translate OpenAI&#8596;Anthropic (equal work), one worker each, 1 KB requests. 200 ms mock backend, open-loop load. Single co-located host. Lower is better; log scale.</text>')

# y grid + labels (decades)
dec = -2
while dec <= 5:
    v = 10.0 ** dec
    y = ly(v)
    svg.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{ML + PW}" y2="{y:.1f}" stroke="{GRID}" stroke-width="1"/>')
    lbl = (f"{v:g} ms" if v >= 1 else f"{v:g} ms")
    svg.append(f'<text x="{ML - 8}" y="{y + 4:.1f}" font-size="11" fill="{SUB}" text-anchor="end">{lbl}</text>')
    dec += 1

# 1 ms PASS line
y1 = ly(1.0)
svg.append(
    f'<line x1="{ML}" y1="{y1:.1f}" x2="{ML + PW}" y2="{y1:.1f}" stroke="#2563eb" stroke-width="1.5" stroke-dasharray="6 4"/>')
svg.append(
    f'<text x="{ML + PW}" y="{y1 - 6:.1f}" font-size="11" fill="#2563eb" text-anchor="end" font-weight="600">Phase A pass bar: p99 &lt; 1 ms</text>')

# bars
n = len(rows)
group_w = PW / n
bw = group_w * 0.30
axis_y = ly(YMIN)
for i, r in enumerate(rows):
    gx = ML + i * group_w + group_w / 2
    # llmbridge bar
    cx = gx - bw - 4
    cy = ly(r["llmbridge_p99"])
    svg.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{bw:.1f}" height="{axis_y - cy:.1f}" fill="{LLMBRIDGE}" rx="2"/>')
    svg.append(
        f'<text x="{cx + bw / 2:.1f}" y="{cy - 6:.1f}" font-size="11" fill="{LLMBRIDGE}" text-anchor="middle" font-weight="700">{r["llmbridge_p99"]:.3f}</text>')
    # LiteLLM bar
    lx = gx + 4
    lyv = ly(r["litellm_p99"])
    svg.append(
        f'<rect x="{lx:.1f}" y="{lyv:.1f}" width="{bw:.1f}" height="{axis_y - lyv:.1f}" fill="{LITELLM}" rx="2"/>')
    val = f'{r["litellm_p99"]:.1f}' if r["litellm_p99"] >= 10 else f'{r["litellm_p99"]:.2f}'
    warn = " ⚠" if r["litellm_sat"] else ""
    svg.append(
        f'<text x="{lx + bw / 2:.1f}" y="{lyv - 6:.1f}" font-size="11" fill="{LITELLM}" text-anchor="middle" font-weight="700">{val}{warn}</text>')
    # x label
    svg.append(
        f'<text x="{gx:.1f}" y="{axis_y + 20:.1f}" font-size="13" fill="{INK}" text-anchor="middle" font-weight="600">{r["rps"]} RPS</text>')
    if r["litellm_sat"]:
        svg.append(
            f'<text x="{gx:.1f}" y="{axis_y + 36:.1f}" font-size="10" fill="{LITELLM}" text-anchor="middle">LiteLLM saturated (~250 RPS cap)</text>')

# x axis line
svg.append(f'<line x1="{ML}" y1="{axis_y:.1f}" x2="{ML + PW}" y2="{axis_y:.1f}" stroke="{INK}" stroke-width="1.5"/>')

# legend
lgx, lgy = ML, H - 28
svg.append(f'<rect x="{lgx}" y="{lgy - 12}" width="14" height="14" fill="{LLMBRIDGE}" rx="2"/>')
svg.append(
    f'<text x="{lgx + 20}" y="{lgy}" font-size="12" fill="{INK}">llmbridge (C++/io_uring, 1 worker) — proxy self-measured added p99</text>')
svg.append(f'<rect x="{lgx + 430}" y="{lgy - 12}" width="14" height="14" fill="{LITELLM}" rx="2"/>')
svg.append(
    f'<text x="{lgx + 450}" y="{lgy}" font-size="12" fill="{INK}">LiteLLM (Python, 1 worker) — client-measured added p99</text>')

svg.append('</svg>')

with open(OUT, "w") as f:
    f.write("\n".join(svg))
print(f"wrote {OUT}")
