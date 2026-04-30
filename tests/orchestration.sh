#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-orch.XXXXXX")"
SOCK="${ORCH_SOCK:-$TMPBASE/cgroupd.sock}"
LOG="${ORCH_LOG:-$TMPBASE/cgroupd.log}"
LOGDIR="$TMPBASE/joblogs"
SUDO="${SUDO:-sudo}"

CGROUPD="$BUILD/cgroupd"
CGCTL="$BUILD/cgroupctl"
WRAPPER="$HERE/tests/wrapper.sh"

ctl() { $SUDO env CGROUPD_SOCKET="$SOCK" "$CGCTL" "$@"; }

remove_eventually() {
    local id="$1"
    for _ in $(seq 1 30); do
        if ctl remove "$id" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    ctl remove "$id"
}

cleanup() {
    [ -n "${PID:-}" ] && $SUDO kill -TERM "$PID" 2>/dev/null || true
    [ -n "${PID:-}" ] && wait "$PID" 2>/dev/null || true
    rm -f "$SOCK"
    rm -rf "$TMPBASE"
}
trap cleanup EXIT

SERVICE_CMD="echo sidecar-ready; trap 'exit 0' TERM INT; while :; do sleep 1; done"

echo "==> starting cgroupd with per-job logging"
$SUDO "$CGROUPD" -d -s "$SOCK" -L "$LOGDIR" >"$LOG" 2>&1 &
PID=$!
sleep 0.4

echo "==> run with require-path, service, and io.max wiring"
ctl run --id svc23 \
    --require-path "$WRAPPER" \
    --io-max "8:0 rbps=1048576" \
    --service "$SERVICE_CMD" \
    -- /bin/sh -c 'sleep 1; exit 23'
sleep 0.4

inspect_out="$(ctl inspect svc23)"
printf '%s\n' "$inspect_out"
printf '%s\n' "$inspect_out" | grep -q '^service_count: 1$'
printf '%s\n' "$inspect_out" | grep -q '^require_path_count: 1$'
printf '%s\n' "$inspect_out" | grep -q '^io_max_rule_count: 1$'
printf '%s\n' "$inspect_out" | grep -E '^pid_count: [2-9][0-9]*$' >/dev/null

echo "==> workload exit status still propagates through service wrapper"
set +e
wait_out="$(ctl wait svc23 2>&1)"
wait_rc=$?
set -e
printf '%s\n' "$wait_out"
[ "$wait_rc" -eq 23 ] || { echo "FAIL wrapper wait rc=$wait_rc"; exit 1; }
printf '%s\n' "$wait_out" | grep -q '^exit: 23$'

echo "==> logs contain sidecar output"
ctl logs svc23 | grep -q 'sidecar-ready'

echo "==> missing require-path is rejected before spawn"
set +e
missing_out="$(ctl run --id missing-preflight --require-path "$TMPBASE/no-such-file" -- /bin/true 2>&1)"
missing_rc=$?
set -e
printf '%s\n' "$missing_out"
[ "$missing_rc" -ne 0 ] || { echo "FAIL expected preflight rejection"; exit 1; }
printf '%s\n' "$missing_out" | grep -q "^reason: missing path: $TMPBASE/no-such-file$"

echo "==> cleanup"
remove_eventually svc23
ctl quit
wait "$PID" 2>/dev/null || true
PID=""

echo "OK orchestration"
