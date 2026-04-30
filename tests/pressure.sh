#!/usr/bin/env bash



set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-pressure.XXXXXX")"
SOCK="${PR_SOCK:-$TMPBASE/cgroupd.sock}"
LOG="${PR_LOG:-$TMPBASE/cgroupd.log}"
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


echo "==> starting cgroupd (mem-kill-avg10=5)"
$SUDO "$CGROUPD" -d -s "$SOCK" -k 5.0 >"$LOG" 2>&1 &
PID=$!
sleep 0.4




ROOT="$(grep -m1 'using cgroup root' "$LOG" | sed 's/.*: //')"
echo "==> root: $ROOT"
$SUDO bash -c "echo 32M > '$ROOT'/memory.high"
$SUDO bash -c "echo 0 > '$ROOT'/memory.swap.max" || true

echo "==> launch low-prio memhog (priority=10) and high-prio (priority=90)"
ctl run --id low  --priority 10 --memory-max 64M -- "$MEMHOG" 32 30 &
LOW=$!
sleep 0.3
ctl run --id high --priority 90 --memory-max 64M -- "$MEMHOG" 24 30 &
HIGH=$!


wait "$LOW" "$HIGH" || true


echo "==> watching for backpressure action (up to 12s)..."
acted=0
for i in $(seq 1 24); do
    sleep 0.5
    if grep -qE 'FREEZE|KILL victim' "$LOG"; then
        acted=1
        break
    fi
done

echo "==> daemon log tail:"
tail -25 "$LOG" || true

echo "==> stats:"
ctl stats || true

echo "==> list:"
ctl list || true

if [ "$acted" = "1" ]; then
    echo "OK pressure: backpressure action observed"
else
    echo "WARN pressure: no backpressure action observed in window"
    
    
    grep -E 'Killed|reaped' "$LOG" || true
fi

echo "==> shutdown"
ctl kill low || true
ctl kill high || true
sleep 0.3
ctl quit
wait "$PID" 2>/dev/null || true
PID=""
