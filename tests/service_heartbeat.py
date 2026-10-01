from history_fixture import contracts, seed
"""Local default supervision runs without any UI timer or command replay."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
with tempfile.TemporaryDirectory(prefix="asterion-local-health-", ignore_cleanup_errors=True) as folder:
    root = Path(folder); (root / "ledger").mkdir()
    host = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    def call(method, params=None):
        host.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n"); host.stdin.flush()
        response = json.loads(host.stdout.readline()); assert "result" in response, response; return response["result"]
    def paper_service(snapshot):
        return next(s for n in snapshot["nodes"] if n["id"] == "local" for s in n["health"]["services"] if s["kind"] == "paper")
    try:
        seed(call, [100,101], "fixture0")
        call("paper.create", {"directory": str(root / "ledger"), "deposit": "1000", "contracts": contracts(), "max_order_quantity":"100", "max_gross_quantity":"100", "max_working_orders":"100"})
        command = {"request_id": "tick-one", "action": "advance"}
        state = call("paper.act", command)
        time.sleep(6)
        alive = call("runtime.snapshot")
        assert alive["connection"]["last_heartbeat_ms"] > state["connection"]["last_heartbeat_ms"]
        old_pid = alive["diagnostics"]["trading_process_id"]
        os.kill(old_pid, signal.SIGTERM if os.name == "nt" else signal.SIGKILL)
        end = time.monotonic() + 20
        while time.monotonic() < end:
            recovered = call("runtime.snapshot")
            if recovered["connection"]["state"] == "connected" and recovered["diagnostics"]["trading_process_id"] != old_pid:
                break
            time.sleep(0.2)
        else:
            raise AssertionError("local service was not restarted")
        assert recovered["connection"]["restarts"] == 1
        assert recovered["connection"]["health"]["instance_id"] != alive["connection"]["health"]["instance_id"]
        assert recovered["paper"] == state["paper"]
        assert call("paper.act", command)["paper"] == state["paper"]
        # Trading and Agent monitors refresh independently. Wait for the Agent's
        # report of the recovered PID before using it as the no-client baseline.
        end = time.monotonic() + 20
        while time.monotonic() < end:
            service = paper_service(call("runtime.snapshot"))
            if service["pid"] == recovered["diagnostics"]["trading_process_id"] and service["health"] == "ready":
                break
            time.sleep(0.2)
        else:
            raise AssertionError("Agent status did not observe the recovered trading process")
        call("paper.close")
        host.kill(); host.communicate(timeout=15)
        time.sleep(6)  # No Terminal client is alive; Agent must keep probing the service.
        host = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        status = call("node.local")
        observed = paper_service(status)
        assert observed["pid"] == service["pid"] and observed["health"] == "ready", observed
        assert observed["last_heartbeat_ms"] > service["last_heartbeat_ms"], observed
        if os.name != "nt":
            os.kill(observed["pid"], signal.SIGSTOP)  # Alive OS process, completely unresponsive business.
            end = time.monotonic() + 35
            while time.monotonic() < end:
                time.sleep(1)
                observed = paper_service(call("runtime.snapshot"))
                if observed["pid"] and observed["pid"] != service["pid"] and observed["health"] == "ready":
                    break
            else:
                raise AssertionError("Agent did not recover an unresponsive service without a trading client")
        restored = call("paper.open", {"directory": str(root / "ledger")})
        assert restored["paper"] == state["paper"]
        call("paper.close")
        before = call("node.action", {"id": "local", "service": service["id"], "action": "restart"})
        assert paper_service(before)["pid"] != observed["pid"]
        call("node.action", {"id": "local", "service": service["id"], "action": "stop"})
        time.sleep(6)
        assert paper_service(call("runtime.snapshot"))["state"] == "stopped"
    finally:
        host.kill(); host.communicate(timeout=15)
print("Native local heartbeat, crash restart, durable state and explicit close verified")
