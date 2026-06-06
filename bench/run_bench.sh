#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# Phase A benchmark orchestrator.
#
# For each target RPS we measure latency twice with the same open-loop loadgen:
#   (1) through the llmbridge proxy        -> backend latency + proxy overhead
#   (2) direct to the mock provider     -> backend latency baseline
# The proxy's added latency is the delta of those two client-observed numbers,
# cross-checked against the proxy's own self-reported overhead (request-path +
# response-path, with the upstream wait excluded). PASS = p99 added < 1 ms at
# 1000 RPS.
#
# Everything runs on one box (mock + proxy + loadgen compete for the same 10
# cores), so treat absolute tails as a dev-box upper bound — a clean number
# wants separate hosts (Phase D). The delta methodology cancels most co-location
# noise because both legs see the same contention.
#
#   ./bench/run_bench.sh [latency_ms] [duration] [warmup] [rps_list...]
set -euo pipefail
cd "$(dirname "$0")/.."

LAT_MS="${1:-200}"
DUR="${2:-20}"
WARMUP="${3:-5}"
shift $(( $# < 3 ? $# : 3 )) || true
RPS_LIST=("${@:-100 500 1000 5000}")
# allow a single quoted "100 500 ..." arg or separate args
if [ "${#RPS_LIST[@]}" -eq 1 ]; then read -r -a RPS_LIST <<< "${RPS_LIST[0]}"; fi

MOCK_PORT=9001
PROXY_PORT=8088
RESDIR="bench/results"
mkdir -p "$RESDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
SUMMARY="$RESDIR/summary-$STAMP.txt"

# Locate CMake-built binaries (CLion uses cmake-build-*/bin). Override: BIN=path
BIN="${BIN:-}"
if [ -z "$BIN" ]; then
  for d in cmake-build-release/bin cmake-build-debug/bin build-linux/bin build/bin; do
    if [ -x "$d/llmbridge" ]; then BIN="$d"; break; fi
  done
fi
[ -z "$BIN" ] && { echo "No built binaries; build the CMake project first or set BIN=path" >&2; exit 1; }

cleanup() { pkill -f mock_provider.py 2>/dev/null || true; pkill -f "$BIN/llmbridge" 2>/dev/null || true; }
trap cleanup EXIT
cleanup; sleep 0.3

echo "Using binaries in $BIN"

echo "Starting mock provider (latency=${LAT_MS}ms) ..."
python3 bench/mock_provider.py --port "$MOCK_PORT" --latency-ms "$LAT_MS" >"$RESDIR/mock-$STAMP.log" 2>&1 &
sleep 1

{
  echo "llmbridge Phase A benchmark — $STAMP"
  echo "mock latency=${LAT_MS}ms  duration=${DUR}s  warmup=${WARMUP}s  host=$(uname -mn)"
  echo
  printf "%-7s | %-32s | %-32s | %s\n" "RPS" "DIRECT-to-mock (client e2e, ms)" "THROUGH-proxy (client e2e, ms)" "PROXY ADDED (ms)"
  printf "%-7s | %-32s | %-32s | %s\n" "" "p50 / p99 / p99.9 / max" "p50 / p99 / p99.9 / max" "delta-p50 / delta-p99 | self-p99"
  echo "--------+----------------------------------+----------------------------------+-----------------------------------"
} | tee "$SUMMARY"

# parse "p50_us=NNN p99_us=NNN p999_us=NNN max_us=NNN" -> echo "p50 p99 p999 max" in ms (3 decimals)
extract_us() { sed -n 's/.*p50_us=\([0-9]*\) p99_us=\([0-9]*\) p999_us=\([0-9]*\) max_us=\([0-9]*\).*/\1 \2 \3 \4/p'; }
ms() { awk -v u="$1" 'BEGIN{printf "%.3f", u/1000.0}'; }

for RPS in "${RPS_LIST[@]}"; do
  echo ">>> RPS=$RPS" >&2

  # (2) direct-to-mock baseline
  DIRECT=$($BIN/loadgen --target 127.0.0.1:$MOCK_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" 2>>"$RESDIR/loadgen-direct-$RPS-$STAMP.log")
  read -r d50 d99 d999 dmax <<< "$(echo "$DIRECT" | extract_us)"

  # (1) through-proxy: fresh proxy per level so its self-stats are per-RPS
  $BIN/llmbridge --listen $PROXY_PORT --upstream 127.0.0.1:$MOCK_PORT \
      --duration $((DUR + WARMUP + 3)) --warmup "$WARMUP" >"$RESDIR/proxy-$RPS-$STAMP.log" 2>&1 &
  PROXY_PID=$!
  sleep 1
  PROXIED=$($BIN/loadgen --target 127.0.0.1:$PROXY_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" 2>>"$RESDIR/loadgen-proxy-$RPS-$STAMP.log")
  read -r p50 p99 p999 pmax <<< "$(echo "$PROXIED" | extract_us)"
  kill -TERM "$PROXY_PID" 2>/dev/null || true; wait "$PROXY_PID" 2>/dev/null || true

  # proxy self-reported added-total p99 (µs) from its dumped histogram. The
  # histogram prints "us"/"µs" for microseconds and "ns" for nanoseconds; handle
  # all three (us/µs values are already in µs; ns gets divided down).
  self_p99=$(sed -n 's/.*added-total.*p99=\([0-9.]*\) [µu]s.*/\1/p' "$RESDIR/proxy-$RPS-$STAMP.log" | head -1)
  [ -z "$self_p99" ] && self_p99=$(sed -n 's/.*added-total.*p99=\([0-9.]*\) ns.*/\1/p' "$RESDIR/proxy-$RPS-$STAMP.log" | head -1 | awk '{printf "%.3f", $1/1000}')

  # added latency delta (ms)
  add50=$(awk -v a="$p50" -v b="$d50" 'BEGIN{printf "%.3f", (a-b)/1000.0}')
  add99=$(awk -v a="$p99" -v b="$d99" 'BEGIN{printf "%.3f", (a-b)/1000.0}')

  printf "%-7s | %-32s | %-32s | %s\n" \
    "$RPS" \
    "$(ms "$d50") / $(ms "$d99") / $(ms "$d999") / $(ms "$dmax")" \
    "$(ms "$p50") / $(ms "$p99") / $(ms "$p999") / $(ms "$pmax")" \
    "Δp50=$add50 Δp99=$add99 | self-p99=$(awk -v u="$self_p99" 'BEGIN{printf "%.3f", u/1000.0}')ms" \
    | tee -a "$SUMMARY"
done

echo | tee -a "$SUMMARY"
echo "Raw logs + per-RPS proxy histograms in $RESDIR/  (stamp $STAMP)" | tee -a "$SUMMARY"
echo "Done."