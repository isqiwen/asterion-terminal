"""A recording node's market service logs in, reads the catalog and records
with no client: the login from config/market-recording.json and systemd
credentials in $CREDENTIALS_DIRECTORY, against the test-only CTP libraries.

    market_unattended.py <asterion_market> <test ctp md library> <test ctp trader library>
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

market, md, trader = sys.argv[1:4]
with tempfile.TemporaryDirectory(prefix="ast-unattended-") as folder:
    root = Path(folder)
    (root / "service").mkdir()
    (root / "credentials").mkdir(mode=0o700)
    (root / "credentials/asterion.ctp.password").write_text("fixture-only-secret\n")
    (root / "credentials/asterion.ctp.auth_code").write_text("fixture-auth")
    configuration = root / "market-recording.json"
    configuration.write_text(json.dumps({
        "market_front": "tcp://127.0.0.1:1", "trade_front": "tcp://127.0.0.1:1",
        "broker": "test", "user": "fixture", "app_id": "app"}))
    env = dict(os.environ, CREDENTIALS_DIRECTORY=str(root / "credentials"),
               ASTERION_LOG_DIRECTORY=str(root / "logs"))
    process = subprocess.Popen(
        [market, "--session", "market-data", "--node-id", "fixture-node",
         "--directory", str(root / "service"), "--endpoint", str(root / "m.sock"),
         "--health-endpoint", str(root / "h.sock"), "--ctp-library", md,
         "--ctp-catalog-library", trader, "--recording-config", str(configuration),
         "--owner-pid", str(os.getpid())], env=env, stderr=subprocess.PIPE, text=True)
    try:
        # The catalog was read with nobody asking for it: it is cached once ready.
        deadline = time.monotonic() + 30
        while not (root / "service/ctp-catalog.pb").exists():
            assert process.poll() is None, process.stderr.read()
            assert time.monotonic() < deadline, "the catalog was not read unattended"
            time.sleep(.2)
        time.sleep(3)  # quotes of the subscribed market
    finally:
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=15)
    ticks = list((root / "service/recordings/ticks").rglob("*.parquet"))
    assert ticks, "nothing was recorded"
    logs = "".join(path.read_text() for path in (root / "logs").rglob("*.log"))
    assert "unattended.configured" in logs, logs
    assert "fixture-only-secret" not in logs and "fixture-auth" not in logs
    assert "fixture-only-secret" not in process.stderr.read()
    print("unattended login recorded", len(ticks), "tick files")
