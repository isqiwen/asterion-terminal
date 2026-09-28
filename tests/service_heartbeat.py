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
    root = Path(folder); (root / "ledger").mkdir(); source = root / "ticks.csv"
    source.write_text("timestamp_ns,price,quantity\n100,100,1\n200,101,1\n")
    host = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    def call(method, params=None):
        host.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n"); host.stdin.flush()
        response = json.loads(host.stdout.readline()); assert "result" in response, response; return response["result"]
    try:
        call("futures.inspect_csv", {"path": str(source), "venue": "SHFE", "symbol": "rb2610", "product": "rb", "delivery_month": "2026-10", "currency": "CNY", "price_increment": "1", "quantity_increment": "1", "multiplier": "10"})
        call("paper.create", {"directory": str(root / "ledger"), "deposit": "1000", "margin_per_lot": "100", "open_fee": "2", "close_today_fee": "3", "close_yesterday_fee": "4", "margin_rate": "0", "open_fee_rate": "0", "close_today_fee_rate": "0", "close_yesterday_fee_rate": "0", "max_order_quantity":"100", "max_gross_quantity":"100", "max_working_orders":"100"})
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
        service = next(s for n in recovered["nodes"] if n["id"] == "local" for s in n["health"]["services"])
        call("paper.close")
        host.kill(); host.communicate(timeout=15)
        time.sleep(6)  # No Terminal client is alive; Agent must keep probing the service.
        host = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        status = call("node.local")
        observed = status["nodes"][0]["health"]["services"][0]
        assert observed["pid"] == service["pid"] and observed["health"] == "ready", observed
        assert observed["last_heartbeat_ms"] > service["last_heartbeat_ms"], observed
        if os.name != "nt":
            os.kill(observed["pid"], signal.SIGSTOP)  # Alive OS process, completely unresponsive business.
            end = time.monotonic() + 35
            while time.monotonic() < end:
                time.sleep(1)
                observed = call("runtime.snapshot")["nodes"][0]["health"]["services"][0]
                if observed["pid"] and observed["pid"] != service["pid"] and observed["health"] == "ready":
                    break
            else:
                raise AssertionError("Agent did not recover an unresponsive service without a trading client")
        restored = call("paper.open", {"directory": str(root / "ledger")})
        assert restored["paper"] == state["paper"]
        call("paper.close")
        before = call("node.action", {"id": "local", "service": service["id"], "action": "restart"})
        assert before["nodes"][0]["health"]["services"][0]["pid"] != observed["pid"]
        call("node.action", {"id": "local", "service": service["id"], "action": "stop"})
        time.sleep(6)
        assert call("runtime.snapshot")["nodes"][0]["health"]["services"][0]["state"] == "stopped"
    finally:
        host.kill(); host.communicate(timeout=15)
print("Native local heartbeat, crash restart, durable state and explicit close verified")
