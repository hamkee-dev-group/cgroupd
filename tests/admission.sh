#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-admission.XXXXXX")"
SOCK="${ADMISSION_SOCK:-$TMPBASE/cgroupd.sock}"
LOG="${ADMISSION_LOG:-$TMPBASE/cgroupd.log}"
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

echo "==> starting cgroupd with admission threshold forced to zero"
$SUDO "$CGROUPD" -d -s "$SOCK" --mem-admit-some-avg10 0 >"$LOG" 2>&1 &
PID=$!
sleep 0.4
export CGROUPD_SOCKET="$SOCK"

echo "==> run is rejected before spawn when admission is blocked"
set +e
reject_out="$(ctl run --id admit-block -- /bin/true 2>&1)"
reject_rc=$?
set -e
printf '%s\n' "$reject_out"
[ "$reject_rc" -ne 0 ] || { echo "FAIL expected admission rejection"; exit 1; }
printf '%s\n' "$reject_out" | grep -q '^reason: admission blocked: memory.some.avg10 '

echo "==> daemon emitted structured admission rejection pressure event"
grep -q 'EVENT schema=cgroupd.v1 .*type=pressure resource=memory .*action=admission_reject target=admit-block' "$LOG"

echo "==> shutdown"
ctl quit
wait "$PID" 2>/dev/null || true
PID=""

echo "OK admission"
