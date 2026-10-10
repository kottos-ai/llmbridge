#!/bin/sh
# SIGINT must end a --duration run at once, with exit 0, not after the deadline.
# usage: sigint_ends_duration.sh PATH/TO/llmbridge
log=$(mktemp)
"$1" --listen 0 --upstream 127.0.0.1:9 --duration 60 2>"$log" &
pid=$!
i=0
until grep -q "backend requested" "$log"; do
    i=$((i + 1))
    if [ "$i" -gt 400 ]; then kill -9 "$pid"; cat "$log"; rm -f "$log"; exit 1; fi
    sleep 0.05
done
sleep 0.5 # the handlers go in right after the constructor logs this
start=$(date +%s)
kill -INT "$pid"
wait "$pid"
rc=$?
elapsed=$(($(date +%s) - start))
cat "$log"
rm -f "$log"
echo "rc=$rc elapsed=${elapsed}s"
[ "$rc" -eq 0 ] && [ "$elapsed" -lt 5 ]
