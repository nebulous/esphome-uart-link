#!/usr/bin/env python3
"""Host runtime tests for esphome-uart-link.

Compiles each tests/host/*.yaml scenario with `esphome compile`, runs the
host firmware binary, and checks its behavior against plain Python socket
peers. One scenario per test; any failure exits non-zero.

Requires: esphome on PATH (2026.8 or newer). Uses only the stdlib.
"""

import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

HOST = "127.0.0.1"
HERE = Path(__file__).resolve().parent
BUILD_TIMEOUT = 900  # esphome compile, first run downloads toolchains
BOOT_WAIT = 10.0  # firmware: max seconds until it accepts/sends
RECV_TIMEOUT = 10.0


def log(msg):
    print(f"[run_tests] {msg}", flush=True)


def compile_firmware(yaml_path):
    """Compile and return the firmware binary path."""
    proc = subprocess.run(
        ["esphome", "compile", str(yaml_path)],
        capture_output=True, text=True, timeout=BUILD_TIMEOUT,
    )
    if proc.returncode != 0:
        log(f"compile FAILED for {yaml_path.name}:\n{proc.stdout[-2000:]}\n{proc.stderr[-2000:]}")
        raise RuntimeError(f"compile failed: {yaml_path.name}")
    m = re.search(r"Successfully compiled program to path '([^']+)'", proc.stdout)
    if m:
        return m.group(1)
    # Cached rebuilds do not reprint the path; derive it from the yaml's
    # esphome name: <yaml_dir>/.esphome/build/<name>/.pioenvs/<name>/program
    name = re.search(r"^\s*name:\s*(\S+)", yaml_path.read_text(), re.MULTILINE).group(1)
    binary = yaml_path.parent / ".esphome" / "build" / name / ".pioenvs" / name / "program"
    if not binary.exists():
        raise RuntimeError(f"built binary not found at {binary}")
    return str(binary)


def recv_exact(sock, n, timeout=RECV_TIMEOUT):
    """Receive exactly n bytes or raise TimeoutError."""
    sock.settimeout(timeout)
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("peer closed early")
        data += chunk
    return data


def recv_until_closed(sock, timeout):
    """Wait for the server to close; raise TimeoutError if it does not."""
    sock.settimeout(timeout)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if sock.recv(1) == b"":
                return  # closed
        except socket.timeout:
            continue
    raise TimeoutError("connection stayed open")


def connect_with_retry(port, timeout=BOOT_WAIT):
    """Connect to a firmware-side listener, retrying while it boots."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            s = socket.create_connection((HOST, port), timeout=1.0)
            return s
        except OSError as e:
            last = e
            time.sleep(0.1)
    raise TimeoutError(f"could not connect to firmware port {port}: {last}")


class Firmware:
    """Run a host firmware binary; kill on exit."""

    def __init__(self, binary):
        self.proc = subprocess.Popen(
            [binary], stdout=open(f"/tmp/{Path(binary).stem}.log", "w"),
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.3)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def run_scenario(name, yaml_name, peer_fn):
    yaml_path = HERE / yaml_name
    binary = compile_firmware(yaml_path)
    log(f"{name}: firmware built")
    result = {}

    def peer_thread():
        try:
            peer_fn(result)
            result["ok"] = True
        except Exception as e:  # noqa: BLE001 - report any failure
            result["error"] = f"{type(e).__name__}: {e}"

    t = threading.Thread(target=peer_thread, daemon=True)
    t.start()
    fw = Firmware(binary)
    try:
        t.join(timeout=60)
    finally:
        fw.stop()

    if t.is_alive():
        log(f"{name}: FAIL (peer hung)")
        return False
    if result.get("ok"):
        log(f"{name}: PASS")
        return True
    log(f"{name}: FAIL ({result.get('error')})")
    return False


# ---- peers -----------------------------------------------------------------

def peer_client_echo(result):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, 44601))
    srv.listen(1)
    srv.settimeout(BOOT_WAIT)
    conn, _ = srv.accept()
    conn.sendall(b"ping")
    got = recv_exact(conn, 4)
    assert got == b"ping", f"echo mismatch: {got!r}"
    conn.close()
    srv.close()


def peer_server_fanout(result):
    a = connect_with_retry(44602)
    b = connect_with_retry(44602)
    got_a = recv_exact(a, 4)
    got_b = recv_exact(b, 4)
    assert got_a == b"tick", f"peer A got {got_a!r}"
    assert got_b == b"tick", f"peer B got {got_b!r}"
    a.close()
    b.close()


def peer_server_exclusive(result):
    a = connect_with_retry(44603)
    got = recv_exact(a, 2)
    assert got == b"ex", f"first client got {got!r}"
    b = connect_with_retry(44603)
    # First client must be evicted (server closes it).
    try:
        a.settimeout(RECV_TIMEOUT)
        deadline = time.time() + 5.0
        closed = False
        while time.time() < deadline:
            try:
                if a.recv(1) == b"":
                    closed = True
                    break
            except socket.timeout:
                continue
        assert closed, "first client was not evicted"
    finally:
        a.close()
    got = recv_exact(b, 2)
    assert got == b"ex", f"second client got {got!r}"
    b.close()


def peer_server_idle(result):
    c = connect_with_retry(44604)
    c.sendall(b"x")  # arm the idle timer (traffic in either direction)
    recv_until_closed(c, timeout=4.0)
    c.close()


def peer_reconnect(result):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, 44607))
    srv.listen(1)
    srv.settimeout(BOOT_WAIT)
    conn, _ = srv.accept()
    conn.sendall(b"one")
    got = recv_exact(conn, 3)
    assert got == b"one", f"first roundtrip: {got!r}"
    conn.close()  # kill the link under the firmware
    srv.settimeout(BOOT_WAIT + 5.0)
    conn2, _ = srv.accept()  # firmware must reconnect
    conn2.sendall(b"two")
    got = recv_exact(conn2, 3)
    assert got == b"two", f"second roundtrip: {got!r}"
    conn2.close()
    srv.close()


def peer_bridge(result):
    """Two Python listeners; the firmware bridges its two outbound clients."""
    def listener(port):
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((HOST, port))
        srv.listen(1)
        srv.settimeout(BOOT_WAIT + 5.0)
        return srv.accept()[0]

    a = listener(44605)
    b = listener(44606)
    a.sendall(b"hello")
    got = recv_exact(b, 5)
    assert got == b"hello", f"a->b: {got!r}"
    b.sendall(b"world")
    got = recv_exact(a, 5)
    assert got == b"world", f"b->a: {got!r}"
    a.close()
    b.close()


SCENARIOS = [
    # Server-side scenarios (fanout, exclusive, idle timeout) are blocked on
    # host: async_tcp's socket fallback has no AsyncServer. See
    # tests/README.md and private/QUESTIONS.md Q3.
    ("client_echo", "client_echo.yaml", peer_client_echo),
    ("reconnect", "reconnect.yaml", peer_reconnect),
    ("bridge_e2e", "bridge_e2e.yaml", peer_bridge),
]


def main():
    only = sys.argv[1:] if len(sys.argv) > 1 else None
    results = []
    for name, yaml_name, peer in SCENARIOS:
        if only and name not in only:
            continue
        results.append((name, run_scenario(name, yaml_name, peer)))
    failed = [n for n, ok in results if not ok]
    print()
    for name, ok in results:
        log(f"{'PASS' if ok else 'FAIL'}  {name}")
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
