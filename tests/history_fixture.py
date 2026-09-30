"""Seed only isolated test history using the compiled test provider, then select it."""
import json
import os
from pathlib import Path
import subprocess
import sys


def seed(call, prices, identity="fixture"):
    assert os.environ.get("ASTERION_TEST_NODE_ISOLATED") == "1"
    call("research.local")
    snapshot = call("node.action", {"id":"local", "service":"research", "action":"stop"})
    service = next(s for n in snapshot["nodes"] if n["id"] == "local" for s in n["health"]["services"] if s["id"] == "research")
    root = Path(os.environ["ASTERION_NODE_DIRECTORY"]).resolve()
    assert Path(service["directory"]).resolve() == root / "services" / "research" / "ledger"
    executable = Path(sys.argv[1]).resolve().parent / "asterion_test_history"
    result = subprocess.run([str(executable), "--directory", service["directory"], "--id", identity,
                             "--price", *map(str, prices)], check=True, capture_output=True, text=True)
    selection = json.loads(result.stdout)
    call("research.local")
    return call("research.dataset.select", selection)
