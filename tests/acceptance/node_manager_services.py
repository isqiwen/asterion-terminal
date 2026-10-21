"""Opt-in Linux user-service acceptance with temporary, independently registered Manager and Agent."""
import argparse
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--allow-user-services", action="store_true", required=True)
args = parser.parse_args()
build = args.build.resolve()


def wait(check):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if value := check():
            return value
        time.sleep(0.1)
    raise AssertionError("temporary user service did not reach the expected state")


subprocess.run(["systemctl", "--user", "show-environment"], check=True, stdout=subprocess.DEVNULL)
with tempfile.TemporaryDirectory(prefix="ast-manager-services-", dir="/tmp") as directory:
    root = Path(directory)
    root.chmod(0o700)
    node = root / "node"
    manager = node / "manager"
    node.mkdir()
    manager.mkdir()
    shutil.copytree(build / "config", node / "config")
    services = {}
    for role, state in (("agent", node), ("manager", manager)):
        name = f"me.asterion.acceptance.{root.name}.{role}"
        executable = build / f"asterion-node-{role}"
        if role == "manager":
            staged = root / executable.name
            shutil.copy2(executable, staged)
            executable = staged
        endpoint = root / f"{role}.sock"
        definition = Path.home() / ".config/systemd/user" / f"{name}.service"
        assert not definition.exists(), definition
        services[role] = (name, executable, state, endpoint, definition)

    def control(role, operation, pid=0):
        name, executable, state, endpoint, _ = services[role]
        result = subprocess.run(
            [str(build / "asterion_test_node_service_control"), "--operation", operation,
             "--executable", str(executable), "--root", str(state), "--endpoint", str(endpoint),
             "--name", name, "--pid", str(pid)] +
            (["--enrollment-directory", str(node / "enrollments")] if role == "manager" else []), capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr

    def pid(role):
        name = services[role][0]
        return int(subprocess.check_output(
            ["systemctl", "--user", "show", "--property=MainPID", "--value", name], text=True).strip() or 0)

    try:
        control("agent", "register")
        assert pid("agent") == 0
        control("agent", "verify-stopped")
        control("agent", "start")
        first_agent = wait(lambda: pid("agent"))
        control("manager", "register")
        assert pid("manager") == 0
        control("manager", "verify-stopped")
        control("manager", "start")
        first_manager = wait(lambda: pid("manager"))
        wait(lambda: (manager / "manager.pid").exists())
        for _, executable, _, _, definition in services.values():
            content = definition.read_text()
            assert str(executable) in content
            assert "Requires=" not in content and "PartOf=" not in content and "BindsTo=" not in content
        control("agent", "stop", first_agent)
        assert pid("manager") == first_manager
        control("agent", "install")
        next_agent = wait(lambda: pid("agent"))
        assert next_agent != first_agent
        # Desktop staging publishes a new inode while the old process still runs.
        installed = services["manager"][1]
        candidate = root / "manager-staged"
        shutil.copy2(installed, candidate)
        candidate.replace(installed)
        control("manager", "stop", first_manager)
        assert pid("agent") == next_agent
        control("agent", "stop", next_agent)
        control("manager", "verify-stopped")
        control("agent", "verify-stopped")
    finally:
        # Names were reserved by this test and never refer to an installed/dev environment.
        for name, _, _, _, definition in services.values():
            subprocess.run(["systemctl", "--user", "disable", "--now", name],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            definition.unlink(missing_ok=True)
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)

print("Independent systemd registrations verified; stopping either service preserves the other")
