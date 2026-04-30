#!/usr/bin/env bash
set -euo pipefail






JOB_ID="${JOB_ID:-demo-job}"
ROOT="${ROOT:-/run/jobs/$JOB_ID/root}"
POLICY="${POLICY:-/run/jobs/$JOB_ID/policy.toml}"
TARGET="${TARGET:-/usr/bin/job}"
MEMFDBUS_SERVICE="${MEMFDBUS_SERVICE:-}"
IOURINGD_SERVICE="${IOURINGD_SERVICE:-}"

overlayd prepare --id "$JOB_ID" --root "$ROOT"
sandbox --prepare-only --root "$ROOT" -- /usr/bin/true

RUN_ARGS=(
    --id "$JOB_ID"
    --memory-max 2G
    --memory-high 1536M
    --memory-swap-max 0
    --cpu-max 400000/1000000
    --cpu-weight 200
    --pids-max 256
    --require-path "$ROOT"
    --require-path "$ROOT$TARGET"
    --require-path "$POLICY"
)

[ -n "$MEMFDBUS_SERVICE" ] && RUN_ARGS+=(--service "$MEMFDBUS_SERVICE")
[ -n "$IOURINGD_SERVICE" ] && RUN_ARGS+=(--service "$IOURINGD_SERVICE")

cgroupctl run "${RUN_ARGS[@]}" -- \
    landlockd run \
        --policy-file "$POLICY" \
        -- "$TARGET"

cgroupctl wait "$JOB_ID"
