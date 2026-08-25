#!/usr/bin/env bash

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

# debug_trace.sh: trace a single request end-to-end through the llmbridge
# gateway, showing the dialect translation at every hop.
#
# Topology:   curl ──OpenAI──▶ llmbridge ──translated──▶ logging mock
#                  ◀─OpenAI──            ◀─provider──┘
#
# It prints four stages so you can see exactly what the translator did:
#   1. client   -> gateway    the OpenAI request you send
#   2. gateway  -> upstream   the request after openai_to_<provider>_request()
#   3. upstream -> gateway    the canned provider response
#   4. gateway  -> client     the response after <provider>_to_openai_response()
# ...plus the gateway's own added-latency profile.
#
# Modes:
#   (default)   payload trace (above)
#   --gdb       also run the gateway under gdb, breaking inside every translate
#               function and printing its args + a backtrace (step the code)
#   --strace    also capture the gateway's network/read/write syscalls
#
# Usage:
#   ./bench/debug_trace.sh [--provider anthropic|gemini|cohere|none]
#                          [--io auto|epoll|uring] [--request FILE]
#                          [--bin DIR] [--gdb] [--strace] [--keep]
#
# Examples:
#   ./bench/debug_trace.sh                          # OpenAI⇄Anthropic round-trip
#   ./bench/debug_trace.sh --provider gemini        # OpenAI⇄Gemini
#   ./bench/debug_trace.sh --gdb                     # break inside the translator
#   ./bench/debug_trace.sh --request my_openai.json  # your own payload
set -uo pipefail
cd "$(dirname "$0")/.."

PROVIDER=anthropic
IO=auto
REQFILE=""
BIN_DIR=""
MODE=trace            # trace | gdb | strace
KEEP=0
GW_PORT=8088
MOCK_PORT=9001

while [ $# -gt 0 ]; do
  case "$1" in
    --provider) PROVIDER="${2:?}"; shift 2;;
    --io)       IO="${2:?}"; shift 2;;
    --request)  REQFILE="${2:?}"; shift 2;;
    --bin)      BIN_DIR="${2:?}"; shift 2;;
    --gdb)      MODE=gdb; shift;;
    --strace)   MODE=strace; shift;;
    --keep)     KEEP=1; shift;;
    -h|--help)  sed -n '11,40p' "$0"; exit 0;;
    *) echo "unknown arg: $1 (try --help)" >&2; exit 1;;
  esac
done
case "$PROVIDER" in anthropic|gemini|cohere|none) ;; *) echo "bad --provider: $PROVIDER" >&2; exit 1;; esac

# ── locate the gateway binary (same discovery as the bench scripts) ──────────
if [ -z "$BIN_DIR" ]; then
  for d in build/bin build-linux/bin cmake-build-debug/bin cmake-build-release/bin build-prof; do
    [ -x "$d/llmbridge" ] && BIN_DIR="$d" && break
  done
fi
GWBIN="$BIN_DIR/llmbridge"
[ -x "$GWBIN" ] || { echo "No llmbridge binary found. Build it, or pass --bin DIR." >&2; exit 1; }

WORK="$(mktemp -d)"
MOCK_PID=""; GW_PID=""
cleanup() {
  [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
  [ -n "$GW_PID" ]   && kill "$GW_PID"   2>/dev/null
  pkill -f "$WORK/dbgmock.py" 2>/dev/null
  if [ "$KEEP" = 1 ]; then echo "(artifacts kept in $WORK)"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

# ── colors / pretty-printers ─────────────────────────────────────────────────
if [ -t 1 ]; then G=$'\033[1;32m'; C=$'\033[1;36m'; Y=$'\033[1;33m'; D=$'\033[2m'; R=$'\033[0m'; else G=; C=; Y=; D=; R=; fi
hr()  { printf '\n%s── %s %s\n' "$G" "$*" "$R"; }
pp()  { jq . "$1" 2>/dev/null || cat "$1"; }

# ── the OpenAI request (system + user + params so translation is non-trivial) ─
# llmbridge translates the request *format*, not the *model identity*: the "model"
# string is passed through verbatim (model-name mapping is a routing concern). So
# pick a model name appropriate to the target provider for a realistic trace.
case "$PROVIDER" in
  anthropic) REQ_MODEL="claude-3-5-sonnet-20241022";;
  gemini)    REQ_MODEL="gemini-1.5-pro";;
  cohere)    REQ_MODEL="command-r-plus";;
  none)      REQ_MODEL="gpt-4o-mini";;
esac
if [ -n "$REQFILE" ]; then
  cp "$REQFILE" "$WORK/request.json"
else
  cat > "$WORK/request.json" <<JSON
{
  "model": "$REQ_MODEL",
  "messages": [
    { "role": "system", "content": "You are a terse developer." },
    { "role": "user",   "content": "Reply with exactly: pong" }
  ],
  "max_tokens": 64,
  "temperature": 0.2,
  "top_p": 0.9
}
JSON
fi

# ── logging mock upstream: dumps what it receives, returns a canned reply ─────
cat > "$WORK/dbgmock.py" <<'PY'
import os, json, socket
PORT = int(os.environ["DBG_PORT"]); FMT = os.environ["DBG_FORMAT"]; OUT = os.environ["DBG_OUT"]
CANNED = {
    "anthropic": {"id":"msg_dbg01","type":"message","role":"assistant",
        "model":"claude-3-5-sonnet-mock","content":[{"type":"text","text":"pong (from upstream)"}],
        "stop_reason":"end_turn","stop_sequence":None,"usage":{"input_tokens":14,"output_tokens":4}},
    "gemini": {"candidates":[{"content":{"role":"model","parts":[{"text":"pong (from upstream)"}]},
        "finishReason":"STOP","index":0}],
        "usageMetadata":{"promptTokenCount":14,"candidatesTokenCount":4,"totalTokenCount":18}},
    "cohere": {"id":"dbg01","finish_reason":"COMPLETE",
        "message":{"role":"assistant","content":[{"type":"text","text":"pong (from upstream)"}]},
        "usage":{"tokens":{"input_tokens":14,"output_tokens":4}}},
    "none": {"id":"chatcmpl-dbg01","object":"chat.completion","created":0,"model":"mock-1",
        "choices":[{"index":0,"message":{"role":"assistant","content":"pong (from upstream)"},
        "finish_reason":"stop"}],"usage":{"prompt_tokens":14,"completion_tokens":4,"total_tokens":18}},
}
body = json.dumps(CANNED[FMT]).encode()
open(os.path.join(OUT, "upstream_resp.json"), "w").write(json.dumps(CANNED[FMT], indent=2))
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT)); srv.listen(16)
print("debug-mock listening on :%d (format=%s)" % (PORT, FMT), flush=True)
while True:
    conn, _ = srv.accept(); buf = b""
    try:
        while True:
            while b"\r\n\r\n" not in buf:
                d = conn.recv(65536)
                if not d: raise ConnectionError
                buf += d
            head, _, rest = buf.partition(b"\r\n\r\n")
            clen = 0
            for line in head.split(b"\r\n")[1:]:
                k, _, v = line.partition(b":")
                if k.strip().lower() == b"content-length": clen = int(v.strip() or b"0")
            while len(rest) < clen:
                d = conn.recv(65536)
                if not d: break
                rest += d
            reqbody, buf = rest[:clen], rest[clen:]
            open(os.path.join(OUT, "upstream_raw.txt"), "wb").write(head + b"\r\n\r\n" + reqbody)
            try:
                open(os.path.join(OUT, "upstream_body.json"), "w").write(
                    json.dumps(json.loads(reqbody.decode() or "{}"), indent=2))
            except Exception:
                open(os.path.join(OUT, "upstream_body.json"), "wb").write(reqbody)
            print("received upstream request: %d body bytes" % len(reqbody), flush=True)
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                         + str(len(body)).encode() + b"\r\nConnection: keep-alive\r\n\r\n" + body)
    except Exception:
        pass
    finally:
        conn.close()
PY

echo "${C}llmbridge debug trace${R}  provider=${PROVIDER}  io=${IO}  mode=${MODE}  bin=${GWBIN}"

# ── start the logging mock ───────────────────────────────────────────────────
DBG_PORT=$MOCK_PORT DBG_FORMAT=$PROVIDER DBG_OUT="$WORK" python3 "$WORK/dbgmock.py" >"$WORK/mock.log" 2>&1 &
MOCK_PID=$!
sleep 0.4

GW_ARGS=(--listen "$GW_PORT" --upstream "127.0.0.1:$MOCK_PORT" --upstream-dialect "$PROVIDER" --workers 1)

case "$MODE" in
  trace)
    "$GWBIN" "${GW_ARGS[@]}" --io "$IO" >"$WORK/gateway.log" 2>&1 &
    GW_PID=$!
    ;;
  strace)
    strace -f -tt -e trace=network,read,write -o "$WORK/strace.log" \
      "$GWBIN" "${GW_ARGS[@]}" --io "$IO" --duration 6 >"$WORK/gateway.log" 2>&1 &
    GW_PID=$!
    ;;
  gdb)
    # io_uring is awkward to step; force epoll for a clean call stack.
    cat > "$WORK/trace.gdb" <<'GDB'
set pagination off
set print pretty on
set print frame-arguments all
break llmbridge::provider::openai_to_anthropic_request
break llmbridge::provider::anthropic_to_openai_response
break llmbridge::provider::openai_to_gemini_request
break llmbridge::provider::gemini_to_openai_response
break llmbridge::provider::openai_to_cohere_request
break llmbridge::provider::cohere_to_openai_response
commands 1 2 3 4 5 6
  silent
  printf "\n========================= TRANSLATE CALL =========================\n"
  info args
  printf "---- call stack ----\n"
  bt 5
  printf "==================================================================\n"
  continue
end
run
GDB
    gdb --batch -x "$WORK/trace.gdb" --args \
      "$GWBIN" "${GW_ARGS[@]}" --io epoll --duration 6 >"$WORK/gdb.log" 2>&1 &
    GW_PID=$!
    ;;
esac

# wait for the gateway to bind
for _ in $(seq 1 50); do
  (exec 3<>"/dev/tcp/127.0.0.1/$GW_PORT") 2>/dev/null && { exec 3>&- 3<&-; break; }
  sleep 0.1
done
[ "$MODE" = gdb ] && sleep 1.5   # give gdb time to install breakpoints

# ── fire one request ─────────────────────────────────────────────────────────
HTTP_CODE=$(curl -s -o "$WORK/response.json" -D "$WORK/resp_hdr.txt" -w '%{http_code}' \
  -H 'Content-Type: application/json' --data @"$WORK/request.json" \
  "http://127.0.0.1:$GW_PORT/v1/chat/completions" || echo "000")
sleep 0.3

# stop the gateway so it prints its added-latency profile
[ -n "$GW_PID" ] && { kill -TERM "$GW_PID" 2>/dev/null; wait "$GW_PID" 2>/dev/null; }

# ── report ───────────────────────────────────────────────────────────────────
hr "1 · CLIENT → GATEWAY   (OpenAI request, you sent this)"
pp "$WORK/request.json"

hr "2 · GATEWAY → UPSTREAM  (after openai_to_${PROVIDER}_request, the translation)"
[ "$PROVIDER" = none ] && echo "${D}(translate=none: byte-forwarded, identical to stage 1)${R}"
[ -f "$WORK/upstream_body.json" ] && pp "$WORK/upstream_body.json" || echo "${Y}(no upstream request captured: the gateway never forwarded)${R}"
[ "$PROVIDER" != none ] && echo "${D}note: the format is translated (system extracted, params renamed, blocks restructured); the \"model\" string is passed through verbatim; model-name mapping is a routing decision, not part of dialect translation. (Omit \"model\" entirely and the translator falls back to a provider default.)${R}"

hr "3 · UPSTREAM → GATEWAY  (canned ${PROVIDER} response)"
pp "$WORK/upstream_resp.json"

hr "4 · GATEWAY → CLIENT    (after ${PROVIDER}_to_openai_response, translated back)"
echo "${D}$(head -1 "$WORK/resp_hdr.txt" 2>/dev/null)  [curl http_code=$HTTP_CODE]${R}"
if [ -s "$WORK/response.json" ]; then pp "$WORK/response.json"; else echo "${Y}(empty response body)${R}"; fi

hr "Gateway added-latency profile (self-measured)"
grep -E 'gateway:|added-total|request-path|response-path|requests=' "$WORK/gateway.log" 2>/dev/null \
  || { echo "${D}(profile not found; full gateway log:)${R}"; cat "$WORK/gateway.log" 2>/dev/null; }

if [ "$MODE" = gdb ]; then
  hr "IN-CODE TRACE (gdb breakpoints inside the translator)"
  if grep -q 'TRANSLATE CALL' "$WORK/gdb.log" 2>/dev/null; then
    sed -n '/TRANSLATE CALL/,/=====$/p' "$WORK/gdb.log"
  else
    echo "${Y}No translate breakpoints hit. Notes:${R}"
    grep -iE 'No symbol|not defined|Breakpoint [0-9]+ at|warning' "$WORK/gdb.log" | head
    echo "${D}(needs a binary with symbols for llmbridge::provider::*; build a Debug build if these are stripped/inlined.)${R}"
  fi
fi

if [ "$MODE" = strace ]; then
  hr "SYSCALL TRACE (network + read/write, gateway side)"
  grep -E 'accept|connect|recvfrom|sendto|^.*read\(|^.*write\(|io_uring|epoll' "$WORK/strace.log" 2>/dev/null \
    | sed -n '1,60p'
  echo "${D}(full strace: $WORK/strace.log; re-run with --keep to inspect)${R}"
fi

hr "Done"
echo "Provider tested: ${PROVIDER}   |   stages above show the full round-trip translation."
[ "$KEEP" = 1 ] || echo "${D}(re-run with --keep to retain raw artifacts in a temp dir)${R}"
