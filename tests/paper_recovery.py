from history_fixture import seed
"""Process-kill recovery through the same C ABI transport as the desktop."""
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile


def call(process, method, params=None, error=False):
    process.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n")
    process.stdin.flush()
    response = json.loads(process.stdout.readline())
    if error:
        assert "error" in response, response
        return response["error"]
    if "error" in response:
        raise AssertionError(response)
    return response["result"]


def launch():
    return subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, encoding="utf-8")


def kill(process):
    process.kill()
    process.wait(timeout=10)
    process.stdin.close()
    process.stdout.close()


with tempfile.TemporaryDirectory(prefix="asterion-crash-中文-", ignore_cleanup_errors=True) as temporary:
    directory = Path(temporary) / "account"
    directory.mkdir()
    source = Path(temporary) / "ticks.csv"
    source.write_text("timestamp_ns,price,quantity\n100,100,1\n200,99,1\n300,110,1\n", encoding="utf-8")
    process = launch()
    try:
        seed(lambda method, params=None: call(process, method, params), [100,99,110], "fixture0")
        call(process, "paper.create", {
            "directory": str(directory), "deposit": "1000", "margin_per_lot": "100",
            "open_fee": "2", "close_today_fee": "3", "close_yesterday_fee": "4", "margin_rate": "0", "open_fee_rate": "0", "close_today_fee_rate": "0", "close_yesterday_fee_rate": "0", "max_order_quantity":"1", "max_gross_quantity":"1", "max_working_orders":"1",
        })
        call(process, "paper.act", {"request_id": "tick1", "action": "advance"})
        buy = {"request_id": "buy", "action": "submit", "order_id": "order1", "side": "buy",
               "offset": "open", "quantity": "1", "price": "100"}
        oversized = dict(buy, request_id="oversized", order_id="oversized", quantity="2")
        before = call(process, "runtime.snapshot")["paper"]
        denied = call(process, "paper.act", oversized, error=True)
        assert "risk rejected" in denied["message"], denied
        assert call(process, "runtime.snapshot")["paper"] == before
        call(process, "paper.act", buy)
        state = call(process, "paper.act", {"request_id": "tick2", "action": "advance"})
        expected = state["paper"]
        child_pid = state["diagnostics"]["trading_process_id"]
        assert child_pid != process.pid and child_pid > 0
        os.kill(child_pid, signal.SIGTERM if os.name == "nt" else signal.SIGKILL)
        failed = call(process, "runtime.snapshot")
        assert failed["paper"]["storage_state"] == "recovery_required"
        assert failed["paper"]["connection_state"] == "disconnected"
    finally:
        kill(process)  # Kill the desktop host too; the ledger process was already force-terminated.
    source.unlink()
    (directory / "pending.tmp").write_text('{"incomplete":', encoding="utf-8")
    process = launch()
    try:
        rejected = call(process, "paper.open", {"directory": str(directory)}, error=True)
        assert "incomplete trading journal" in rejected["message"], rejected
        assert (directory / "pending.tmp").read_text(encoding="utf-8") == '{"incomplete":'
        # Explicit fixture inspection: this is a known test-injected partial file,
        # not a product migration or an automatic deletion of uncertain commands.
        (directory / "pending.tmp").unlink()
        recovered = call(process, "paper.open", {"directory": str(directory)})["paper"]
        assert recovered == expected, (recovered, expected)
        assert recovered["risk"] == {"max_order_quantity":"1", "max_gross_quantity":"1", "max_working_orders":1}
        denied = call(process, "paper.act", oversized, error=True)
        assert "risk rejected" in denied["message"], denied
        assert call(process, "runtime.snapshot")["paper"] == expected
        assert call(process, "paper.act", buy)["paper"] == expected
        call(process, "paper.act", {"request_id": "close", "action": "submit", "order_id": "order2",
                                   "side": "sell", "offset": "close_today", "quantity": "1", "price": "110"})
        result = call(process, "paper.act", {"request_id": "tick3", "action": "advance"})["paper"]
        assert result["balance"] == "1105" and result["positions"] == [] and len(result["fills"]) == 2
    finally:
        kill(process)
print("Process-kill recovery, lock release, interrupted write and exactly-once ledger verified")
