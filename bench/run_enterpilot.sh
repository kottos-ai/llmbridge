#!/usr/bin/env bash
# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Run llmbridge inside the ENTERPILOT gateway benchmark, against the same six
# gateways it already measures, on a methodology we did not design.
#
# Why this exists alongside bench/run_headtohead.sh: ours compares one competitor on
# a laptop with a harness we wrote, which a skeptic cannot reproduce and cannot check
# for bias. This one is public, runs every gateway through the same mock and load
# generator, and records a no-gateway baseline we can validate against.
#
#   bench/run_enterpilot.sh                 # llmbridge + the five it ships with
#   bench/run_enterpilot.sh --only llmbridge
#   bench/run_enterpilot.sh --gateways "llmbridge bifrost litellm"
#   bench/run_enterpilot.sh --force         # run on a busy host, smoke test only
#
# Needs docker and the compose plugin. No AWS: the upstream project drives an EC2
# instance from run.sh, and this calls the on-instance runner directly instead.
set -uo pipefail
cd "$(dirname "$0")/.."
SRC="$PWD"

UPSTREAM=https://github.com/ENTERPILOT/ai-gateway-reproducible-benchmark
WORK="${ENTERPILOT_DIR:-$SRC/bench/.enterpilot}"
RES_LOGDIR="$SRC/bench/results/enterpilot"   # console logs, kept next to the numbers
GATEWAYS="tcprelay llmbridge llmbridge-anthropic gomodel litellm portkey bifrost tensorzero omniroute"
FORCE=0
ORIG_ARGS=("$@")   # the parse loop below shifts these away; the docker re-exec needs them
while [ $# -gt 0 ]; do
  case "$1" in
    --only) GATEWAYS="$2"; shift 2 ;;
    --gateways) GATEWAYS="$2"; shift 2 ;;
    --force) FORCE=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

command -v docker >/dev/null || { echo "docker not installed" >&2; exit 1; }
docker compose version >/dev/null 2>&1 || { echo "docker compose plugin missing" >&2; exit 1; }
# A fresh `usermod -aG docker` does not reach a login session that predates it, and
# re-logging in is a poor thing to demand mid-benchmark. If the group grants access,
# re-exec the whole script under it once.
if ! docker info >/dev/null 2>&1; then
  if [ "${LLMBRIDGE_SG_RETRY:-0}" != 1 ] && id -nG "$(id -un)" 2>/dev/null | tr ' ' '\n' | grep -qx docker ||
     getent group docker 2>/dev/null | grep -q "[:,]$(id -un)\(,\|$\)"; then
    if [ "${LLMBRIDGE_SG_RETRY:-0}" != 1 ] && command -v sg >/dev/null; then
      echo "note: this login session predates the docker group; re-running under it"
      export LLMBRIDGE_SG_RETRY=1
      exec sg docker -c "$(printf '%q ' "$0" "${ORIG_ARGS[@]}")"
    fi
  fi
  echo "cannot talk to the docker daemon. You are in the docker group but this login" >&2
  echo "session predates it: log out and back in, or run under: sg docker -c '...'" >&2
  exit 1
fi

# Same gate as rerun_litellm.sh, for the same reason: on 2026-09-02 a run on a host
# with two IDEs open put llmbridge's own p99 six times above its published value, and
# both columns moved together. A busy host measures the host.
unfit=""
gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
[ "$gov" = performance ] || unfit="$unfit\n  governor is $gov, want performance"
load=$(awk '{print $1}' /proc/loadavg)
awk -v l="$load" 'BEGIN{exit !(l>0.5)}' && unfit="$unfit\n  load average $load, want a quiet host"
busy=$(ps -eo pcpu,comm --sort=-pcpu | awk 'NR>1 && $1>5 {printf "%s(%s%%) ", $2, $1}' | head -c 200)
[ -z "$busy" ] || unfit="$unfit\n  busy: $busy"
if [ -n "$unfit" ]; then
  printf 'host is not fit to benchmark:%b\n' "$unfit"
  if [ "$FORCE" != 1 ]; then
    echo "refusing. close the IDEs and browser, then:"
    echo "  sudo cpupower frequency-set -g performance"
    echo "or pass --force for a smoke test whose numbers must not be published."
    exit 1
  fi
  echo "--force given: SMOKE TEST ONLY, do not publish these numbers"
fi

if [ -d "$WORK/.git" ]; then
  echo "== updating harness in $WORK"
  git -C "$WORK" pull --ff-only >/dev/null 2>&1 || echo "  (pull failed, using the checkout as it stands)"
else
  echo "== cloning harness into $WORK"
  git clone --depth 1 "$UPSTREAM" "$WORK" || exit 1
fi
echo "  harness commit: $(git -C "$WORK" rev-parse --short HEAD 2>/dev/null)"

# Install our gateway definition. Copied, not symlinked, so the harness stays a
# clean checkout that `git -C "$WORK" status` can still speak for.
mkdir -p "$WORK/remote/gateways/llmbridge" "$WORK/remote/gateways/llmbridge-anthropic" \
         "$WORK/remote/gateways/tcprelay"
cp bench/enterpilot/compose.yml bench/enterpilot/gateway.env "$WORK/remote/gateways/llmbridge/"
cp bench/enterpilot/anthropic/compose.yml bench/enterpilot/anthropic/gateway.env \
   "$WORK/remote/gateways/llmbridge-anthropic/"
cp bench/enterpilot/tcprelay/compose.yml bench/enterpilot/tcprelay/gateway.env \
   "$WORK/remote/gateways/tcprelay/"

# The harness includes each gateway explicitly, so ours has to be listed. Idempotent.
INC="$WORK/remote/compose.yml"
for g in llmbridge llmbridge-anthropic tcprelay; do
  grep -q "gateways/$g/compose.yml" "$INC" && continue
  python3 - "$INC" "$g" <<'PY'
import io,sys
p,g=sys.argv[1],sys.argv[2]; s=io.open(p,encoding="utf-8").read()
anchor="  - gateways/gomodel/compose.yml\n"
assert s.count(anchor)==1, "harness compose.yml changed shape; add the include by hand"
io.open(p,"w",encoding="utf-8").write(s.replace(anchor, anchor+"  - gateways/%s/compose.yml\n" % g))
PY
  echo "  added $g to the harness include list"
done

# A killed run leaves bench-* containers behind, and the harness's own teardown cannot
# clear them: `compose down` interpolates every included service and dies on the other
# gateways' required image variables. Leftovers compete with the next run.
stale=$(docker ps -aq --filter "name=^bench-" 2>/dev/null)
if [ -n "$stale" ]; then
  echo "== removing containers left by an interrupted run"
  docker rm -f $stale >/dev/null 2>&1
fi
# Unconditional, and deliberately outside the branch above. A benchnet created by anything other
# than compose (a hand-run docker network create, say) makes compose refuse to own it:
# it warns about the missing label, the mock comes up unreachable, and every arm
# including the no-gateway baseline reports ok=0. That happened on 2026-09-02 and it
# looked like a catastrophic gateway failure instead of a stale network.
docker network rm benchnet >/dev/null 2>&1

# Docker's default seccomp filter is evaluated on every syscall, so it taxes each
# gateway in proportion to its kernel time. Measured 2026-09-02: one llmbridge worker
# went 32,785 -> 38,528 req/s with it off, an 18% gain, because this workload is 89%
# kernel. A gateway bound in userspace barely notices, so the default is not neutral
# between arms: it penalises the most syscall-efficient one hardest. Removed for every
# gateway without exception, and the run says so in its output.
python3 "$SRC/bench/enterpilot/unconfine.py" "$WORK/remote/gateways"

echo "== building llmbridge image (first run compiles from source, several minutes)"
# Plain docker build, not `docker compose build`: compose interpolates every included
# service and dies on the other gateways' required image variables, which
# run-on-instance.sh only exports once it reaches them.
docker build -t llmbridge:bench -f bench/enterpilot/Dockerfile "$SRC" || {
  echo "image build failed" >&2; exit 1; }

# One worker by default, because that is what the binary ships with and what an
# unmodified deployment gets. The harness hands LiteLLM one worker per core and the Go
# gateways take every core through their runtime, so the capacity column understates us
# at this setting; run with LLMBRIDGE_WORKERS=$(nproc) for the matched comparison, and
# say which setting produced any number quoted from it.
export LLMBRIDGE_WORKERS="${LLMBRIDGE_WORKERS:-1}"
echo "== llmbridge workers: $LLMBRIDGE_WORKERS"
echo "== running: $GATEWAYS"
RUNLOG="$RES_LOGDIR/enterpilot-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$(dirname "$RUNLOG")"
echo "console log: $RUNLOG"
cd "$WORK/remote"
GATEWAYS="$GATEWAYS" ./run-on-instance.sh 2>&1 | tee "$RUNLOG"
rc=${PIPESTATUS[0]}
cd "$SRC"

echo
echo "== summary"
# The runner writes to remote/results and overwrites it every time. run.sh normally
# archives that into results/<stamp>; we bypass run.sh, so do the archiving here or a
# second run silently destroys the first.
LATEST="$WORK/remote/results"
if [ -d "$LATEST" ]; then
  ARCHIVE="$WORK/results/$(date +%Y%m%d-%H%M%S)-local"
  mkdir -p "$ARCHIVE" && cp -r "$LATEST"/. "$ARCHIVE"/ 2>/dev/null && echo "archived to $ARCHIVE"
fi
if [ -n "$LATEST" ] && [ -f "$WORK/scripts/summarize.py" ]; then
  python3 "$WORK/scripts/summarize.py" --results-dir "$LATEST" 2>/dev/null || ls -1 "$LATEST"
  python3 "$SRC/bench/enterpilot/corrected_overhead.py" "$LATEST" 2>/dev/null || true
  echo "results: $LATEST"
else
  echo "no results directory found; check the output above"
fi

cat <<'EOF'

== reading this honestly
This run removes Docker's seccomp and apparmor filters from every gateway, which the
upstream harness leaves on. That is a deliberate divergence: the filter costs in
proportion to kernel time, so it is not neutral between arms. Numbers from this run are
therefore not directly comparable to the figures published at enterpilot.io, and any
table mixing them must say which had the filters on.

The tcprelay row is not a competitor. It is a socat byte pipe that parses no HTTP, so
it measures the cost of being in the path: one extra bridge traversal the baseline
never pays. Every gateway's honest overhead is its added figure less the tcprelay
one, because the harness differences against a baseline with one fewer network hop.
Ignore tcprelay's capacity column: socat forks per connection and collapses.

Run bench/enterpilot/probe_dialect.sh afterwards to see what each gateway actually
sent upstream. The mock serves all three dialects, so "6 of 6" can mean forwarding
rather than translating, and the table should say which.

Check the no-gateway baseline row first. If it moved from previous runs, the host is
the variable and no gateway column means anything, which is the rule that caught a
void run on 2026-09-02.

llmbridge should serve 4 of the 6 workloads. chat and responses byte-forward to the
mock with the request line intact; /v1/messages is refused, because that pair needs
an Anthropic-to-OpenAI translator and it is not built. A refusal is not a failure and
must not be reported as one. Portkey scores 4/6 in this harness for its own reasons.

Both numbers belong in bench/BENCHMARK-CONFIG.md whichever way they come out, with
the harness commit recorded next to them.
EOF
exit $rc
