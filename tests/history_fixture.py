"""Seed only isolated test history using the compiled test provider, then select it."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def seed(call, prices, identity="fixture"):
    assert os.environ.get("ASTERION_TEST_NODE_ISOLATED") == "1"
    call("node.data_tasks.local.open")
    snapshot = call("node.action", {"id":"local", "service":"historical-data", "action":"stop"})
    service = next(s for n in snapshot["nodes"] if n["id"] == "local" for s in n["health"]["services"] if s["id"] == "historical-data")
    root = Path(os.environ["ASTERION_NODE_DIRECTORY"]).resolve()
    assert Path(service["directory"]).resolve() == root / "services" / "historical-data" / "ledger"
    executable = Path(sys.argv[1]).resolve().parent / "asterion_test_history"
    result = subprocess.run([str(executable), "--directory", service["directory"], "--id", identity,
                             "--price", *map(str, prices)], check=True, capture_output=True, text=True)
    selection = json.loads(result.stdout)
    call("node.data_tasks.local.open")
    deadline = time.monotonic() + 10
    while not call("runtime.snapshot")["data"]["online"]:
        if time.monotonic() >= deadline:
            raise AssertionError("isolated data service did not become available")
        time.sleep(.05)
    return call("data.dataset.select", selection)


# The seeded contract with the costs the process tests trade it under.
CONTRACT = {"venue": "SHFE", "symbol": "rb2610"}
COSTS = {"margin_per_lot": "100", "open_fee": "2", "close_today_fee": "3", "close_yesterday_fee": "4",
         "margin_rate": "0", "open_fee_rate": "0", "close_today_fee_rate": "0",
         "close_yesterday_fee_rate": "0"}


def contracts():
    return [dict(CONTRACT, cost_schedule=[{"effective_from":"1970-01-01", "source":"test fixture", "values":dict(COSTS)}])]
