#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# CPU-per-token and saturation, epoll vs io_uring.
#
# Why this exists: io_uring issues 2.31 fewer syscalls per delivered token than epoll
# (measured with strace), but a syscall costs ~537 ns here, so the whole saving is
# ~1.24 us/token: under 4% of the added latency and below run-to-run variance. It is
# therefore invisible on the latency axis. If io_uring is worth anything on the
# streaming path it must show up as CPU efficiency (less CPU burned per token) and
# hence as headroom (a later knee). This script measures those two axes directly.
#
# It also separates two things the earlier saturation sweep conflated:
#   phase 1 (50 tok/s/stream): raises both stream count and tokens/s together, so at
#           >=2048 streams the provider's own ~100k tok/s ceiling co-limits the result
#   phase 2 (10 tok/s/stream): pushes stream count high while keeping tokens/s well
#           inside the provider's capacity, isolating connection scaling
#
#   ./bench/run_stream_cpu.sh [duration] [warmup]
set -uo pipefail
cd "$(dirname "$0")/.."

DUR="${1:-20}"
WARMUP="${2:-5}"
BIN=build-linux/bin
RESDIR=bench/results
mkdir -p "$RESDIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
CSV="$RESDIR/stream-cpu-$STAMP.csv"
MOCK=9961; GW_E=9962; GW_U=9963

PIDS=()
cleanup(){ for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

ticks_per_s=$(getconf CLK_TCK)
# process-wide CPU (all threads) in clock ticks: utime+stime from /proc/PID/stat
cpu_ticks(){ awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }

# This reference box is a mobile i7-9750H that reaches TjMax under sustained load and
# throttles. That is not a transient to wait out; it cannot be held cool while
# benchmarking. So record the thermal state per level: if late levels are throttled
# harder than early ones, the "knee" is partly thermal and the data must say so.
pkg_temp_c(){ local t; t=$(cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | sort -rn | head -1); echo $(( ${t:-0} / 1000 )); }
throttles(){ cat /sys/devices/system/cpu/cpu0/thermal_throttle/package_throttle_count 2>/dev/null || echo 0; }
max_mhz(){ awk '/cpu MHz/{if($4>m)m=$4} END{printf "%.0f", m}' /proc/cpuinfo; }

echo "backend,streams,tok_per_stream,offered_tok_s,delivered_tok_s,delivery_pct,p50_us,p99_us,cpu_s,cpu_us_per_tok,cpu_s_per_Mtok,temp_c,throttle_delta,max_mhz" > "$CSV"

start_all(){
  local interval_us="$1"
  $BIN/faststream --port $MOCK --tokens 60 --token-interval-us "$interval_us" --prefill-us "$interval_us" \
      >/tmp/cpu_prov.log 2>&1 & local FP=$!
  sleep 1.5
  $BIN/llmbridge --listen $GW_E --upstream 127.0.0.1:$MOCK --translate anthropic --workers 1 --io epoll \
      >/tmp/cpu_ge.log 2>&1 & local GE=$!
  $BIN/llmbridge --listen $GW_U --upstream 127.0.0.1:$MOCK --translate anthropic --workers 1 --io uring \
      >/tmp/cpu_gu.log 2>&1 & local GU=$!
  PIDS=($FP $GE $GU); EPOLL_PID=$GE; URING_PID=$GU
  sleep 2
  # discard round: no gateway's first measured level is its cold start
  for p in $GW_E $GW_U; do $BIN/streamgen --port $p --streams 64 --duration 4 --warmup 0 >/dev/null 2>&1; done
}

# $1=label $2=port $3=pid("" = no gateway) $4=streams $5=tok_per_stream
measure(){
  local lbl="$1" port="$2" pid="$3" streams="$4" tps="$5"
  local c0=0 c1=0 w0 w1 out th0 th1 tc mhz
  [ -n "$pid" ] && c0=$(cpu_ticks "$pid")
  th0=$(throttles)
  w0=$(date +%s%N)
  out=$($BIN/streamgen --port "$port" --streams "$streams" --duration "$DUR" --warmup "$WARMUP" 2>&1)
  w1=$(date +%s%N)
  [ -n "$pid" ] && c1=$(cpu_ticks "$pid")
  th1=$(throttles); tc=$(pkg_temp_c); mhz=$(max_mhz)

  local rate p50 p99 chunks
  rate=$(sed -n 's/.*chunk_rate=\([0-9]*\)\/s.*/\1/p' <<<"$out"); rate=${rate:-0}
  p50=$(sed -n 's/.*p50=\([0-9]*\).*/\1/p' <<<"$out" | head -1); p50=${p50:-0}
  p99=$(sed -n 's/.* p99=\([0-9]*\).*/\1/p' <<<"$out" | head -1); p99=${p99:-0}
  chunks=$(sed -n 's/.*chunks=\([0-9]*\).*/\1/p' <<<"$out" | head -1); chunks=${chunks:-0}
  grep -qi overflow <<<"$out" && p99="OVERFLOW"

  local offered=$((streams * tps))
  awk -v lbl="$lbl" -v s="$streams" -v tps="$tps" -v off="$offered" -v r="$rate" \
      -v p50="$p50" -v p99="$p99" -v dt="$((c1-c0))" -v tk="$ticks_per_s" \
      -v ch="$chunks" -v w="$((w1-w0))" -v csv="$CSV" \
      -v tc="$tc" -v thd="$((th1-th0))" -v mhz="$mhz" '
  BEGIN{
    cpu = (tk>0 ? dt/tk : 0);
    pct = (off>0 ? 100.0*r/off : 0);
    upt = (ch>0 ? cpu*1e6/ch : 0);
    printf "  %-8s streams=%-5s offered=%-7s delivered=%-7s (%5.1f%%)  p50=%-6s p99=%-8s cpu=%5.2fs  %5.2f us/tok  [%sC %sMHz thr+%s]\n",
           lbl, s, off, r, pct, p50, p99, cpu, upt, tc, mhz, thd;
    printf "%s,%s,%s,%s,%s,%.1f,%s,%s,%.2f,%.3f,%.1f,%s,%s,%s\n", lbl,s,tps,off,r,pct,p50,p99,cpu,upt,upt,tc,thd,mhz >> csv;
  }'
}

phase(){
  local title="$1" interval_us="$2" tps="$3"; shift 3
  local levels=("$@")
  echo
  echo "=================================================================================="
  echo " $title  (${tps} tok/s per stream, ${DUR}s/level, ${WARMUP}s warmup)"
  echo "=================================================================================="
  start_all "$interval_us"
  for s in "${levels[@]}"; do
    echo "--- $s concurrent streams ---"
    measure direct "$MOCK" ""            "$s" "$tps"
    measure epoll  "$GW_E" "$EPOLL_PID"  "$s" "$tps"
    measure uring  "$GW_U" "$URING_PID"  "$s" "$tps"
  done
  cleanup; PIDS=(); sleep 2
}

echo "llmbridge streaming CPU-efficiency + saturation, $STAMP"
echo "host: $(uname -r), idle states: $(paste -d/ <(cat /sys/devices/system/cpu/cpu0/cpuidle/state*/name | tr '\n' ' ') <(cat /sys/devices/system/cpu/cpu0/cpuidle/state*/disable | tr '\n' ' '))"
echo "NOTE: cpu is process-wide utime+stime over the whole run (incl. warmup), divided by"
echo "      total chunks; identical treatment for both backends, so the RATIO is the result."

phase "PHASE 1: tokens/s scaling (stream count and token rate rise together)" \
      20000 50 64 256 512 1024 2048 2560 3072 3584 4096

phase "PHASE 2: connection scaling (token rate kept inside provider capacity)" \
      100000 10 1024 2048 4096 6144 8192

echo
echo "CSV: $CSV"
