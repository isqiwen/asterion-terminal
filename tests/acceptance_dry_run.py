"""Keep the SimNow acceptance script working: dry run against the test SDK.

The password must never reach the report; the run must pass end to end.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

build, sdk = sys.argv[1:3]
script = Path(__file__).resolve().parents[1] / "scripts/acceptance/simnow_market.py"
secret = "dry-run-secret-value"
with tempfile.TemporaryDirectory(prefix="ast-acceptance-", ignore_cleanup_errors=True) as folder:
    report = Path(folder) / "report.json"
    result = subprocess.run(
        [sys.executable, str(script), "--build", build, "--sdk", sdk, "--front", "tcp://127.0.0.1:1",
         "--broker", "test", "--user", "fixture", "--instrument", "SHFE:rb2610", "--timeout", "30",
         "--password-stdin", "--report", str(report)],
        input=secret + "\n", capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    text = report.read_text(encoding="utf-8")
    assert secret not in text and secret not in result.stdout + result.stderr
    assert json.loads(text)["passed"] is True
print("SimNow acceptance script passes a dry run without leaking the password")
