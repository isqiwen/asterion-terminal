"""Keep the CTP acceptance script working: dry run against the test SDK.

The password must never reach the report; the run must pass end to end.
"""
import json
import io
import os
from pathlib import Path
import runpy
import subprocess
import signal
import sys
import tempfile
import time
from unittest.mock import patch


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False

build, sdk = sys.argv[1:3]
script = Path(__file__).resolve().parents[1] / "acceptance/ctp_market.py"
def exercise(secret, expected_success):
    with tempfile.TemporaryDirectory(prefix="ast-acceptance-", ignore_cleanup_errors=True) as folder:
        report = Path(folder) / "report.json"
        process = subprocess.Popen(
            [sys.executable, str(script), "--build", build, "--sdk", sdk, "--front", "tcp://127.0.0.1:1",
             "--broker", "test", "--user", "fixture", "--instrument", "SHFE:rb2610", "--timeout", "30",
             "--password-stdin", "--report", str(report)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env=dict(os.environ, TMPDIR=folder, TEMP=folder, TMP=folder))
        owned = set()
        try:
            process.stdin.write(secret + "\n")
            process.stdin.close()
            process.stdin = None
            deadline = time.monotonic() + 120
            while process.poll() is None:
                assert time.monotonic() < deadline, "acceptance script timed out"
                for pidfile in Path(folder).glob("asterion-ctp-*/agent.pid"):
                    try:
                        pid = int(pidfile.read_text())
                        assert pid > 1 and pid != os.getpid()
                        owned.add(pid)
                    except (FileNotFoundError, ValueError):
                        pass  # The file can be in the middle of its initial write.
                time.sleep(0.02)
            stdout, stderr = process.communicate(timeout=5)
            assert process.returncode == (0 if expected_success else 1), stdout + stderr
            assert owned, "did not observe the test-owned Agent"
            assert not any(alive(pid) for pid in owned), "acceptance left its Agent running"
        finally:
            if process.poll() is None:
                process.kill(); process.communicate(timeout=15)
            for pid in owned:
                if alive(pid):
                    os.kill(pid, signal.SIGTERM)
        text = report.read_text(encoding="utf-8")
        assert secret not in text and secret not in stdout + stderr
        assert json.loads(text)["passed"] is expected_success


exercise("dry-run-secret-value", True)
exercise("reject-test-only", False)


def failed_completion(disconnect, cleanup_error=False, local_market=True):
    """Quotes alone must not pass acceptance when another required step fails."""
    with tempfile.TemporaryDirectory(prefix="ast-acceptance-failure-") as folder:
        report = Path(folder) / "report.json"
        responses = [
            {"result": {"market": {"phase": "disconnected"} if local_market else None}},
            {"result": {}},
            {"result": {"market": {"phase": "connected", "subscriptions": [
                {"venue": "SHFE", "symbol": "rb2610", "quote": {"last": "100", "volume": "1"}}
            ]}}},
            disconnect,
        ]

        class Bridge:
            stdin = io.StringIO()
            stdout = io.StringIO("".join(json.dumps(row) + "\n" for row in responses))

            def terminate(self):
                if cleanup_error:
                    raise RuntimeError("test bridge cleanup failed")

            def wait(self, timeout):
                return 0

        argv = [str(script), "--build", build, "--sdk", sdk, "--front", "tcp://127.0.0.1:1",
                "--broker", "test", "--user", "fixture", "--instrument", "SHFE:rb2610",
                "--password-stdin", "--report", str(report)]
        output = io.StringIO()
        with patch.object(sys, "argv", argv), patch.object(sys, "stdin", io.StringIO("fixture-secret\n")), \
                patch.object(sys, "stdout", output), patch("subprocess.Popen", return_value=Bridge()):
            try:
                runpy.run_path(str(script), run_name="__main__")
            except SystemExit as error:
                assert error.code == 1, output.getvalue()
            else:
                raise AssertionError("Acceptance did not return a failure status")
        result = json.loads(report.read_text())
        assert result["passed"] is False, result
        assert any(not row["ok"] for row in result["steps"]), result
        assert "fixture-secret" not in report.read_text() + output.getvalue()


failed_completion({"result": {"market": {"phase": "connected"}}})
failed_completion({"error": {"code": "operation_failed", "message": "disconnect failed"}})
failed_completion({"result": {"market": {"phase": "disconnected"}}}, cleanup_error=True)
failed_completion({"result": {"market": {"phase": "disconnected"}}}, local_market=False)

with tempfile.TemporaryDirectory(prefix="ast-acceptance-startup-") as folder:
    report = Path(folder) / "report.json"
    report.write_text(json.dumps({"passed": True, "steps": [{"step": "previous run", "ok": True}]}))
    failed = subprocess.run(
        [sys.executable, str(script), "--build", folder, "--sdk", sdk,
         "--front", "tcp://127.0.0.1:1", "--broker", "test", "--user", "fixture",
         "--instrument", "SHFE:rb2610", "--password-stdin", "--report", str(report)],
        input="fixture-secret\n", capture_output=True, text=True, timeout=10)
    assert failed.returncode != 0, failed.stdout + failed.stderr
    assert json.loads(report.read_text())["passed"] is False
    assert "fixture-secret" not in report.read_text() + failed.stdout + failed.stderr

print("CTP success, rejection and failed completion preserve secrecy, cleanup and accurate verdicts")
