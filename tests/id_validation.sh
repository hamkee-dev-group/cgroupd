#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-idval.XXXXXX")"
SOCK="${IDVAL_SOCK:-$TMPBASE/cgroupd.sock}"
LOG="${IDVAL_LOG:-$TMPBASE/cgroupd.log}"
ROOT="${IDVAL_ROOT:-/sys/fs/cgroup/cgroupd-idval.slice}"
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
    $SUDO rmdir "$ROOT"/* 2>/dev/null || true
    $SUDO rmdir "$ROOT" 2>/dev/null || true
}
trap cleanup EXIT

echo "==> starting cgroupd"
$SUDO "$CGROUPD" -d -r "$ROOT" -s "$SOCK" >"$LOG" 2>&1 &
PID=$!
sleep 0.4
if ! $SUDO kill -0 "$PID" 2>/dev/null; then
    echo "daemon failed to start"; cat "$LOG"; exit 1
fi
export CGROUPD_SOCKET="$SOCK"

reject_id() {
    local label="$1"; shift
    local id="$1"; shift
    set +e
    local out
    out="$(ctl run --id "$id" -- /bin/true 2>&1)"
    local rc=$?
    set -e
    printf '==> --id %q rejected: rc=%d\n' "$id" "$rc"
    [ "$rc" -ne 0 ] || { echo "FAIL --id $id unexpectedly accepted"; exit 1; }
    printf '%s\n' "$out" | grep -q '^STATUS: ok' && {
        echo "FAIL --id $id was accepted by the daemon"
        printf '%s\n' "$out"
        exit 1
    }
    if printf '%s\n' "$out" | grep -q '^STATUS: err'; then
        printf '%s\n' "$out" | grep -q '^reason: id' || {
            echo "FAIL reason did not name id for --id $id"
            printf '%s\n' "$out"
            exit 1
        }
    fi
}

accept_id() {
    local id="$1"; shift
    ctl run --id "$id" -- /bin/true >/dev/null
    printf '==> --id %q accepted\n' "$id"
    ctl wait "$id" >/dev/null 2>&1 || true
    ctl remove "$id" >/dev/null 2>&1 || true
}

long_id="$(printf 'a%.0s' $(seq 1 64))"

for case in \
        'parent:../escape' \
        'nested:a/b' \
        'absolute:/etc' \
        'dot:.' \
        'dotdot:..' \
        'leading-dot:.hidden' \
        'leading-dash:-lead' \
        'newline:bad
id' \
        'space:bad id' \
        'semicolon:bad;id' \
        'dollar:bad$id' \
        'backtick:bad`id`' \
        'tab:bad	id'; do
    label="${case%%:*}"
    value="${case#*:}"
    reject_id "$label" "$value"
done

reject_id "too-long" "$long_id"

echo "==> escaped cgroup was not created"
if [ -d "$(dirname "$ROOT")/escape" ]; then
    echo "FAIL ../escape created a cgroup outside the root"
    exit 1
fi

for id in smoke1 smoke2 svc23 wait-exit7 rm-oom react-low a.b_c-9; do
    accept_id "$id"
done

echo "==> generated ids are accepted"
ctl run -- /bin/true >/dev/null
echo "OK id validation"
