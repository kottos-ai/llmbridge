#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# Streaming saturation hunt: push concurrency until something knees over, and say
# Which thing kneed. Only the direct baseline and llmbridge are run. LiteLLM
# saturates at ~64 streams (see run_stream_headtohead.sh), so including it here
# would burn minutes per level to re-measure a known ceiling.
#
# The critical distinction this script exists to make: at high concurrency the
# Python mock will eventually become the limiter. If the direct baseline stops
# scaling, the mock is the bottleneck and llmbridge's number at that level says
# nothing about llmbridge. Each level therefore reports achieved-vs-offered chunk
# rate for both paths, and flags which one (if either) fell behind.
#
#   ./bench/run_stream_saturate.sh [tokens] [interval_ms] [duration] [warmup] [streams...]
set -uo pipefail
cd "$(dirname "$0")/.."

TOKENS="${1:-60}"
INTERVAL="${2:-20}"
DUR="${3:-20}"
WARMUP="${4:-5}"
shift $(( $# < 4 ? $# : 4 )) || true
STREAM_LIST=("${@:-256 512 1024 2048}")
if [ "${#STREAM_LIST[@]}" -eq 1 ]; then read -r -a STREAM_LIST <<< "${STREAM_LIST[0]}"; fi

MOCK_PORT=${MOCK_PORT:-9901}
GW_PORT=${GW_PORT:-8901}
RESDIR=bench/results
mkdir -p "$RESDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
SUMMARY="$RESDIR/stream-saturate-$STAMP.txt"
IO="${IO:-auto}"

cleanup() {
  [ -n "${MOCK_PID:-}" ] && kill "$MOCK_PID" 2>/dev/null
  [ -n "${GW_PID:-}" ] && kill "$GW_PID" 2>/dev/null
  wait 2>/dev/null
}
trap cleanup EXIT

BIN="${BIN:-}"
if [ -z "$BIN" ]; then
  for d in cmake-build-release/bin build-linux/bin build/bin; do
    [ -x "$d/llmbridge" ] && [ -x "$d/streamgen" ] && { BIN="$d"; break; }
  done
fi
[ -z "$BIN" ] && { echo "No built binaries; set BIN=path" >&2; exit 1; }

wait_port() { for _ in $(seq 1 120); do (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }; sleep 0.5; done; return 1; }

# PROVIDER=fast (default) uses the C++ faststream: the Python mock saturates around
# 45-50k chunks/s, which is below llmbridge, so a sweep against it measures the
# harness. faststream is wire-identical (same events, same chunked framing, same
# emission stamp) but single-threaded epoll with no per-token allocation.
PROVIDER="${PROVIDER:-fast}"
if [ "$PROVIDER" = "fast" ]; then
  "$BIN/faststream" --port "$MOCK_PORT" --tokens "$TOKENS" \
        --token-interval-us $((INTERVAL * 1000)) --prefill-us 20000 \
        >"$RESDIR/sat-mock-$STAMP.log" 2>&1 &
else
  python3 bench/mock_provider.py --port "$MOCK_PORT" --format anthropic --latency-ms 20 \
        --tokens "$TOKENS" --token-interval-ms "$INTERVAL" >"$RESDIR/sat-mock-$STAMP.log" 2>&1 &
fi
MOCK_PID=$!
wait_port "$MOCK_PORT" || { echo "mock failed" >&2; exit 1; }

"$BIN/llmbridge" --listen "$GW_PORT" --upstream "127.0.0.1:$MOCK_PORT" --upstream-dialect anthropic \
        --workers 1 --io "$IO" >"$RESDIR/sat-gw-$STAMP.log" 2>&1 &
GW_PID=$!
wait_port "$GW_PORT" || { echo "llmbridge failed" >&2; exit 1; }

# unmeasured warm round
"$BIN/streamgen" --port "$GW_PORT" --streams 32 --duration 6 --warmup 0 --label warm >/dev/null 2>&1

run_one() { # port streams tag -> "p50 p99 chunks rate"
  # Note: separate `local` statements, because a single `local a=$1 log="...$a..."` does
  # not see the earlier assignment, which under `set -u` silently breaks the run.
  local port="$1"
  local streams="$2"
  local tag="$3"
  local log="$RESDIR/sat-$tag-$streams-$STAMP.log"
  "$BIN/streamgen" --port "$port" --streams "$streams" --duration "$DUR" --warmup "$WARMUP" \
        --label "$tag" >"$log" 2>&1
  local p50 p99 chunks rate
  p50=$(sed -n 's/.*p50=\([0-9]*\) p99=.*/\1/p' "$log" | head -1)
  p99=$(sed -n 's/.*p99=\([0-9]*\) p99\.9=.*/\1/p' "$log" | head -1)
  chunks=$(sed -n 's/^chunks=\([0-9]*\).*/\1/p' "$log" | head -1)
  rate=$(sed -n 's/.*chunk_rate=\([0-9]*\).*/\1/p' "$log" | head -1)
  echo "${p50:-0} ${p99:-0} ${chunks:-0} ${rate:-0}"
}

{
  echo "streaming saturation hunt, $(date)   io=$IO  provider=${PROVIDER:-fast}"
  echo "mock: $TOKENS tokens @ ${INTERVAL}ms  (offered = streams x $(awk "BEGIN{printf \"%.0f\",1000/$INTERVAL}") chunks/s)"
  echo
  printf "%-8s | %-10s | %-28s | %-28s | %s\n" \
    "streams" "offered/s" "DIRECT achieved/s (p50/p99)" "LLMBRIDGE achieved/s (p50/p99)" "verdict"
  printf -- "---------+------------+------------------------------+------------------------------+--------\n"
} | tee "$SUMMARY"

for S in "${STREAM_LIST[@]}"; do
  offered=$(awk "BEGIN{printf \"%.0f\", $S * 1000/$INTERVAL}")
  read -r d50 d99 dch drate <<< "$(run_one "$MOCK_PORT" "$S" direct)"
  read -r k50 k99 kch krate <<< "$(run_one "$GW_PORT" "$S" llmbridge)"

  # Achieved/offered tells us who kneed. The mock is Python: it is expected to be
  # the first thing to fall over, and when it does, llmbridge's number is not a
  # measurement of llmbridge.
  dpct=$(awk -v r="${drate:-0}" -v o="${offered:-0}" 'BEGIN{ if (o>0) printf "%d", (r/o)*100; else printf "0" }')
  kpct=$(awk -v r="${krate:-0}" -v o="${offered:-0}" 'BEGIN{ if (o>0) printf "%d", (r/o)*100; else printf "0" }')
  dpct=${dpct:-0}; kpct=${kpct:-0}
  verdict="ok"
  if [ "$dpct" -lt 90 ]; then
    verdict="PROVIDER saturated (${dpct}%); llmbridge number not meaningful here"
  elif [ "$kpct" -lt 90 ]; then
    verdict="LLMBRIDGE saturated (${kpct}% of offered)"
  fi

  printf "%-8s | %-10s | %-8s (%3s%%) %5s/%-6s | %-8s (%3s%%) %5s/%-6s | %s\n" \
    "$S" "$offered" "$drate" "$dpct" "$d50" "$d99" "$krate" "$kpct" "$k50" "$k99" "$verdict" | tee -a "$SUMMARY"
done

echo | tee -a "$SUMMARY"
echo "p50/p99 are per-chunk added latency in us (arrival - mock's embedded emission stamp)." | tee -a "$SUMMARY"
echo "Summary: $SUMMARY"
