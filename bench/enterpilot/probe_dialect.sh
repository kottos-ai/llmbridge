#!/usr/bin/env bash
# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# What did each gateway actually send upstream?
#
# The harness mock serves all three dialects on one port, so a gateway can satisfy
# /v1/messages by forwarding an Anthropic request to the mock's Anthropic endpoint. It
# never has to translate. That matters because "serves 6 of 6" reads like a capability
# claim and may only be a routing one, and we are about to publish a table next to it.
#
# The mock records every request it received and returns the newest at /__last, so this
# settles it with evidence: send one request per dialect through a gateway, then read
# the path the mock saw. Same path in and out means it was forwarded; a different path
# means a translator ran.
#
#   bench/enterpilot/probe_dialect.sh                    # every gateway
#   bench/enterpilot/probe_dialect.sh llmbridge gomodel
set -uo pipefail
cd "$(dirname "$0")/../.."
SRC="$PWD"
WORK="${ENTERPILOT_DIR:-$SRC/bench/.enterpilot}"
R="$WORK/remote"
[ -d "$R/gateways" ] || { echo "harness not present; run bench/run_enterpilot.sh first" >&2; exit 1; }
cd "$R"

if ! docker info >/dev/null 2>&1 && command -v sg >/dev/null && [ "${PROBE_SG:-0}" != 1 ]; then
  export PROBE_SG=1; exec sg docker -c "$(printf '%q ' "$0" "$@")"
fi

GATEWAYS="${*:-$(ls gateways)}"
export BENCH_TOOLS_IMAGE="${BENCH_TOOLS_IMAGE:-bench-tools:local}"
export LLMBRIDGE_WORKERS="${LLMBRIDGE_WORKERS:-1}"
PROFILES=$(for d in gateways/*/; do printf -- '--profile %s ' "$(basename "$d")"; done)

# The mock only captures requests when MOCK_RECORD=1, which the harness leaves unset so
# a benchmark run stays byte-identical. An override file turns it on for the probe
# without editing the harness checkout.
OVR=$(mktemp /tmp/probe-override-XXXXXX.yml)
cat > "$OVR" <<YML
services:
  mock:
    environment:
      - MOCK_PORT=9999
      - MOCK_RECORD=1
YML
COMPOSE=(docker compose -f compose.yml -f "$OVR")

# Export the per-gateway image variable for every gateway before any compose call: compose interpolates
# every included service, not only the one being started.
for d in gateways/*/; do
  n=$(basename "$d"); up=$(echo "$n" | tr 'a-z-' 'A-Z_')
  IMAGE=""; PORT=""; . "$d/gateway.env"
  export "${up}_IMAGE=${IMAGE}"
done

cleanup() {
  "${COMPOSE[@]}" $PROFILES down -v >/dev/null 2>&1
  docker network rm benchnet >/dev/null 2>&1
}
# cleanup is called explicitly before the run too, so deleting the override file has to
# happen on exit only. Doing it inside cleanup removed the file before compose read it.
trap 'cleanup; rm -f "$OVR"' EXIT
cleanup
if ! "${COMPOSE[@]}" up -d mock >/tmp/probe-mock.log 2>&1; then
  echo "mock failed to start:" >&2; tail -5 /tmp/probe-mock.log >&2; exit 1
fi
sleep 1

printf '\n%-22s %-22s %-24s %s\n' "gateway" "sent to" "arrived at mock as" "verdict"
printf '%s\n' "----------------------------------------------------------------------------------------------"

for g in $GATEWAYS; do
  [ -d "gateways/$g" ] || continue
  IMAGE=""; PORT=""; MODEL="gpt-4o-mini"; HEADERS=()
  CHAT_PATH="/v1/chat/completions"; RESPONSES_PATH="/v1/responses"; MESSAGES_PATH="/v1/messages"
  . "gateways/$g/gateway.env"
  "${COMPOSE[@]}" --profile "$g" up -d "$g" >/dev/null 2>&1
  for _ in $(seq 1 60); do
    "${COMPOSE[@]}" exec -T mock true >/dev/null 2>&1 || true
    code=$(docker run --rm --network benchnet curlimages/curl:latest -s -o /dev/null -w '%{http_code}' \
           --max-time 3 -X POST "http://$g:$PORT$CHAT_PATH" -H 'Content-Type: application/json' \
           -d "{\"model\":\"$MODEL\",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}" 2>/dev/null || echo 000)
    [ "$code" = 200 ] && break
    sleep 1
  done

  for dialect in chat messages responses; do
    case "$dialect" in
      chat)      p="$CHAT_PATH";      body="{\"model\":\"$MODEL\",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}" ;;
      messages)  p="$MESSAGES_PATH";  body="{\"model\":\"$MODEL\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}" ;;
      responses) p="$RESPONSES_PATH"; body="{\"model\":\"$MODEL\",\"input\":\"ping\"}" ;;
    esac
    docker run --rm --network benchnet curlimages/curl:latest -s -o /dev/null --max-time 5 \
      -X POST "http://mock:9999/__reset" >/dev/null 2>&1
    code=$(docker run --rm --network benchnet curlimages/curl:latest -s -o /dev/null -w '%{http_code}' \
           --max-time 5 -X POST "http://$g:$PORT$p" -H 'Content-Type: application/json' \
           -d "$body" 2>/dev/null || echo 000)
    seen=$(docker run --rm --network benchnet curlimages/curl:latest -s --max-time 5 \
           "http://mock:9999/__last" 2>/dev/null |
           python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("path","-"))
except Exception: print("-")' 2>/dev/null || echo "-")
    if [ "$code" != 200 ]; then verdict="refused ($code)"
    elif [ "$seen" = "-" ]; then verdict="answered without calling upstream"
    elif [ "$seen" = "$p" ]; then verdict="forwarded"
    else verdict="TRANSLATED"; fi
    printf '%-22s %-22s %-24s %s\n' "$g" "$p" "$seen" "$verdict"
  done
  "${COMPOSE[@]}" --profile "$g" rm -sf "$g" >/dev/null 2>&1
done
echo
echo "forwarded = the mock saw the same endpoint the client called, so no translation ran."
