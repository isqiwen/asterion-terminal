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
    call(process, "node.data_tasks.local.open")
    params = {"id": "tushare-invalid-token-fixture", "source": "tushare.ft_mins",
              "contract_id": "SHFE/cu/2023-10", "interval_minutes": 1, "snapshot": "0" * 64,
              "cutoff_ns": "1", "requests_per_minute": 60, "token": "asterion-invalid-token-fixture"}
    for field in ("interval_minutes", "requests_per_minute"):
        for invalid in (0, -1, 1.5, 4294967297):
            process.stdin.write(json.dumps({"version": 1, "method": "data.download.minutes.submit", "params": dict(params, **{field: invalid})}) + "\n")
            process.stdin.flush()
            response = json.loads(process.stdout.readline())
            assert response["error"]["code"] == "invalid_request", response
    # A download takes its contract from the stored contract list; a snapshot
    # the store never had is refused before any request reaches the source.
    for values in (params, dict(params, start="2023-08-25 09:00:00", end="2023-08-25 09:02:00")):
        process.stdin.write(json.dumps({"version": 1, "method": "data.download.minutes.submit", "params": values}) + "\n")
        process.stdin.flush()
        response = json.loads(process.stdout.readline())
        assert "error" in response, response
        assert params["token"] not in json.dumps(response), response
    # The source itself rejects the token over HTTPS.
    provider = call(process, "runtime.snapshot")["data"]["sources"][0]["plugin_id"]
    call(process, "data.credentials.save", {"provider": provider, "credential": params["token"],
                                             "remember": False, "requests_per_minute": 60})
    verification = call(process, "data.credentials.verify", {"provider": provider})
    checks = verification["credential_verification"]["checks"]
    assert checks and all(check["state"] != 0 for check in checks), checks
    assert params["token"] not in json.dumps(verification), verification
    call(process, "data.credentials.clear", {"provider": provider})
    snapshot = call(process, "runtime.snapshot")
    assert not any(task["id"] == params["id"] for task in snapshot["task_service"]["tasks"])
    for file in Path(os.environ["ASTERION_NODE_DIRECTORY"]).rglob("*"):
        if file.is_file() and not file.is_symlink() and file.suffix in {".json", ".log"}:
            assert params["token"].encode() not in file.read_bytes(), file.name
    print("Unsupported range rejection, online token rejection and credential exclusion passed")
finally:
    close(process)
