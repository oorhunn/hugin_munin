#!/bin/sh
# Launches the app and fails if it exits on its own. A Swift concurrency error can pass every test
# and still crash the app on launch, so the tests alone do not cover this.
# Usage: scripts/smoke_test.sh [path/to/HuginMunin.app] [seconds]
set -eu

app=${1:-build/HuginMunin.app}
seconds=${2:-10}
executable="$app/Contents/MacOS/HuginMunin"

if [ ! -x "$executable" ]; then
    echo "No executable at $executable" >&2
    exit 1
fi

log=$(mktemp)
"$executable" >"$log" 2>&1 &
pid=$!
sleep "$seconds"

if ! kill -0 "$pid" 2>/dev/null; then
    status=0
    wait "$pid" || status=$?
    echo "The app exited within $seconds seconds with status $status" >&2
    cat "$log" >&2
    exit 1
fi

kill "$pid"
wait "$pid" || true
echo "The app stayed running for $seconds seconds"
