#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-wait.XXXXXX")"
SOCK="${WAIT_SOCK:-$TMPBASE/cgroupd.sock}"
LOG="${WAIT_LOG:-$TMPBASE/cgroupd.log}"
SUDO="${SUDO:-}"

CGROUPD="$BUILD/cgroupd"
CGCTL="$BUILD/cgroupctl"
WRAPPER="$HERE/tests/wrapper.sh"

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

echo "==> starting cgroupd"
$SUDO "$CGROUPD" -d -s "$SOCK" >"$LOG" 2>&1 &
PID=$!
sleep 0.4
export CGROUPD_SOCKET="$SOCK"

echo "==> verify wait returns shell exit code"
ctl run --id wait-exit7 -- /bin/sh -c 'exit 7'
set +e
wait_out="$(ctl wait wait-exit7 2>&1)"
wait_rc=$?
set -e
printf '%s\n' "$wait_out"
[ "$wait_rc" -eq 7 ] || { echo "FAIL wait exit rc=$wait_rc"; exit 1; }
printf '%s\n' "$wait_out" | grep -q '^exit: 7$'
printf '%s\n' "$wait_out" | grep -q '^signal: 0$'

echo "==> verify wrapper propagation through landlockd-style argv"
ctl run --id wait-wrap42 -- "$WRAPPER" run --policy-file /tmp/wait-policy.toml -- /bin/sh -c 'exit 42'
set +e
wrap_out="$(ctl wait wait-wrap42 2>&1)"
wrap_rc=$?
set -e
printf '%s\n' "$wrap_out"
[ "$wrap_rc" -eq 42 ] || { echo "FAIL wrapper wait rc=$wrap_rc"; exit 1; }
printf '%s\n' "$wrap_out" | grep -q '^exit: 42$'
printf '%s\n' "$wrap_out" | grep -q '^signal: 0$'

echo "==> inspect exposes attribution metadata"
inspect_out="$(ctl inspect wait-wrap42)"
printf '%s\n' "$inspect_out"
printf '%s\n' "$inspect_out" | grep -q '^start_unix_ms: [1-9]'
printf '%s\n' "$inspect_out" | grep -q '^exit_unix_ms: [1-9]'
printf '%s\n' "$inspect_out" | grep -q '^pid_count: '
printf '%s\n' "$inspect_out" | grep -q '^pids:'

echo "==> daemon emitted structured lifecycle events"
grep -q 'EVENT schema=cgroupd.v1 .*type=job.start id=wait-wrap42 ' "$LOG"
grep -q 'EVENT schema=cgroupd.v1 .*type=job.exit id=wait-wrap42 ' "$LOG"

echo "==> cleanup"
ctl remove wait-exit7
ctl remove wait-wrap42
ctl quit
wait "$PID" 2>/dev/null || true
PID=""

echo "OK wait"
