"""SIGTERM stops a managed service within the Agent's 500 ms escalation window.

The service must exit 0 on its own and remove its local socket, rather than
being killed with SIGKILL. POSIX only; Windows services stop via console
control events and are covered by the native service acceptance.
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

if os.name == "nt":
    print("POSIX signal semantics only")
    sys.exit(0)
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
        "task": ["--session", "stop", "--directory", str(root / "data")],
    }[kind]
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
print(f"{kind} stopped gracefully on SIGTERM")
