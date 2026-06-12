#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# Find the single-thread saturation point of the llmbridge proxy.
#
# Uses the C++ fastbackend (instant response) so the *backend* never saturates
# first — confirmed by also driving the backend directly at each RPS. The
# saturation point is the highest target RPS where the proxy still tracks the
# target (achieved ~= target) with a tight client tail; past it, achieved
# plateaus, the loadgen backlog explodes, and client p99 climbs while the
# direct-to-backend leg keeps tracking — proving the proxy is the bottleneck.
#
#   ./bench/saturate.sh [dur] [warmup] [rps_list...]
set -uo pipefail
cd "$(dirname "$0")/.."

DUR="${1:-6}"
WARMUP="${2:-2}"
shift $(( $# < 2 ? $# : 2 )) || true
RPS_LIST=(${@:-20000 40000 60000 80000 100000 120000})
BODY="${BODY:-64}" # request body size in bytes (loadgen --body-bytes)
WORKERS="${WORKERS:-1}" # number of SO_REUSEPORT worker threads
IO="${IO:-uring}"        # event-loop backend (auto|epoll|uring)
BACKENDS="${BACKENDS:-1}" # mock backend instances (SO_REUSEPORT) so the backend is not the cap

BACK_PORT=9002
PROXY_PORT=8090
CONNS=2048
RESDIR=bench/results
mkdir -p "$RESDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$RESDIR/saturation-$STAMP.txt"

# Locate CMake-built binaries (CLion uses cmake-build-*/bin). Override: BIN=path
BIN="${BIN:-}"
if [ -z "$BIN" ]; then
  for d in cmake-build-release/bin cmake-build-debug/bin build-linux/bin build/bin; do
    if [ -x "$d/llmbridge" ]; then BIN="$d"; break; fi
  done
fi
[ -z "$BIN" ] && { echo "No built binaries; build the CMake project first or set BIN=path" >&2; exit 1; }

cleanup() { pkill -f "$BIN/fastbackend" 2>/dev/null; pkill -f "$BIN/llmbridge" 2>/dev/null; }
trap cleanup EXIT
cleanup; sleep 0.3
echo "Using binaries in $BIN"

for b in $(seq 1 "$BACKENDS"); do
  $BIN/fastbackend --port "$BACK_PORT" --latency-us 0 >>"$RESDIR/fb-$STAMP.log" 2>&1 &
done
sleep 0.5

extract() { sed -n 's/.*achieved=\([0-9]*\) .*p50_us=\([0-9]*\) p99_us=\([0-9]*\) p999_us=\([0-9]*\) max_us=\([0-9]*\).*/\1 \2 \3 \4 \5/p'; }
backlog_of() { sed -n 's/.*max_backlog=\([0-9]*\).*/\1/p' "$1" | tail -1; }

{
  echo "llmbridge single-thread saturation — $STAMP  (instant C++ backend, ${DUR}s/level, ${WARMUP}s warmup, body=${BODY}B, io=${IO}, workers=${WORKERS}, backends=${BACKENDS})"
  printf "%-9s | %-22s | %-38s | %s\n" "TARGET" "DIRECT backend" "THROUGH proxy" "proxy"
  printf "%-9s | %-22s | %-38s | %s\n" "RPS" "achieved (p99 µs)" "achieved / p50 / p99 / p99.9 / max (µs)" "backlog"
  echo "----------+------------------------+----------------------------------------+--------"
} | tee "$OUT"

for RPS in "${RPS_LIST[@]}"; do
  echo ">>> RPS=$RPS" >&2
  # direct-to-backend control
  D=$($BIN/loadgen --target 127.0.0.1:$BACK_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" --conns "$CONNS" --body-bytes "$BODY" 2>/dev/null)
  read -r da d_p50 d_p99 d_p999 d_max <<< "$(echo "$D" | extract)"

  # fresh proxy per level
  $BIN/llmbridge --listen $PROXY_PORT --upstream 127.0.0.1:$BACK_PORT --io "$IO" --workers "$WORKERS" \
      --duration $((DUR + WARMUP + 3)) --warmup "$WARMUP" >"$RESDIR/sat-proxy-$RPS-$STAMP.log" 2>&1 &
  PP=$!
  sleep 0.7
  PLOG="$RESDIR/sat-loadgen-$RPS-$STAMP.log"
  P=$($BIN/loadgen --target 127.0.0.1:$PROXY_PORT --rps "$RPS" --duration "$DUR" --warmup "$WARMUP" --conns "$CONNS" --body-bytes "$BODY" 2>"$PLOG")
  read -r pa p_p50 p_p99 p_p999 p_max <<< "$(echo "$P" | extract)"
  bl=$(backlog_of "$PLOG")
  kill -TERM "$PP" 2>/dev/null; wait "$PP" 2>/dev/null

  : "${da:=0}" "${d_p99:=0}" "${pa:=0}" "${p_p50:=0}" "${p_p99:=0}" "${p_p999:=0}" "${p_max:=0}" "${bl:=0}"
  printf "%-9s | %-22s | %-38s | %s\n" "$RPS" \
    "$da ($d_p99)" \
    "$pa / $p_p50 / $p_p99 / $p_p999 / $p_max" \
    "$bl" | tee -a "$OUT"
done
echo "Logs in $RESDIR/ (stamp $STAMP)" | tee -a "$OUT"