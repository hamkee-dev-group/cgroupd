#!/usr/bin/env bash


set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-oom.XXXXXX")"
SOCK="${OOM_SOCK:-$TMPBASE/cgroupd.sock}"
LOG="${OOM_LOG:-$TMPBASE/cgroupd.log}"
SUDO="${SUDO:-sudo}"

CGROUPD="$BUILD/cgroupd"
CGCTL="$BUILD/cgroupctl"
MEMHOG="$BUILD/memhog"

ctl() { $SUDO env CGROUPD_SOCKET="$SOCK" "$CGCTL" "$@"; }

cleanup() {
    [ -n "${PID:-}" ] && $SUDO kill -TERM "$PID" 2>/dev/null || true
    [ -n "${PID:-}" ] && wait "$PID" 2>/dev/null || true
    rm -f "$SOCK"
    rm -rf "$TMPBASE"
}
trap cleanup EXIT

echo "==> starting cgroupd"
$SUDO "$CGROUPD" -d -s "$SOCK" >"$LOG" 2>&1 &
PID=$!
sleep 0.4

echo "==> launching memhog with memory.max=32M while it tries to alloc 64MiB"
ctl run --id oom1 --memory-max 32M --memory-swap-max 0 --priority 50 -- \
    "$MEMHOG" 64 5


for i in 1 2 3 4 5 6 7 8 9 10; do
    state=$(ctl inspect oom1 | awk -F': ' '/^state:/{print $2; exit}')
    [[ "$state" == killed || "$state" == failed ]] && break
    sleep 0.5
done

echo "==> final state:"
ctl inspect oom1
ctl inspect oom1 | grep -E 'state: (killed|failed)' >/dev/null || {
    echo FAIL: expected killed/failed, got $state; exit 1; }

echo "==> shutdown"
ctl quit
wait "$PID" 2>/dev/null || true
PID=""
echo "OK oom"
