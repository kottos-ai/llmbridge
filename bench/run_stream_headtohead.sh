#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# Streaming (SSE) head-to-head: llmbridge vs LiteLLM, both translating the SAME
# Anthropic event stream from the SAME mock into OpenAI chat.completion.chunks,
# measured by the SAME client instrument, swept over concurrency.
#
# ── Fairness controls (each one is a way this could have been rigged) ─────────
#  1. SAME INSTRUMENT, BOTH SIDES. Every number comes from streamgen, client-side.
#     Neither gateway self-reports. (The non-streaming benchmark measures llmbridge
#     by self-report and LiteLLM by client delta — this one removes that asymmetry.)
#  2. SAME BASELINE. "Added" latency is measured against the identical
#     direct-to-mock control run at the SAME concurrency, subtracted the same way
#     for both. The floor (loopback + the Python mock's own jitter) is charged to
#     neither gateway.
#  3. SAME WORKERS. One worker each: llmbridge single-threaded, LiteLLM one uvicorn
#     worker. No SO_REUSEPORT fan-out for llmbridge.
#  4. SAME WARMUP. Both get the same (generous) warmup so LiteLLM's cold-start /
#     first-request path is never charged to it.
#  5. SAME WORK. Identical request body, model, mock, and token rate; neither side
#     requests stream_options.include_usage, so neither emits an extra usage chunk.
#  6. BOTH PROXIES STAY UP for the whole sweep — no restart between levels, so
#     neither gets a "fresh process" advantage at any concurrency.
#  7. ORDER ALTERNATES per level (llmbridge-first, then LiteLLM-first) so any
#     thermal/cache drift within a level doesn't systematically favour one side.
#  8. DISCARD ROUND. Both gateways get one full unmeasured round before the sweep
#     begins. LiteLLM's lazy imports / first-request path cost far more than a few
#     seconds of in-run warmup — measured directly, its FIRST level came out 134x
#     worse than the same level run later. Without this the sweep's first entry is
#     an artifact of cold start, not of the gateway.
#  9. MOCK-SATURATION GUARD. If the direct baseline itself degrades at a level, the
#     Python mock — not either gateway — is the limiter, and the level is flagged
#     UNRELIABLE rather than silently reported.
#
#   ./bench/run_stream_headtohead.sh [tokens] [interval_ms] [duration] [warmup] [streams...]
set -uo pipefail
cd "$(dirname "$0")/.."

export PYTHONUNBUFFERED=1 LITELLM_LOCAL_MODEL_COST_MAP=True \
       LITELLM_DONT_SHOW_FEEDBACK_BOX=True DISABLE_LLM_UPDATE_CHECK=1

TOKENS="${1:-60}"        # deltas per stream
INTERVAL="${2:-20}"      # ms between tokens (20 = 50 tok/s, a realistic rate)
DUR="${3:-20}"           # seconds per measurement
WARMUP="${4:-6}"         # seconds excluded from stats (generous: LiteLLM cold start)
shift $(( $# < 4 ? $# : 4 )) || true
STREAM_LIST=("${@:-1 4 16 64}")
if [ "${#STREAM_LIST[@]}" -eq 1 ]; then read -r -a STREAM_LIST <<< "${STREAM_LIST[0]}"; fi

MOCK_PORT=9601
GW_PORT=8601
LL_PORT=8602
VENV=bench/.litellm-venv
RESDIR=bench/results
mkdir -p "$RESDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
SUMMARY="$RESDIR/stream-headtohead-$STAMP.txt"
CSV="$RESDIR/stream-comparison.csv"
IO="${IO:-auto}"

cleanup() {
  [ -n "${LL_CFG:-}" ] && rm -f "$LL_CFG"
  [ -n "${MOCK_PID:-}" ] && kill "$MOCK_PID" 2>/dev/null
  [ -n "${GW_PID:-}"   ] && kill "$GW_PID"   2>/dev/null
  [ -n "${LL_PID:-}"   ] && kill "$LL_PID"   2>/dev/null
  wait 2>/dev/null
}
trap cleanup EXIT

BIN="${BIN:-}"
if [ -z "$BIN" ]; then
  for d in cmake-build-release/bin build-linux/bin build/bin cmake-build-debug/bin; do
    if [ -x "$d/llmbridge" ] && [ -x "$d/streamgen" ]; then BIN="$d"; break; fi
  done
fi
[ -z "$BIN" ] && { echo "No built llmbridge+streamgen; build first or set BIN=path" >&2; exit 1; }
echo "Using binaries in $BIN"

# A stale binary silently benchmarks the WRONG code — streaming lands in v0.3.0, so
# refuse to run against a gateway that can't stream.
if ! "$BIN/llmbridge" --help 2>&1 | grep -q 'upstream-timeout'; then
  echo "ERROR: $BIN/llmbridge looks stale (no --upstream-timeout). Rebuild before benchmarking." >&2
  exit 1
fi

wait_port() { for _ in $(seq 1 120); do (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }; sleep 0.5; done; return 1; }

# PROVIDER=fast (default): the C++ faststream. The Python mock saturates near
# 45-50k chunks/s — below llmbridge — so it would cap the comparison. faststream is
# wire-identical (same events, framing and emission stamps), so BOTH gateways see
# exactly the same upstream, and neither is measured against a limited provider.
PROVIDER="${PROVIDER:-fast}"
echo "Starting provider=$PROVIDER (anthropic SSE: ${TOKENS} tokens @ ${INTERVAL}ms) ..."
if [ "$PROVIDER" = "fast" ]; then
  "$BIN/faststream" --port "$MOCK_PORT" --tokens "$TOKENS" \
        --token-interval-us $((INTERVAL * 1000)) --prefill-us 20000 \
        >"$RESDIR/stream-mock-$STAMP.log" 2>&1 &
else
  python3 bench/mock_provider.py --port "$MOCK_PORT" --format anthropic --latency-ms 20 \
        --tokens "$TOKENS" --token-interval-ms "$INTERVAL" >"$RESDIR/stream-mock-$STAMP.log" 2>&1 &
fi
MOCK_PID=$!
wait_port "$MOCK_PORT" || { echo "mock failed to start" >&2; exit 1; }

echo "Starting llmbridge (1 worker, io=$IO) ..."
"$BIN/llmbridge" --listen "$GW_PORT" --upstream "127.0.0.1:$MOCK_PORT" \
        --translate anthropic --workers 1 --io "$IO" >"$RESDIR/stream-gw-$STAMP.log" 2>&1 &
GW_PID=$!
wait_port "$GW_PORT" || { echo "llmbridge failed to start" >&2; exit 1; }

echo "Starting LiteLLM (1 worker) ..."
# The checked-in config hardcodes the mock's default port; point it at THIS run's
# mock, otherwise LiteLLM dials a port nothing is listening on and every stream fails.
# Derived per-run artifact, not a result: keep it out of bench/results/ (which is
# partly tracked) so runs don't litter the repo with redundant configs.
LL_CFG="$(mktemp -t llmbridge-litellm-cfg-XXXXXX.yaml)"
sed "s#api_base: http://127.0.0.1:9001#api_base: http://127.0.0.1:$MOCK_PORT#" \
    bench/litellm_config_anthropic.yaml > "$LL_CFG"
"$VENV/bin/litellm" --config "$LL_CFG" --port "$LL_PORT" \
        >"$RESDIR/stream-litellm-$STAMP.log" 2>&1 &
LL_PID=$!
wait_port "$LL_PORT" || { echo "LiteLLM failed to start; see $RESDIR/stream-litellm-$STAMP.log" >&2; exit 1; }
sleep 3   # let LiteLLM finish its lazy imports before the first measured run

# Fairness control 8: unmeasured discard round so neither gateway's first measured
# level pays for cold start. Results are thrown away.
echo "Discard round (warming both gateways; results discarded) ..."
"$BIN/streamgen" --port "$GW_PORT" --streams 16 --duration 10 --warmup 0 \
      --header "Authorization: Bearer sk-dummy" --label discard >/dev/null 2>&1
"$BIN/streamgen" --port "$LL_PORT" --streams 16 --duration 15 --warmup 0 \
      --header "Authorization: Bearer sk-dummy" --label discard >/dev/null 2>&1

if [ ! -s "$CSV" ]; then
  echo "label,streams,chunks,completed,chunk_p50_us,chunk_p99_us,chunk_p999_us,chunk_max_us,ttft_p50_ms,ttft_p99_ms,gap_p50_ms,gap_p99_ms" > "$CSV"
fi

run_one() { # label port -> echoes "p50 p99 p999 chunks completed"
  local label="$1" port="$2" streams="$3" log="$RESDIR/stream-$1-$3-$STAMP.log"
  "$BIN/streamgen" --port "$port" --streams "$streams" --duration "$DUR" --warmup "$WARMUP" \
        --header "Authorization: Bearer sk-dummy" \
        --label "$label" --csv "$CSV" >"$log" 2>&1
  local p50 p99 p999 chunks completed
  p50=$(sed -n 's/.*p50=\([0-9]*\) p99=.*/\1/p' "$log" | head -1)
  p99=$(sed -n 's/.*p99=\([0-9]*\) p99\.9=.*/\1/p' "$log" | head -1)
  p999=$(sed -n 's/.*p99\.9=\([0-9]*\) max=.*/\1/p' "$log" | head -1)
  chunks=$(sed -n 's/^chunks=\([0-9]*\).*/\1/p' "$log" | head -1)
  completed=$(sed -n 's/.*completed_streams=\([0-9]*\).*/\1/p' "$log" | head -1)
  echo "${p50:-0} ${p99:-0} ${p999:-0} ${chunks:-0} ${completed:-0}"
}

{
  echo "streaming head-to-head — $(date)"
  echo "mock: ${TOKENS} tokens @ ${INTERVAL}ms/token (=$(awk "BEGIN{printf \"%.0f\", 1000/$INTERVAL}") tok/s per stream), 20ms prefill"
  echo "per level: ${DUR}s measured, ${WARMUP}s warmup, 1 worker each, same instrument both sides"
  echo
  printf "%-8s | %-26s | %-26s | %s\n" "streams" "llmbridge added us" "LiteLLM added us" "chunks (direct/kb/ll)"
  printf -- "---------+----------------------------+----------------------------+----------------------\n"
} | tee "$SUMMARY"

for S in "${STREAM_LIST[@]}"; do
  read -r d50 d99 d999 dch dcs <<< "$(run_one direct "$MOCK_PORT" "$S")"      # baseline first
  # Fairness control 7: alternate which gateway is measured first at each level.
  if [ $(( S % 2 )) -eq 0 ]; then
    read -r k50 k99 k999 kch kcs <<< "$(run_one llmbridge "$GW_PORT" "$S")"
    read -r l50 l99 l999 lch lcs <<< "$(run_one litellm  "$LL_PORT" "$S")"
  else
    read -r l50 l99 l999 lch lcs <<< "$(run_one litellm  "$LL_PORT" "$S")"
    read -r k50 k99 k999 kch kcs <<< "$(run_one llmbridge "$GW_PORT" "$S")"
  fi

  ka50=$(( k50 - d50 )); ka99=$(( k99 - d99 ))
  la50=$(( l50 - d50 )); la99=$(( l99 - d99 ))
  (( ka50 < 0 )) && ka50=0; (( ka99 < 0 )) && ka99=0
  (( la50 < 0 )) && la50=0; (( la99 < 0 )) && la99=0

  note=""
  # Guard 8: if the mock itself is struggling, neither gateway number is meaningful.
  if [ "$d99" -gt 5000 ]; then note="  [UNRELIABLE: mock saturated at this level]"; fi
  # Equal-work check: a gateway delivering far fewer chunks isn't "faster", it's lossy.
  if [ "$kch" -gt 0 ] && [ "$lch" -gt 0 ]; then
    ratio=$(awk "BEGIN{printf \"%.2f\", $lch/$kch}")
    note="$note  [chunk ratio ll/kb=$ratio]"
  fi

  printf "%-8s | p50 %-6s p99 %-10s | p50 %-6s p99 %-10s | %s/%s/%s%s\n" \
    "$S" "$ka50" "$ka99" "$la50" "$la99" "$dch" "$kch" "$lch" "$note" | tee -a "$SUMMARY"
done

{
  echo
  echo "Added latency = (path per-chunk latency) - (direct-to-mock per-chunk latency) at the"
  echo "same concurrency, where per-chunk latency = client arrival - the mock's own embedded"
  echo "emission timestamp. Same instrument, same baseline, same warmup for both gateways."
  echo "Co-located single host: absolute values are a dev-box upper bound."
  echo "CSV: $CSV"
} | tee -a "$SUMMARY"
echo "Summary: $SUMMARY"
