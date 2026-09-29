"""Optional online rejection probe: invalid fixture token, real Terminal/HTTPS.
No account credentials, downloads or production data are used by this probe.
"""
import json
import os
from pathlib import Path
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


def close(process):
    process.terminate()
    process.wait(timeout=15)
    process.stdin.close()
    process.stdout.close()


process = launch()
try:
    call(process, "research.local")
    params = {"id": "tushare-invalid-token-fixture", "ts_code": "CU2310.SHF", "interval_minutes": 1,
              "catalog_cutoff_ns": "0", "requests_per_minute": 60, "token": "asterion-invalid-token-fixture"}
    for field in ("interval_minutes", "requests_per_minute"):
        for invalid in (0, -1, 1.5, 4294967297):
            process.stdin.write(json.dumps({"version": 1, "method": "research.minutes.submit", "params": dict(params, **{field: invalid})}) + "\n")
            process.stdin.flush()
            response = json.loads(process.stdout.readline())
            assert response["error"]["code"] == "invalid_request", response
    for method, values in [
        ("research.minutes.submit", params),
        ("research.minutes.submit", dict(params, start="2023-08-25 09:00:00", end="2023-08-25 09:02:00")),
        ("research.contracts.load", {"exchange": "SHFE", "product": "CU", "token": params["token"]}),
    ]:
        process.stdin.write(json.dumps({"version": 1, "method": method, "params": values}) + "\n")
        process.stdin.flush()
        response = json.loads(process.stdout.readline())
        assert "error" in response, response
        assert params["token"] not in json.dumps(response), response
    snapshot = call(process, "runtime.snapshot")
    assert not snapshot["history_contracts"]["items"], snapshot
    assert not any(task["id"] == params["id"] for task in snapshot["research"]["tasks"])
    for file in Path(os.environ["ASTERION_NODE_DIRECTORY"]).rglob("*"):
        if file.is_file() and not file.is_symlink() and file.suffix in {".json", ".log"}:
            assert params["token"].encode() not in file.read_bytes(), file.name
    print("Whole-contract lookup rejection, legacy range rejection and credential exclusion passed")
finally:
    close(process)
