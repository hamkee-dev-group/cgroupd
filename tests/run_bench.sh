#!/usr/bin/env bash







set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
SOCK="${BENCH_SOCK:-/tmp/cgroupd-bench.sock}"
LOG="${BENCH_LOG:-/tmp/cgroupd-bench.log}"
SUDO="${SUDO:-}"             

CGROUPD="$BUILD/cgroupd"
CGCTL="$BUILD/cgroupctl"
MEMHOG="$BUILD/memhog"

N="${N:-200}"                
PRESSURE_SECS="${PRESSURE_SECS:-15}"

ctl() {
    if [ -n "$SUDO" ]; then
        $SUDO env CGROUPD_SOCKET="$SOCK" "$CGCTL" "$@"
    else
        CGROUPD_SOCKET="$SOCK" "$CGCTL" "$@"
    fi
}

cleanup() {
    [ -n "${PID:-}" ] && { $SUDO kill -TERM "$PID" 2>/dev/null || true; }
    [ -n "${PID:-}" ] && { wait "$PID" 2>/dev/null || true; }
    rm -f "$SOCK"
}
trap cleanup EXIT

echo "==> starting cgroupd"
$SUDO "$CGROUPD" -d -s "$SOCK" -k 5.0 >"$LOG" 2>&1 &
PID=$!

for i in 1 2 3 4 5 6 7 8 9 10; do
    grep -q 'cgroupd ready' "$LOG" && break
    sleep 0.1
done
ctl ping >/dev/null


echo
echo "==> single-job RUN -> spawned latency (10 samples)"
total_us=0
for i in $(seq 1 10); do
    start_ns=$(date +%s%N)
    ctl run --id "lat-$i" -- /bin/true >/dev/null
    end_ns=$(date +%s%N)
    us=$(( (end_ns - start_ns) / 1000 ))
    printf '  sample %2d: %6d us\n' "$i" "$us"
    total_us=$(( total_us + us ))
done
echo "  mean: $(( total_us / 10 )) us"


echo
echo "==> spawn throughput (N=$N /bin/true jobs)"
sweep_start=$(date +%s%N)
for i in $(seq 1 "$N"); do
    ctl run --id "tput-$i" -- /bin/true >/dev/null
done
sweep_end=$(date +%s%N)
ms=$(( (sweep_end - sweep_start) / 1000000 ))
[ "$ms" -gt 0 ] || ms=1
echo "  $N jobs in ${ms} ms = $(( N * 1000 / ms )) jobs/s"


sleep 0.5


echo
echo "==> time-to-react under memory pressure"

ROOT="$(grep -m1 'using cgroup root' "$LOG" | sed 's/.*: //')"
if [ -n "$SUDO" ] && [ -n "$ROOT" ]; then
    $SUDO bash -c "echo 64M > '$ROOT'/memory.high" 2>/dev/null || true
    $SUDO bash -c "echo 0   > '$ROOT'/memory.swap.max" 2>/dev/null || true
fi

ctl run --id react-low  --priority  5 --memory-max 96M -- "$MEMHOG" 48 "$PRESSURE_SECS" >/dev/null
react_t0_ns=$(date +%s%N)
ctl run --id react-high --priority 90 --memory-max 96M -- "$MEMHOG" 32 "$PRESSURE_SECS" >/dev/null


acted=""
deadline=$(( $(date +%s) + PRESSURE_SECS + 5 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -qE 'KILL victim|FREEZE|demote' "$LOG"; then
        acted="$(grep -m1 -E 'KILL victim|FREEZE|demote' "$LOG")"
        break
    fi
    sleep 0.05
done
react_t1_ns=$(date +%s%N)

if [ -n "$acted" ]; then
    react_ms=$(( (react_t1_ns - react_t0_ns) / 1000000 ))
    echo "  reacted in ${react_ms} ms"
    echo "  action: $acted"
else
    echo "  no backpressure action observed within $((PRESSURE_SECS+5))s"
fi


ctl kill react-low  >/dev/null 2>&1 || true
ctl kill react-high >/dev/null 2>&1 || true
sleep 0.3
ctl quit >/dev/null 2>&1 || true
wait "$PID" 2>/dev/null || true
PID=""

echo
echo "OK bench"
