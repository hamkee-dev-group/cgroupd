#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-limval.XXXXXX")"
SOCK="${LIMVAL_SOCK:-$TMPBASE/cgroupd.sock}"
LOG="${LIMVAL_LOG:-$TMPBASE/cgroupd.log}"
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
if ! $SUDO kill -0 "$PID" 2>/dev/null; then
    echo "daemon failed to start"; cat "$LOG"; exit 1
fi
export CGROUPD_SOCKET="$SOCK"

reject_cpu_max() {
    local label="$1"; shift
    local val="$1"; shift
    local id="cpu-max-bad-$label"
    set +e
    local out
    out="$(ctl run --id "$id" --cpu-max "$val" -- /bin/true 2>&1)"
    local rc=$?
    set -e
    printf '==> --cpu-max %q rejected: rc=%d\n' "$val" "$rc"
    [ "$rc" -ne 0 ] || { echo "FAIL --cpu-max $val unexpectedly accepted"; exit 1; }
    printf '%s\n' "$out" | grep -q '^STATUS: err' || {
        echo "FAIL missing STATUS: err for --cpu-max $val"
        printf '%s\n' "$out"
        exit 1
    }
    printf '%s\n' "$out" | grep -q '^reason: cpu_max' || {
        echo "FAIL reason did not name cpu_max for --cpu-max $val"
        printf '%s\n' "$out"
        exit 1
    }
    set +e
    ctl inspect "$id" >/dev/null 2>&1
    local inrc=$?
    set -e
    [ "$inrc" -ne 0 ] || { echo "FAIL job $id is inspectable after rejection"; exit 1; }
}

for case in \
        'abc:abc' \
        'bare:100000' \
        'qzero-period:100/0' \
        'zero-quota:0/100000' \
        'missing-period:100000/' \
        'missing-quota:/100000' \
        'too-many:1/2/3' \
        'negative:-1 100000' \
        'space-period-zero:100000 0' \
        'whitespace: '; do
    label="${case%%:*}"
    val="${case#*:}"
    reject_cpu_max "$label" "$val"
done

echo "==> daemon emitted no job.start for any rejected cpu_max id"
if grep -E 'type=job\.start id=cpu-max-bad-' "$LOG" >/dev/null; then
    echo "FAIL job.start event emitted for a rejected cpu_max job"
    grep -E 'type=job\.start id=cpu-max-bad-' "$LOG" || true
    exit 1
fi

accept_cpu_max() {
    local label="$1"; shift
    local val="$1"; shift
    local id="cpu-max-ok-$label"
    set +e
    local out
    out="$(ctl run --id "$id" --cpu-max "$val" -- /bin/true 2>&1)"
    local rc=$?
    set -e
    printf '==> --cpu-max %q accepted: rc=%d\n' "$val" "$rc"
    [ "$rc" -eq 0 ] || { echo "FAIL --cpu-max $val unexpectedly rejected"; printf '%s\n' "$out"; exit 1; }
    printf '%s\n' "$out" | grep -q '^STATUS: ok' || {
        echo "FAIL missing STATUS: ok for --cpu-max $val"
        printf '%s\n' "$out"
        exit 1
    }
}

accept_cpu_max max-bare 'max'
accept_cpu_max max-period 'max 100000'
accept_cpu_max ratio '50000/100000'
accept_cpu_max space '50000 100000'

echo "==> shutdown"
ctl quit >/dev/null
wait "$PID" 2>/dev/null || true
PID=""

echo "OK limit_validation"
