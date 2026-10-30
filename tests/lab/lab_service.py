"""The Terminal starts, supervises and stops the Python lab service.

Runs under support/isolated_node.py with ASTERION_PYTHON naming the prepared
runtime's interpreter. The Terminal starts the lab as its child, probes its
health, restarts it after it is killed and leaves it stopped when asked to
stop it. The lab exits with the Terminal, and the next Terminal starts a
service that should run.
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
    node = next(n for n in call(process, "runtime.snapshot")["nodes"] if n["id"] == "local")
    services = (node.get("health") or {}).get("services", [])
    return next((s for s in services if s["id"] == "lab"), None)


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
ready = lambda s: s["state"] == "running" and s["health"] == "ready" and s["pid"] > 0
process = launch()
try:
    call(process, "lab.endpoint")
    first = wait(process, ready)
    assert first["kind"] == "lab" and first["desired_running"] and first["restarts"] == 0, first
    assert first["endpoint"].endswith("lab.sock"), first
    # The lab logs in its own installation, <root>/lab/logs.
    logs = root / "lab" / "logs"
    started = [line for path in logs.glob(f"asterion_lab-{first['pid']}_*.log")
               for line in path.read_text().splitlines()]
    assert any(" asterion_lab lab-service service.started " in line for line in started), started

    # A process that dies is started again under the same service.
    os.kill(first["pid"], signal.SIGKILL)
    second = wait(process, lambda s: ready(s) and s["pid"] != first["pid"])
    assert second["restarts"] >= 1, second

    # A service the user stopped stays stopped.
    call(process, "node.action", {"service": "lab", "action": "stop"})
    stopped = wait(process, lambda s: s["state"] == "stopped" and not s["desired_running"])
    time.sleep(2)
    assert lab(process)["state"] == "stopped", lab(process)
    assert gone(second["pid"]), "the stopped lab process is still running"

    call(process, "node.action", {"service": "lab", "action": "start"})
    third = wait(process, ready)
finally:
    close(process)
# The lab watches the Terminal's process and exits with it, even when the
# Terminal is killed.
assert gone(third["pid"]), "the lab outlived the Terminal"

# The next Terminal starts what should run; a first start is not a restart.
process = launch()
try:
    fourth = wait(process, ready)
    assert fourth["pid"] != third["pid"] and fourth["restarts"] == 0, fourth
    call(process, "node.action", {"service": "lab", "action": "stop"})
    wait(process, lambda s: s["state"] == "stopped")
finally:
    close(process)
process = launch()
try:
    time.sleep(2)
    assert lab(process)["state"] == "stopped", lab(process)
finally:
    close(process)
print("lab service started, restarted, stopped and tied to the Terminal's lifetime")
