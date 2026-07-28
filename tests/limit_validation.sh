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

reject_weight() {
    local key="$1"; shift
    local flag="$1"; shift
    local label="$1"; shift
    local val="$1"; shift
    local id="${key//_/-}-bad-$label"
    set +e
    local out
    out="$(ctl run --id "$id" "$flag" "$val" -- /bin/true 2>&1)"
    local rc=$?
    set -e
    printf '==> %s %q rejected: rc=%d\n' "$flag" "$val" "$rc"
    [ "$rc" -ne 0 ] || { echo "FAIL $flag $val unexpectedly accepted"; exit 1; }
    printf '%s\n' "$out" | grep -q '^STATUS: err' || {
        echo "FAIL missing STATUS: err for $flag $val"
        printf '%s\n' "$out"
        exit 1
    }
    printf '%s\n' "$out" | grep -q "^reason: $key" || {
        echo "FAIL reason did not name $key for $flag $val"
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
        'garbage:foo' \
        'zero:0' \
        'too-high:10001' \
        'negative:-5'; do
    label="${case%%:*}"
    val="${case#*:}"
    reject_weight cpu_weight --cpu-weight "$label" "$val"
    reject_weight io_weight --io-weight "$label" "$val"
done

echo "==> daemon emitted no job.start for any rejected weight id"
if grep -E 'type=job\.start id=(cpu|io)-weight-bad-' "$LOG" >/dev/null; then
    echo "FAIL job.start event emitted for a rejected weight job"
    grep -E 'type=job\.start id=(cpu|io)-weight-bad-' "$LOG" || true
    exit 1
fi

ROOT_LINE="$(grep -E 'using cgroup root: ' "$LOG" | head -n1)"
DROOT="${ROOT_LINE##*using cgroup root: }"
if [ -n "$DROOT" ] && [ -d "$DROOT" ]; then
    for case in garbage zero too-high negative; do
        for key in cpu-weight io-weight; do
            id="$key-bad-$case"
            if [ -e "$DROOT/$id" ]; then
                echo "FAIL per-job cgroup directory exists for rejected $id: $DROOT/$id"
                exit 1
            fi
        done
    done
fi

accept_weight() {
    local key="$1"; shift
    local flag="$1"; shift
    local label="$1"; shift
    local val="$1"; shift
    local id="${key//_/-}-ok-$label"
    set +e
    local out
    out="$(ctl run --id "$id" "$flag" "$val" -- /bin/true 2>&1)"
    local rc=$?
    set -e
    printf '==> %s %q accepted: rc=%d\n' "$flag" "$val" "$rc"
    [ "$rc" -eq 0 ] || { echo "FAIL $flag $val unexpectedly rejected"; printf '%s\n' "$out"; exit 1; }
    printf '%s\n' "$out" | grep -q '^STATUS: ok' || {
        echo "FAIL missing STATUS: ok for $flag $val"
        printf '%s\n' "$out"
        exit 1
    }
}

accept_weight cpu_weight --cpu-weight min   '1'
accept_weight cpu_weight --cpu-weight mid   '100'
accept_weight cpu_weight --cpu-weight max   '10000'
accept_weight io_weight  --io-weight  min   '1'
accept_weight io_weight  --io-weight  mid   '100'
accept_weight io_weight  --io-weight  max   '10000'

reject_priority() {
    local label="$1"; shift
    local val="$1"; shift
    local id="priority-bad-$label"
    set +e
    local out
    out="$(ctl run --id "$id" --priority "$val" -- /bin/true 2>&1)"
    local rc=$?
    set -e
    printf '==> --priority %q rejected: rc=%d\n' "$val" "$rc"
    [ "$rc" -ne 0 ] || { echo "FAIL --priority $val unexpectedly accepted"; exit 1; }
    printf '%s\n' "$out" | grep -q '^STATUS: err' || {
        echo "FAIL missing STATUS: err for --priority $val"
        printf '%s\n' "$out"
        exit 1
    }
    printf '%s\n' "$out" | grep -q '^reason: priority' || {
        echo "FAIL reason did not name priority for --priority $val"
        printf '%s\n' "$out"
        exit 1
    }
    set +e
    ctl inspect "$id" >/dev/null 2>&1
    local inrc=$?
    set -e
    [ "$inrc" -ne 0 ] || { echo "FAIL job $id is inspectable after rejection"; exit 1; }
}

reject_priority garbage  'foo'
reject_priority too-high '101'
reject_priority negative '-1'
reject_priority empty    ''

echo "==> daemon emitted no job.start for any rejected priority id"
if grep -E 'type=job\.start id=priority-bad-' "$LOG" >/dev/null; then
    echo "FAIL job.start event emitted for a rejected priority job"
    grep -E 'type=job\.start id=priority-bad-' "$LOG" || true
    exit 1
fi

if [ -n "$DROOT" ] && [ -d "$DROOT" ]; then
    for case in garbage too-high negative empty; do
        id="priority-bad-$case"
        if [ -e "$DROOT/$id" ]; then
            echo "FAIL per-job cgroup directory exists for rejected $id: $DROOT/$id"
            exit 1
        fi
    done
fi

accept_priority() {
    local label="$1"; shift
    local val="$1"; shift
    local id="priority-ok-$label"
    set +e
    local out
    out="$(ctl run --id "$id" --priority "$val" -- /bin/true 2>&1)"
    local rc=$?
    set -e
    printf '==> --priority %q accepted: rc=%d\n' "$val" "$rc"
    [ "$rc" -eq 0 ] || { echo "FAIL --priority $val unexpectedly rejected"; printf '%s\n' "$out"; exit 1; }
    printf '%s\n' "$out" | grep -q '^STATUS: ok' || {
        echo "FAIL missing STATUS: ok for --priority $val"
        printf '%s\n' "$out"
        exit 1
    }
}

accept_priority min '0'
accept_priority mid '50'
accept_priority max '100'

echo "==> starting long-lived job for kill-signal validation"
ctl run --id kill-target -- /bin/sleep 300 >/dev/null
ctl inspect kill-target 2>/dev/null | grep -q '^state: running' || {
    echo "FAIL kill-target did not start running"; exit 1;
}

assert_running() {
    ctl inspect kill-target 2>/dev/null | grep -q '^state: running' || {
        echo "FAIL kill-target not running after $1"
        exit 1
    }
}

reject_signal() {
    local val="$1"; shift
    set +e
    local out
    out="$(ctl kill kill-target --signal "$val" 2>&1)"
    local rc=$?
    set -e
    printf '==> kill --signal %q rejected: rc=%d\n' "$val" "$rc"
    [ "$rc" -ne 0 ] || { echo "FAIL kill --signal $val unexpectedly accepted"; printf '%s\n' "$out"; exit 1; }
    printf '%s\n' "$out" | grep -q '^STATUS: err' || {
        echo "FAIL missing STATUS: err for kill --signal $val"
        printf '%s\n' "$out"
        exit 1
    }
    printf '%s\n' "$out" | grep -q '^reason: signal' || {
        echo "FAIL reason did not name signal for kill --signal $val"
        printf '%s\n' "$out"
        exit 1
    }
    assert_running "invalid kill --signal $val"
}

reject_signal 'abc'
reject_signal '0'
reject_signal '-1'
reject_signal '999999'
reject_signal 'foo'
reject_signal '999'

echo "==> kill rejects malformed signal option syntax"
set +e
ctl kill kill-target --signal >/dev/null 2>&1
[ $? -ne 0 ] || { echo "FAIL kill --signal without value accepted"; exit 1; }
ctl kill kill-target --signal 15 extra >/dev/null 2>&1
[ $? -ne 0 ] || { echo "FAIL kill --signal with trailing argument accepted"; exit 1; }
ctl kill kill-target bogus >/dev/null 2>&1
[ $? -ne 0 ] || { echo "FAIL kill with unexpected argument accepted"; exit 1; }
set -e
assert_running "malformed signal option syntax"

echo "==> valid kill --signal keeps working"
set +e
KOUT="$(ctl kill kill-target --signal 18 2>&1)"
KRC=$?
set -e
[ "$KRC" -eq 0 ] || { echo "FAIL kill --signal 18 unexpectedly rejected"; printf '%s\n' "$KOUT"; exit 1; }
printf '%s\n' "$KOUT" | grep -q '^STATUS: ok' || {
    echo "FAIL missing STATUS: ok for kill --signal 18"
    printf '%s\n' "$KOUT"
    exit 1
}
assert_running "valid kill --signal 18"

echo "==> shutdown"
ctl quit >/dev/null
wait "$PID" 2>/dev/null || true
PID=""

echo "OK limit_validation"
