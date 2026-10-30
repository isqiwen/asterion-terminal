"""Test-owned independent trading host for browser journeys; never uses systemd."""
import json
from pathlib import Path
import sys
from trading_installation import install, Programs

root, build, fingerprint = sys.argv[1:]
build = Path(build)
installation = install(root, Programs(str(build / "asterion_trading"),
    str(build / "asterion_test_certificates"), str(build / "libasterion_test_ctp_trader.so")))
port = installation.account("fixture", terminals=[fingerprint])
process = installation.start("fixture")
try:
    print(json.dumps({"host": "127.0.0.1", "port": port, "instance": "fixture",
                      "fingerprint": installation.server}), flush=True)
    for line in sys.stdin:
        if line.strip() != "restart":
            raise ValueError("unsupported test command")
        process.terminate()
        process.wait(timeout=15)
        process = installation.start("fixture")
        print("{}", flush=True)
finally:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=15)
