"""Keep the CTP acceptance script working: dry run against the test SDK.

The password must never reach the report; the run must pass end to end.
"""
import json
import os
from pathlib import Path
import subprocess
import signal
import sys
import tempfile
import time


def alive(pid):
    if os.name == "nt":
        import ctypes
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        api.OpenProcess.restype = ctypes.c_void_p
        api.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        api.WaitForSingleObject.restype = ctypes.c_uint32
        api.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = api.OpenProcess(0x00100000, False, pid)
        if not handle:
            if ctypes.get_last_error() == 87:
                return False
            raise OSError(ctypes.get_last_error(), "Cannot observe test process")
        try:
            status = api.WaitForSingleObject(handle, 0)
            assert status in (0, 258), status
            return status == 258
        finally:
            api.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False

build, sdk = sys.argv[1:3]
script = Path(__file__).resolve().parents[1] / "scripts/acceptance/ctp_market.py"
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
print("CTP success and rejection runs preserve password secrecy and stop their own Agent")
