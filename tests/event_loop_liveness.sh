#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD="$HERE/build"
TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/cgroupd-loop.XXXXXX")"
LOG="$TMPBASE/cgroupd.log"

cleanup() {
    rm -rf "$TMPBASE"
}
trap cleanup EXIT

# The real daemon runs unprivileged: an empty scratch directory stands in for
# the cgroup root, so every cgroup/PSI operation fails soft and only the
# accept/dispatch loop is exercised.
python3 - "$BUILD/cgroupd" "$BUILD/cgroupctl" "$TMPBASE" "$LOG" <<'PY' || { cat "$LOG"; exit 1; }
import os
import signal
import socket
import subprocess
import sys
import threading
import time

cgroupd, cgctl, tmp, log_path = sys.argv[1:]
sock_path = os.path.join(tmp, "cgroupd.sock")

PONG = b"STATUS: ok\npong: 1\n\n"
TIMED_OUT = b"STATUS: err\nreason: request timeout\n\n"
REPLY_BOUND = 1.0            # a healthy loop answers PING in milliseconds
DEADLINE_MIN = 4.0           # REQ_DEADLINE_MS is 5000, swept on the 1 s tick
DEADLINE_MAX = 10.0
SHUTDOWN_BOUND = 5.0         # the shutdown path itself spends ~2.5 s reaping


def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    return s


def read_to_eof(s, timeout):
    s.settimeout(timeout)
    out = b""
    while True:
        try:
            data = s.recv(4096)
        except ConnectionResetError:
            # the daemon closed with one of our trickled bytes still unread
            return out
        if not data:
            return out
        out += data


def exchange(chunks, half_close=False):
    start = time.monotonic()
    with connect() as s:
        for i, chunk in enumerate(chunks):
            if i:
                time.sleep(0.1)
            s.sendall(chunk)
        if half_close:
            s.shutdown(socket.SHUT_WR)
        reply = read_to_eof(s, REPLY_BOUND)
    return reply, time.monotonic() - start


def expect_pong(what, chunks, half_close=False):
    reply, took = exchange(chunks, half_close)
    assert reply == PONG, f"{what}: got {reply!r}"
    bound = REPLY_BOUND + 0.1 * (len(chunks) - 1)
    assert took < bound, f"{what}: took {took:.2f}s"


def open_stalled():
    s = connect()
    s.sendall(b"PING\n")
    return s


def open_trickling():
    s = connect()
    s.sendall(b"PING\n")

    def feed():
        try:
            while True:
                time.sleep(0.1)
                s.sendall(b"x")
        except OSError:
            pass

    t = threading.Thread(target=feed, daemon=True)
    t.start()
    return s, t


def fd_count():
    return len(os.listdir(f"/proc/{daemon.pid}/fd"))


with open(log_path, "wb") as log:
    daemon = subprocess.Popen(
        [cgroupd, "-r", os.path.join(tmp, "cgroot"), "-s", sock_path],
        stdout=log, stderr=log)
try:
    for _ in range(100):
        assert daemon.poll() is None, "daemon exited during startup"
        try:
            if exchange([b"PING\n\n"])[0] == PONG:
                break
        except (FileNotFoundError, ConnectionRefusedError):
            pass
        time.sleep(0.05)
    else:
        raise AssertionError("daemon did not become ready")
    baseline_fds = fd_count()

    # A sends "PING\n" and stays connected; another A keeps its unterminated
    # request growing by one byte every 100 ms.
    opened = time.monotonic()
    stalled = open_stalled()
    trickling, feeder = open_trickling()

    expect_pong("PING behind a stalled sender", [b"PING\n\n"])
    time.sleep(0.35)
    expect_pong("PING behind a trickling sender", [b"PING\n\n"])
    expect_pong("fragmented PING", [b"PI", b"NG\n", b"\n"])
    expect_pong("EOF-terminated PING", [b"PING\n"], half_close=True)
    subprocess.run([cgctl, "ping"], check=True, timeout=REPLY_BOUND,
                   stdout=subprocess.DEVNULL,
                   env=dict(os.environ, CGROUPD_SOCKET=sock_path))

    # The deadline covers the whole request: steady progress must not extend it.
    for name, s in (("stalled", stalled), ("trickling", trickling)):
        reply = read_to_eof(s, DEADLINE_MAX)
        closed_after = time.monotonic() - opened
        assert reply == TIMED_OUT, f"{name} sender: got {reply!r}"
        assert DEADLINE_MIN <= closed_after <= DEADLINE_MAX, \
            f"{name} sender closed after {closed_after:.2f}s"
    feeder.join(1.0)
    assert not feeder.is_alive(), "trickling sender was never disconnected"
    stalled.close()
    trickling.close()
    assert fd_count() == baseline_fds, \
        f"daemon fds: {fd_count()} != {baseline_fds}"

    # SIGTERM must be consumed while both kinds of sender are still connected.
    stalled = open_stalled()
    trickling, feeder = open_trickling()
    time.sleep(0.35)
    daemon.send_signal(signal.SIGTERM)
    rc = daemon.wait(SHUTDOWN_BOUND)
    assert rc == 0, f"daemon exit status {rc}"
finally:
    if daemon.poll() is None:
        daemon.kill()
        daemon.wait()
PY

echo "OK event_loop_liveness"
