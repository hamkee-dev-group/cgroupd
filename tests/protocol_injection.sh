#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-proto.XXXXXX")"
SOCK="$TMPBASE/cgroupd.sock"
READY="$TMPBASE/ready"
CAPTURE="$TMPBASE/capture"
CGCTL="$BUILD/cgroupctl"

cleanup() {
    [ -n "${PID:-}" ] && kill -TERM "$PID" 2>/dev/null || true
    [ -n "${PID:-}" ] && wait "$PID" 2>/dev/null || true
    rm -rf "$TMPBASE"
}
trap cleanup EXIT

: >"$CAPTURE"
python3 - "$SOCK" "$READY" "$CAPTURE" <<'PY' &
import os
import socket
import sys
import time

sock_path, ready_path, capture_path = sys.argv[1:]
try:
    os.unlink(sock_path)
except FileNotFoundError:
    pass

srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(sock_path)
srv.listen(5)
srv.settimeout(0.1)
open(ready_path, "w").close()

end = time.time() + 2.0
with open(capture_path, "ab") as out:
    while time.time() < end:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        with conn:
            conn.settimeout(0.1)
            while True:
                try:
                    data = conn.recv(4096)
                except socket.timeout:
                    break
                if not data:
                    break
                out.write(data)
                out.flush()
PY
PID=$!

for _ in $(seq 1 50); do
    [ -e "$READY" ] && break
    sleep 0.02
done
[ -e "$READY" ] || { echo "server did not become ready"; exit 1; }

set +e
out="$(CGROUPD_SOCKET="$SOCK" "$CGCTL" run --id $'bad\nid: injected' -- /bin/true 2>&1)"
rc=$?
set -e
[ "$rc" -ne 0 ] || { echo "RUN id injection unexpectedly succeeded"; exit 1; }
printf '%s\n' "$out" | grep -q 'id must not contain CR or LF'

set +e
out="$(CGROUPD_SOCKET="$SOCK" "$CGCTL" run --env $'A=B\narg: /bin/false' -- /bin/true 2>&1)"
rc=$?
set -e
[ "$rc" -ne 0 ] || { echo "RUN env injection unexpectedly succeeded"; exit 1; }
printf '%s\n' "$out" | grep -q 'env must not contain CR or LF'

wait "$PID"
PID=""

[ ! -s "$CAPTURE" ] || { echo "client sent data for rejected RUN"; exit 1; }

READY2="$TMPBASE/ready-valid"
CAPTURE2="$TMPBASE/capture-valid"
: >"$CAPTURE2"
python3 - "$SOCK" "$READY2" "$CAPTURE2" <<'PY' &
import os
import socket
import sys

sock_path, ready_path, capture_path = sys.argv[1:]
try:
    os.unlink(sock_path)
except FileNotFoundError:
    pass

srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(sock_path)
srv.listen(1)
open(ready_path, "w").close()

conn, _ = srv.accept()
with conn, open(capture_path, "ab") as out:
    while True:
        data = conn.recv(4096)
        if not data:
            break
        out.write(data)
    conn.sendall(b"STATUS: ok\n\n")
PY
PID=$!

for _ in $(seq 1 50); do
    [ -e "$READY2" ] && break
    sleep 0.02
done
[ -e "$READY2" ] || { echo "valid server did not become ready"; exit 1; }

long_arg="$(python3 - <<'PY'
print("x" * 20000)
PY
)"
CGROUPD_SOCKET="$SOCK" "$CGCTL" run --id valid --env "A=B C" -- \
    /bin/sh -c 'echo hello-from-job; exit 0' "$long_arg" >/dev/null
wait "$PID"
PID=""

grep -q '^env: A=B C$' "$CAPTURE2"
grep -q '^arg: echo hello-from-job; exit 0$' "$CAPTURE2"
[ "$(wc -c <"$CAPTURE2")" -gt 20000 ] || { echo "large valid RUN was not captured"; exit 1; }

echo "OK protocol_injection"
