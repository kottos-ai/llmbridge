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
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "results", "phase-a-comparison.csv")


def chart_outputs(name):
    """Where to write a generated chart. Always bench/results/ (the public OSS
    README embeds these). Also private/website/assets/ when that site tree is
    present, so the README copy and the deployed website never drift — one run,
    no manual copying. Public OSS clones (no private/) just get bench/results/."""
    outs = [os.path.join(HERE, "results", name)]
    site = os.path.join(os.path.dirname(HERE), "private", "website")
    if os.path.isdir(site):
        assets = os.path.join(site, "assets")
        os.makedirs(assets, exist_ok=True)
        outs.append(os.path.join(assets, name))
    return outs


OUTS = chart_outputs("comparison.svg")

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

# Geometry — W/H kept IDENTICAL to make_saturation_chart.py so the two figures
# render at exactly the same size on the page.
W, H = 1000, 620
ML, MR, MT, MB = 96, 44, 116, 116
PW = W - ML - MR
PH = H - MT - MB
YMIN, YMAX = 0.01, 100000.0  # ms, log scale

# Font sizes (bumped for legibility when the SVG is scaled to a page column).
F_TITLE, F_SUB, F_AXIS, F_VAL, F_XLBL, F_LEG, F_ANNOT = 27, 15, 15, 16, 18, 15, 13


def ly(v):
    v = max(v, YMIN)
    t = (math.log10(YMAX) - math.log10(v)) / (math.log10(YMAX) - math.log10(YMIN))
    return MT + t * PH


LLMBRIDGE = "#16a34a"  # green: semantic pass/fail against red, NOT brand colour
LITELLM = "#dc2626"  # red
GRID = "#e5e7eb"
INK = "#111827"
SUB = "#6b7280"

svg = []
svg.append(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">')
svg.append(f'<rect width="{W}" height="{H}" fill="white"/>')
svg.append(
    f'<text x="{ML}" y="44" font-size="{F_TITLE}" font-weight="700" fill="{INK}">llmbridge vs LiteLLM — added latency p99 (64 B body)</text>')
svg.append(
    f'<text x="{ML}" y="74" font-size="{F_SUB}" fill="{SUB}">Both gateways do the full OpenAI&#8596;Anthropic translation (equal work), one worker each.</text>')
svg.append(
    f'<text x="{ML}" y="95" font-size="{F_SUB}" fill="{SUB}">200 ms mock backend &#183; open-loop load &#183; single co-located host &#183; log y-axis &#183; lower is better.</text>')

# y grid + labels (decades)
dec = -2
while dec <= 5:
    v = 10.0 ** dec
    y = ly(v)
    svg.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{ML + PW}" y2="{y:.1f}" stroke="{GRID}" stroke-width="1"/>')
    svg.append(f'<text x="{ML - 10}" y="{y + 5:.1f}" font-size="{F_AXIS}" fill="{SUB}" text-anchor="end">{v:g} ms</text>')
    dec += 1

# 1 ms PASS line
y1 = ly(1.0)
svg.append(
    f'<line x1="{ML}" y1="{y1:.1f}" x2="{ML + PW}" y2="{y1:.1f}" stroke="#2563eb" stroke-width="1.5" stroke-dasharray="6 4"/>')
svg.append(
    f'<text x="{ML + 4}" y="{y1 - 8:.1f}" font-size="{F_ANNOT}" fill="#2563eb" text-anchor="start" font-weight="600">1 ms target</text>')

# bars
n = len(rows)
group_w = PW / n
bw = group_w * 0.30
axis_y = ly(YMIN)
for i, r in enumerate(rows):
    gx = ML + i * group_w + group_w / 2
    # llmbridge bar
    cx = gx - bw - 5
    cy = ly(r["llmbridge_p99"])
    svg.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{bw:.1f}" height="{axis_y - cy:.1f}" fill="{LLMBRIDGE}" rx="2"/>')
    svg.append(
        f'<text x="{cx + bw / 2:.1f}" y="{cy - 7:.1f}" font-size="{F_VAL}" fill="{LLMBRIDGE}" text-anchor="middle" font-weight="700">{r["llmbridge_p99"]:.3f}</text>')
    # LiteLLM bar
    lx = gx + 5
    lyv = ly(r["litellm_p99"])
    svg.append(
        f'<rect x="{lx:.1f}" y="{lyv:.1f}" width="{bw:.1f}" height="{axis_y - lyv:.1f}" fill="{LITELLM}" rx="2"/>')
    val = f'{r["litellm_p99"]:.1f}' if r["litellm_p99"] >= 10 else f'{r["litellm_p99"]:.2f}'
    warn = " &#9888;" if r["litellm_sat"] else ""
    svg.append(
        f'<text x="{lx + bw / 2:.1f}" y="{lyv - 7:.1f}" font-size="{F_VAL}" fill="{LITELLM}" text-anchor="middle" font-weight="700">{val}{warn}</text>')
    # x label
    svg.append(
        f'<text x="{gx:.1f}" y="{axis_y + 26:.1f}" font-size="{F_XLBL}" fill="{INK}" text-anchor="middle" font-weight="600">{r["rps"]} RPS</text>')

# ONE spanning "saturated" annotation, not one per group: the 12px label is wider
# than the group spacing, so per-group copies overlapped into an unreadable smear.
# A thin rule marks the saturated span; the label sits between the x labels and
# the legend (axis_y+47 clears the x-label descenders and the legend's top).
sat = [i for i, r in enumerate(rows) if r["litellm_sat"]]
if sat:
    x0 = ML + sat[0] * group_w + group_w / 2 - 55
    x1 = ML + sat[-1] * group_w + group_w / 2 + 55
    svg.append(
        f'<line x1="{x0:.1f}" y1="{axis_y + 34:.1f}" x2="{x1:.1f}" y2="{axis_y + 34:.1f}" stroke="{LITELLM}" stroke-width="1" opacity=".55"/>')
    svg.append(
        f'<text x="{(x0 + x1) / 2:.1f}" y="{axis_y + 47:.1f}" font-size="12" fill="{LITELLM}" text-anchor="middle">LiteLLM saturated across this range (~250 RPS cap)</text>')

# x axis line
svg.append(f'<line x1="{ML}" y1="{axis_y:.1f}" x2="{ML + PW}" y2="{axis_y:.1f}" stroke="{INK}" stroke-width="1.5"/>')

# legend — two stacked rows (bottom-left), so the longer labels never overflow
sw = 16
for i, (col, label) in enumerate([
    (LLMBRIDGE, "llmbridge (C++/io_uring, 1 worker) — proxy self-measured added p99"),
    (LITELLM, "LiteLLM (Python, 1 worker) — client-measured added p99"),
]):
    yy = H - 50 + i * 24
    svg.append(f'<rect x="{ML}" y="{yy - sw + 3}" width="{sw}" height="{sw}" fill="{col}" rx="2"/>')
    svg.append(f'<text x="{ML + sw + 8}" y="{yy}" font-size="{F_LEG}" fill="{INK}">{label}</text>')

svg.append('</svg>')

for out in OUTS:
    with open(out, "w") as f:
        f.write("\n".join(svg))
    print(f"wrote {out}")
