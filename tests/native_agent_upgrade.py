"""Opt-in cross-platform OS-managed Agent upgrade acceptance.

Uses a unique service/task name and test-owned state. Linux requires a working
systemd user manager; macOS requires a GUI launchd domain; Windows requires
Task Scheduler access for the current account. Missing prerequisites fail.
"""
import argparse
import ctypes
from concurrent.futures import ThreadPoolExecutor
import threading
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--allow-user-service", action="store_true", required=True)
args = parser.parse_args()
build = args.build.resolve()
name = "me.asterion.acceptance." + uuid.uuid4().hex[:12]
windows = sys.platform == "win32"
mac = sys.platform == "darwin"
if not windows and not mac and not sys.platform.startswith("linux"):
    raise SystemExit("Unsupported native service platform")
suffix = ".exe" if windows else ""
fixture = build / ("asterion_test_node_service_control" + suffix)
source = build / ("asterion-node-agent" + suffix)
revision = build / ("asterion_test_agent_revision" + suffix)
def command(argv, **kwargs):
    return subprocess.run(argv, capture_output=True, text=True, timeout=40, **kwargs)
def wait(check):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.1)
    raise AssertionError("Native service condition timed out")
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def alive(pid):
    if windows:
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        api.OpenProcess.restype = ctypes.c_void_p
        api.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        api.WaitForSingleObject.restype = ctypes.c_uint32
        api.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = api.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if not handle:
            if ctypes.get_last_error() == 87:
                return False
            raise OSError(ctypes.get_last_error(), "Cannot observe acceptance Agent")
        try:
            result = api.WaitForSingleObject(handle, 0)
            if result not in (0, 258):
                raise RuntimeError("Cannot observe acceptance Agent exit")
            return result == 258
        finally:
            api.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False

assert digest(source) != digest(revision)
if mac:
    domain = f"gui/{os.getuid()}"
    assert command(["/bin/launchctl", "print", domain]).returncode == 0
elif not windows:
    check = command(["/usr/bin/systemctl", "--user", "show-environment"])
    assert check.returncode == 0, "A working systemd user manager is required: " + check.stderr

with tempfile.TemporaryDirectory(prefix="ast-native-", ignore_cleanup_errors=True) as folder:
    root = Path(folder).resolve()
    state = root / "state"
    (state / "bin").mkdir(parents=True)
    binary = state / "bin" / source.name
    shutil.copy2(source, binary)
    endpoint = "asterion.acceptance." + uuid.uuid4().hex if windows else str(root / "agent.sock")
    env = dict(os.environ)
    if mac:
        home = root / "home"
        home.mkdir()
        env["HOME"] = str(home)
        definition = home / "Library/LaunchAgents" / (name + ".plist")
    elif windows:
        definition = state / "scheduled-task.xml"
    else:
        # systemd resolves installed units in the real user's search path.
        # The unique test-owned unit is removed in finally.
        definition = Path.home() / ".config/systemd/user" / (name + ".service")
        assert not definition.exists()
    def invoke(operation, pid=0, **extra):
        argv = [str(fixture), "--operation", operation, "--executable", str(binary),
                "--root", str(state), "--endpoint", endpoint, "--name", name,
                "--pid", str(pid)]
        for key, value in extra.items():
            argv.extend(["--" + key, str(value)])
        return command(argv, env=env)
    def pid():
        return int((state / "agent.pid").read_text())
    try:
        result = invoke("install")
        assert result.returncode == 0, result.stderr
        wait(lambda: (state / "agent.pid").exists() and alive(pid()))
        first = pid()
        original = definition.read_bytes()
        marker = state / "retained-data"
        marker.write_bytes(b"Test-owned data must survive an upgrade.")
        assert invoke("verify-stopped").returncode != 0
        assert invoke("stop", os.getpid()).returncode != 0
        assert alive(first)
        before = digest(binary)
        deployed = invoke("deploy-market", source=build / ("asterion-market-data" + suffix),
                          provider=build / ("asterion_test_ctp.dll" if windows else "libasterion_test_ctp.dylib" if mac else "libasterion_test_ctp.so"))
        assert deployed.returncode == 0, deployed.stderr
        wait(lambda: json.loads(invoke("market-status").stdout)["phase"] == "connected")
        initial_status = json.loads(invoke("status").stdout)["health"]
        unchanged = invoke("upgrade", source=source, expected=before)
        assert unchanged.returncode == 0, unchanged.stderr
        assert pid() == first, "A current program must not restart the Agent"
        assert json.loads(invoke("market-status").stdout)["phase"] == "connected"
        same_status = json.loads(invoke("status").stdout)["health"]
        assert [(s["id"], s["pid"]) for s in same_status["services"]] == [(s["id"], s["pid"]) for s in initial_status["services"]]
        assert not (state / "agent-service-upgrade.json").exists()
        assert not (state / "maintenance-plan.json").exists()
        pending = state / "agent-upgrade.pending"
        pending.write_bytes(b"test-owned incomplete publication")
        blocked = invoke("upgrade", source=source, expected=before)
        assert blocked.returncode != 0
        assert pending.read_bytes() == b"test-owned incomplete publication" and pid() == first
        pending.unlink()
        barrier = threading.Barrier(2)
        def competing_upgrade():
            barrier.wait()
            return invoke("upgrade", source=revision, expected=before)
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: competing_upgrade(), range(2)))
        assert any(result.returncode == 0 for result in results), [result.stderr for result in results]
        assert all(result.returncode == 0 or "agent directory is already owned" in result.stderr for result in results), [result.stderr for result in results]
        # A caller that lost the bootstrap lock can safely retry its original request.
        upgraded_pid = pid()
        retry = invoke("upgrade", source=revision, expected=before)
        assert retry.returncode == 0, retry.stderr
        assert pid() == upgraded_pid, "Completed update retries must not restart the Agent"

        wait(lambda: pid() != first and alive(pid()))
        assert digest(binary) == digest(revision)
        status = invoke("status")
        assert status.returncode == 0, status.stderr
        services = {s["id"]: s for s in json.loads(status.stdout)["health"]["services"]}
        assert services["market-running"]["desired_running"] and services["market-running"]["pid"] > 0
        assert not services["market-stopped"]["desired_running"] and services["market-stopped"]["pid"] == 0
        assert json.loads(invoke("market-status").stdout)["phase"] == "disconnected"
        assert marker.read_bytes() == b"Test-owned data must survive an upgrade."
        assert definition.read_bytes() == original
        result = invoke("stop", pid())
        assert result.returncode == 0, result.stderr
        assert invoke("verify-stopped").returncode == 0
        # Exact quiesced checkpoint after a verified stop models the stop/save gap.
        record_path = state / "agent-service-upgrade.json"
        record = dict(version=1, installed=str(binary), endpoint=endpoint, name=name,
                      before=digest(revision), after=digest(source), phase="quiesced")
        record_path.write_text(json.dumps(record))
        plan_path = state / "maintenance-plan.json"
        plan = json.loads(plan_path.read_text())
        plan.update(operation="upgrade." + digest(source)[:32], phase="ready", processes=[])
        plan_path.write_text(json.dumps(plan))
        definition.write_bytes(original + b"\n")
        assert invoke("upgrade", source=source, expected=record["before"]).returncode != 0
        assert record_path.exists() and digest(binary) == record["before"]
        definition.write_bytes(original)
        recovered = invoke("upgrade", source=source, expected=record["before"])
        assert recovered.returncode == 0, recovered.stderr
        assert not record_path.exists() and digest(binary) == digest(source)
        assert marker.read_bytes() == b"Test-owned data must survive an upgrade."
        result = invoke("stop", pid())
        assert result.returncode == 0, result.stderr
        # A stopped Agent, as the macOS development environment leaves it,
        # updates without a running process to coordinate with. Only the
        # Terminal's own machine reaches this state, and the Terminal is macOS.
        if mac:
            stopped_before = digest(binary)
            assert stopped_before != digest(revision)
            offline = invoke("upgrade", source=revision, expected=stopped_before)
            assert offline.returncode == 0, offline.stderr
            assert not record_path.exists() and digest(binary) == digest(revision)
            wait(lambda: alive(pid()))
            assert json.loads(invoke("status").stdout)["health"]["pid"] == pid()
            assert marker.read_bytes() == b"Test-owned data must survive an upgrade."
            result = invoke("stop", pid())
            assert result.returncode == 0, result.stderr
    finally:
        if mac:
            command(["/bin/launchctl", "bootout", domain + "/" + name])
        elif windows:
            command(["schtasks.exe", "/End", "/TN", name])
            command(["schtasks.exe", "/Delete", "/TN", name, "/F"])
        else:
            command(["/usr/bin/systemctl", "--user", "disable", "--now", name + ".service"])
            if definition.exists():
                definition.unlink()
            command(["/usr/bin/systemctl", "--user", "daemon-reload"])
print("PASS:", sys.platform, "native Agent no-op and concurrent upgrade, retained publication rejection, checkpoint recovery and data preservation")
