#!/usr/bin/env bash

set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-smoke.XXXXXX")"
SOCK="${SMOKE_SOCK:-$TMPBASE/cgroupd.sock}"
ROOT="${SMOKE_ROOT:-}"   
LOG="${SMOKE_LOG:-$TMPBASE/cgroupd.log}"
SUDO="${SUDO:-}"         

CGROUPD="$BUILD/cgroupd"
CGCTL="$BUILD/cgroupctl"

ctl() {
    if [ -n "$SUDO" ]; then
        $SUDO env CGROUPD_SOCKET="$SOCK" "$CGCTL" "$@"
    else
        CGROUPD_SOCKET="$SOCK" "$CGCTL" "$@"
    fi
}

cleanup() {
    [ -n "${PID:-}" ] && kill -TERM "$PID" 2>/dev/null || true
    [ -n "${PID:-}" ] && wait "$PID" 2>/dev/null || true
    rm -f "$SOCK"
    rm -rf "$TMPBASE"
}
trap cleanup EXIT

echo "==> starting cgroupd (sock=$SOCK)"
ARGS=(-d -s "$SOCK")
[ -n "$ROOT" ] && ARGS+=(-r "$ROOT")
$SUDO "$CGROUPD" "${ARGS[@]}" >"$LOG" 2>&1 &
PID=$!
sleep 0.4
if ! kill -0 "$PID" 2>/dev/null; then
    echo "daemon failed to start"
    cat "$LOG"; exit 1
fi
export CGROUPD_SOCKET="$SOCK"

echo "==> ping"
ctl ping

echo "==> run a quick job"
ctl run --id smoke1 --memory-max 64M --cpu-weight 50 \
    --priority 50 -- /bin/sh -c 'echo hello-from-job; exit 0'
sleep 0.4

echo "==> list"
ctl list

echo "==> stats"
ctl stats

echo "==> run a long job we'll kill"
ctl run --id smoke2 --memory-max 32M --priority 10 -- \
    /bin/sh -c 'sleep 30'
sleep 0.3
ctl inspect smoke2

echo "==> freeze and thaw smoke2"
ctl freeze smoke2
sleep 0.2
ctl inspect smoke2 | grep -q 'state: frozen' || { echo FAIL freeze; exit 1; }
ctl thaw smoke2
sleep 0.2
ctl inspect smoke2 | grep -q 'state: running' || { echo FAIL thaw; exit 1; }

echo "==> kill smoke2"
ctl kill smoke2
sleep 0.5
ctl inspect smoke2 | grep -E 'state: (killed|exited|failed)' >/dev/null || \
    { echo FAIL kill; exit 1; }

echo "==> remove smoke1"
ctl remove smoke1 || true

echo "==> stats after"
ctl stats

echo "==> shutdown"
ctl quit
wait "$PID" 2>/dev/null || true
PID=""

echo "OK smoke"
