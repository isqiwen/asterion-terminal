"""SIGTERM stops a managed service within the Agent's 500 ms escalation window.

The service must exit 0 on its own and remove its local socket, rather than
being killed with SIGKILL. The service runtime supports macOS and Linux.
"""
from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
import time

# Sanitizer builds run several times slower.
SCALE = float(os.environ.get("ASTERION_TIMING_SCALE", "1"))

kind, executable = sys.argv[1:3]
trader_sdk = sys.argv[3] if len(sys.argv) > 3 else ""
with tempfile.TemporaryDirectory(prefix="ast-stop-", dir="/tmp", ignore_cleanup_errors=True) as folder:
    root = Path(folder)
    (root / "data").mkdir()
    endpoint = root / "service.sock"
    args = {
        "trading": ["--session", "stop", "--directory", str(root / "data"),
                    "--ctp-library", trader_sdk],
        "market": ["--session", "stop", "--directory", str(root / "data")],
        "task": ["--session", "stop", "--directory", str(root / "data"),
                 "--data-instance", "history", "--data-endpoint", str(root / "history.sock")],
    }[kind]
    if kind == "trading":
        args += ["--health-endpoint", str(root / "health.sock")]
    process = subprocess.Popen([executable, *args, "--endpoint", str(endpoint)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 10
        while not endpoint.exists():
            assert process.poll() is None, process.stderr.read().decode(errors="replace")
            assert time.monotonic() < deadline, "service never listened"
            time.sleep(.02)
        started = time.monotonic()
        process.send_signal(signal.SIGTERM)
        code = process.wait(timeout=5)
        elapsed = time.monotonic() - started
        assert code == 0, f"exit code {code}"
        assert elapsed < .5 * SCALE, f"graceful stop took {elapsed:.2f}s"
        assert not endpoint.exists(), "local socket left behind"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    if kind == "trading":
        # The I/O coordinator also owns parent liveness; no separate watchdog thread.
        parent_code = """
import os, subprocess, sys
child = subprocess.Popen([*sys.argv[1:], '--owner-pid', str(os.getpid())],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(child.pid, flush=True)
sys.stdin.buffer.read(1)
os._exit(0)
"""
        parent = subprocess.Popen([sys.executable, "-c", parent_code, executable, *args,
                                   "--endpoint", str(endpoint)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        child_pid = int(parent.stdout.readline())
        orphan_stopped = False
        try:
            deadline = time.monotonic() + 5
            while not (endpoint.exists() and (root / "health.sock").exists()):
                assert time.monotonic() < deadline, "managed service never listened"
                time.sleep(.01)
            parent.stdin.close()
            parent.wait(timeout=2)
            deadline = time.monotonic() + 2 * SCALE
            while endpoint.exists() or (root / "health.sock").exists():
                assert time.monotonic() < deadline, "service did not stop after its parent exited"
                time.sleep(.01)
            orphan_stopped = True
        finally:
            if parent.poll() is None:
                parent.kill()
                parent.wait()
            if not orphan_stopped:
                try:
                    os.kill(child_pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
print(f"{kind} stopped gracefully on SIGTERM" +
      (" and parent loss" if kind == "trading" else ""))
