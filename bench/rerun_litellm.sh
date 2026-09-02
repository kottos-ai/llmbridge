#!/usr/bin/env bash
# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Re-measure LiteLLM at its current release, because the published comparison was
# taken against 1.95.0 on 2026-08-06. Since then LiteLLM shipped a compiled Rust
# path (litellm/rust_bridge/_native.abi3.so). It is opt-in behind LITELLM_RUST, it
# serves only the anthropic and bedrock providers, and it declines every streamed
# request. So there are two honest numbers, not one: what a user gets by default,
# and the best the release can do on the non-streamed path.
#
#   bench/rerun_litellm.sh              # both arms, default and LITELLM_RUST=1
#   bench/rerun_litellm.sh --rust       # only the Rust arm
#   bench/rerun_litellm.sh --default    # only the default arm
#   bench/rerun_litellm.sh --version 1.99.0
#   bench/rerun_litellm.sh --force      # run anyway on an unfit host
#
# It builds its own venv and never touches bench/.litellm-venv, so the 1.95.0
# baseline stays reproducible and the two can be compared on one host.
set -uo pipefail
cd "$(dirname "$0")/.."

VERSION=""; ARMS="default rust"; FORCE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --rust) ARMS="rust"; shift ;;
    --default) ARMS="default"; shift ;;
    --force) FORCE=1; shift ;;
    --version) VERSION="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

RES=bench/results; mkdir -p "$RES"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$RES/litellm-rerun-$STAMP.txt"

# A warm laptop measures the laptop. This caught a real one: a run on 2026-09-02
# put llmbridge's own p99 at 0.468 ms against a published 0.080 ms, and the
# LiteLLM column moved with it, so neither number meant anything.
unfit=""
gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
[ "$gov" = performance ] || unfit="$unfit\n  governor is $gov, want performance"
load=$(awk '{print $1}' /proc/loadavg)
awk -v l="$load" 'BEGIN{exit !(l>0.5)}' && unfit="$unfit\n  load average $load, want a quiet host"
busy=$(ps -eo pcpu,comm --sort=-pcpu | awk 'NR>1 && $1>5 {printf "%s(%s%%) ", $2, $1}' | head -c 200)
[ -z "$busy" ] || unfit="$unfit\n  busy: $busy"
if [ -n "$unfit" ]; then
  printf 'host is not fit to benchmark:%b\n' "$unfit" | tee -a "$LOG"
  if [ "$FORCE" != 1 ]; then
    echo "refusing; close the IDEs and browser, pin the governor, or pass --force for a smoke test" | tee -a "$LOG"
    exit 1
  fi
  echo "--force given: SMOKE TEST ONLY, do not publish these numbers" | tee -a "$LOG"
fi

if [ -z "$VERSION" ]; then
  VERSION=$(curl -s --max-time 30 https://pypi.org/pypi/litellm/json |
            python3 -c 'import json,sys; print(json.load(sys.stdin)["info"]["version"])' 2>/dev/null)
  [ -n "$VERSION" ] || { echo "could not reach PyPI; pass --version" >&2; exit 1; }
fi
VENV=bench/.litellm-venv-current
echo "target: litellm $VERSION, arms: $ARMS" | tee -a "$LOG"

# Installing unpinned is the point: the 1.95.0 pin exists because a newer LiteLLM
# once pulled a FastAPI that removed get_flat_dependant. If that still breaks, it
# is the finding and it gets recorded instead of worked around.
rm -rf "$VENV"
python3 -m venv "$VENV" >>"$LOG" 2>&1
"$VENV/bin/pip" install --quiet --upgrade pip >>"$LOG" 2>&1
if ! "$VENV/bin/pip" install --quiet "litellm[proxy]==$VERSION" >>"$LOG" 2>&1; then
    echo "install of litellm[proxy]==$VERSION FAILED, see $LOG" >&2; exit 1
fi

{
  echo "=== versions actually installed"
  "$VENV/bin/pip" freeze | grep -iE '^(litellm|fastapi|sse-starlette|uvicorn|pydantic|openai)='
  echo "=== compiled Rust path in this release?"
  SP=$(echo "$VENV"/lib/python*/site-packages)
  if [ -f "$SP/litellm/rust_bridge/_native.abi3.so" ]; then
      echo "  present: $(du -h "$SP/litellm/rust_bridge/_native.abi3.so" | cut -f1) _native.abi3.so"
      grep -h "RUST_CHAT_COMPLETIONS_PROVIDERS: Final" "$SP/litellm/rust_bridge/chat_completions.py" |
          sed 's/^/  providers: /'
      grep -q "if stream:" "$SP/litellm/rust_bridge/chat_completions.py" &&
          echo "  declines streamed requests (if stream: return False)"
  else
      echo "  absent: no _native.abi3.so in litellm/rust_bridge"
  fi
} | tee -a "$LOG"

for arm in $ARMS; do
  echo "=== arm: $arm" | tee -a "$LOG"
  if [ "$arm" = rust ]; then export LITELLM_RUST=1; else unset LITELLM_RUST; fi
  H2H_CSV="$RES/phase-a-comparison-$arm-$STAMP.csv" \
    LITELLM_VENV="$VENV" bench/run_headtohead.sh 2>&1 | tee -a "$LOG"
done

cat <<EOF | tee -a "$LOG"

=== what to do with this
Baseline, published 2026-08-06 on litellm 1.95.0, cold host, median of 3:
  llmbridge added p99   80 us at 100 RPS
  LiteLLM  added p99    87 ms at 100 RPS
  LiteLLM  ceiling      ~246 RPS, single worker

Check llmbridge's own p99 against that 80 us BEFORE reading the LiteLLM column. If
the control moved, the run measured the host and neither column is publishable.

The streaming claims are unaffected by the Rust path either way: it returns False
for every streamed request, so bench/run_streaming.sh needs no re-measurement on
that account.

Record the version and both arms in BENCHMARK-CONFIG.md "Competitor versions
(pinned)". If LiteLLM has closed the gap, that belongs in BENCHMARKS.md the same
day: a stale competitive claim is the one thing a launch cannot survive.
Full log: $LOG
EOF
