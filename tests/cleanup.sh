#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-cleanup.XXXXXX")"
SOCK="${CLEANUP_SOCK:-$TMPBASE/cgroupd.sock}"
LOG="${CLEANUP_LOG:-$TMPBASE/cgroupd.log}"
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

remove_eventually() {
    local id="$1"
    local rc=1
    for _ in $(seq 1 30); do
        if ctl remove "$id" >/dev/null 2>&1; then
            rc=0
            break
        fi
        sleep 0.2
    done
    [ "$rc" -eq 0 ] || ctl remove "$id"
}

inspect_missing() {
    local id="$1"
    set +e
    ctl inspect "$id" >/dev/null 2>&1
    local rc=$?
    set -e
    [ "$rc" -ne 0 ]
}

echo "==> starting cgroupd"
$SUDO "$CGROUPD" -d -s "$SOCK" >"$LOG" 2>&1 &
PID=$!
sleep 0.4

echo "==> exited job can be removed after wait"
ctl run --id rm-exit -- /bin/sh -c 'exit 0'
ctl wait rm-exit >/dev/null
remove_eventually rm-exit
inspect_missing rm-exit

echo "==> killed job can be removed after wait"
ctl run --id rm-kill -- /bin/sh -c 'sleep 30'
sleep 0.3
ctl kill rm-kill >/dev/null
set +e
ctl wait rm-kill >/dev/null
kill_rc=$?
set -e
[ "$kill_rc" -eq 137 ] || { echo "FAIL kill wait rc=$kill_rc"; exit 1; }
remove_eventually rm-kill
inspect_missing rm-kill

echo "==> frozen running job rejects remove until it is stopped"
ctl run --id rm-freeze -- /bin/sh -c 'sleep 30'
sleep 0.3
ctl freeze rm-freeze >/dev/null
set +e
freeze_remove_out="$(ctl remove rm-freeze 2>&1)"
freeze_remove_rc=$?
set -e
[ "$freeze_remove_rc" -ne 0 ] || { echo "FAIL frozen remove unexpectedly succeeded"; exit 1; }
printf '%s\n' "$freeze_remove_out" | grep -q 'reason: still running'
ctl thaw rm-freeze >/dev/null
ctl kill rm-freeze >/dev/null
set +e
ctl wait rm-freeze >/dev/null
freeze_wait_rc=$?
set -e
[ "$freeze_wait_rc" -eq 137 ] || { echo "FAIL frozen kill wait rc=$freeze_wait_rc"; exit 1; }
remove_eventually rm-freeze
inspect_missing rm-freeze

echo "==> oom-killed job can be removed after wait"
ctl run --id rm-oom --memory-max 32M --memory-swap-max 0 -- "$MEMHOG" 64 5
for _ in $(seq 1 20); do
    state="$(ctl inspect rm-oom | awk -F': ' '/^state:/{print $2; exit}')"
    [ "$state" = "killed" ] || [ "$state" = "failed" ] || [ "$state" = "exited" ] || {
        sleep 0.5
        continue
    }
    break
done
set +e
oom_wait_out="$(ctl wait rm-oom 2>&1)"
oom_wait_rc=$?
set -e
printf '%s\n' "$oom_wait_out"
[ "$oom_wait_rc" -ne 0 ] || { echo "FAIL expected non-zero wait for OOM"; exit 1; }
printf '%s\n' "$oom_wait_out" | grep -q '^oom_killed: 1$'
remove_eventually rm-oom
inspect_missing rm-oom

echo "==> daemon emitted structured cleanup + oom events"
grep -q 'EVENT schema=cgroupd.v1 .*type=job.cleanup id=rm-exit ' "$LOG"
grep -q 'EVENT schema=cgroupd.v1 .*type=job.cleanup id=rm-kill ' "$LOG"
grep -q 'EVENT schema=cgroupd.v1 .*type=job.cleanup id=rm-freeze ' "$LOG"
grep -q 'EVENT schema=cgroupd.v1 .*type=job.cleanup id=rm-oom ' "$LOG"
grep -q 'EVENT schema=cgroupd.v1 .*type=job.oom id=rm-oom ' "$LOG"

echo "==> shutdown"
ctl quit >/dev/null
wait "$PID" 2>/dev/null || true
PID=""

echo "OK cleanup"
