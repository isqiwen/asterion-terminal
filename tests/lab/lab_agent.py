"""The local Agent registers, supervises and stops the Python lab service.

Runs under support/isolated_node.py with ASTERION_PYTHON naming the prepared
runtime's interpreter. Local program preparation (as the installer and the
development entry run it) registers the lab service; the Agent then starts it,
probes its health, restarts it after it is killed and leaves it stopped when
asked to stop it.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


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


# A ServicePrograms manifest naming only the interpreter: version 1, python (field 4).
root = Path(os.environ["ASTERION_NODE_DIRECTORY"])
python = os.environ["ASTERION_PYTHON"].encode()
(root / "services").mkdir(parents=True, exist_ok=True)
(root / "programs.pb").write_bytes(b"\x08\x01\x22" + bytes([len(python)]) + python)
subprocess.run(["/usr/bin/env", "ASTERION_ENVIRONMENT=development", sys.argv[2], "--directory",
                str(root), "--prepare-programs", str(root / "programs.pb")], check=True)

process = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, text=True, encoding="utf-8")
try:
    call(process, "node.local")
    ready = lambda s: s["state"] == "running" and s["health"] == "ready" and s["pid"] > 0
    first = wait(process, ready)
    assert first["kind"] == "lab" and first["desired_running"], first
    assert first["endpoint"].endswith("lab.sock"), first
    logs = root / "logs"
    started = [json.loads(line) for path in logs.glob(f"lab-service-{first['pid']}_*.log")
               for line in path.read_text().splitlines()]
    assert any(record["event"] == "service.started" for record in started), started

    # A process that dies is started again under the same service.
    os.kill(first["pid"], signal.SIGKILL)
    second = wait(process, lambda s: ready(s) and s["pid"] != first["pid"])
    assert second["restarts"] >= 1, second

    # A service the user stopped stays stopped.
    call(process, "node.action", {"id": "local", "service": "lab", "action": "stop"})
    stopped = wait(process, lambda s: s["state"] == "stopped" and not s["desired_running"])
    time.sleep(2)
    assert lab(process)["state"] == "stopped", lab(process)
    try:
        os.kill(second["pid"], 0)
        raise AssertionError("the stopped lab process is still running")
    except ProcessLookupError:
        pass
    print("lab service registered, healthy, restarted and stopped:", stopped["id"])
finally:
    process.kill()
    process.wait(timeout=10)
