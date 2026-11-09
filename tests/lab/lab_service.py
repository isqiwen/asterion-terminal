"""The Terminal starts, supervises and stops the Python lab service.

Runs under support/isolated_node.py with ASTERION_PYTHON naming the prepared
runtime's interpreter. The Terminal starts the lab as its child, probes its
health and restarts it after it is killed. The lab exits with the Terminal.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def launch():
    return subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, encoding="utf-8")


def call(process, method, params=None):
    process.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n")
    process.stdin.flush()
    response = json.loads(process.stdout.readline())
    assert "error" not in response, response
    return response["result"]


def lab(process):
    return call(process, "runtime.snapshot")["lab"]


def wait(process, predicate, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        service = lab(process)
        if service and predicate(service):
            return service
        time.sleep(.1)
    raise AssertionError(f"lab service did not reach the expected state: {lab(process)}")


def gone(pid, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        time.sleep(.1)
    return False


def close(process):
    process.kill()
    process.wait(timeout=10)


root = Path(os.environ["ASTERION_ROOT"])
ready = lambda s: s["state"] == "ready" and s["pid"] > 0
process = launch()
try:
    call(process, "lab.endpoint")
    first = wait(process, ready)
    assert first["restarts"] == 0, first
    # The lab logs in its own installation, <root>/lab/logs.
    logs = root / "lab" / "logs"
    started = [line for path in logs.glob(f"asterion_lab-{first['pid']}_*.log")
               for line in path.read_text().splitlines()]
    assert any("[asterion_lab] [__main__.py:" in line and " lab-service service.started " in line for line in started), started

    # A process that dies is started again.
    os.kill(first["pid"], signal.SIGKILL)
    second = wait(process, lambda s: ready(s) and s["pid"] != first["pid"])
    assert second["restarts"] >= 1, second
finally:
    process.kill()
# The lab watches the Terminal's process and exits with it, even when the
# Terminal is killed and nobody has reaped it yet.
assert gone(second["pid"]), "the lab outlived the Terminal"
process.wait(timeout=10)

# The next Terminal starts a fresh lab; a first start is not a restart.
process = launch()
try:
    call(process, "lab.endpoint")
    third = wait(process, ready)
    assert third["pid"] != second["pid"] and third["restarts"] == 0, third
finally:
    close(process)
print("lab service started, restarted and tied to the Terminal's lifetime")
