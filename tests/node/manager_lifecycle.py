"""Local-only services, durable deployment, and independent Manager/Agent lifecycles."""
import json
import os
import re
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

bridge, agent = sys.argv[1:]


def wait(check, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if result := check():
            return result
        time.sleep(0.1)
    raise AssertionError("node lifecycle did not reach the expected state")


def alive(pid):
    try:
        os.kill(pid, 0)
        if sys.platform == "linux":
            return Path(f"/proc/{pid}/stat").read_text().split(") ")[1][0] != "Z"
        return True
    except (OSError, ProcessLookupError):
        return False


def call(process, method, params=None):
    process.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n")
    process.stdin.flush()
    response = json.loads(process.stdout.readline())
    assert "error" not in response, response
    return response["result"]


with tempfile.TemporaryDirectory(prefix="asterion-manager-") as directory:
    root = Path(directory)
    env = dict(os.environ, ASTERION_ROOT=directory,
               ASTERION_TEST_NODE_ISOLATED="1", ASTERION_TEST_OWNER_PID=str(os.getpid()),
               ASTERION_REMOTE_RESOURCES=str(root / "absent-remote-resources"))
    terminals, pids = [], set()
    restarted = None

    def terminal():
        process = subprocess.Popen([bridge], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, env=env)
        terminals.append(process)
        return process

    def close(process):
        process.stdin.close()
        assert process.wait(timeout=10) == 0, process.stderr.read()

    try:
        first = terminal()
        state = call(first, "node.local")
        assert state["nodes"][0]["state"] == "online", state
        manager_pid = int((root / "manager/data/manager.pid").read_text())
        agent_pid = int((root / "agent/data/agent.pid").read_text())
        pids.update([manager_pid, agent_pid])
        assert manager_pid != agent_pid
        assert not (root / "manager/config/manager.pb").exists()
        policy = root / "agent/config/research.json"
        assert json.loads(policy.read_text()) == {"task_concurrency": 1}
        policy.chmod(0o400)
        original_policy = policy.read_bytes()
        original_mtime = policy.stat().st_mtime_ns
        assert state["nodes"][0]["health"]["services"] == []
        def observed_resources():
            sample = call(first, "runtime.snapshot")["nodes"][0]["health"]["host_resources"]
            return sample if sample and sample["cpu_usage_percent"] is not None else None
        sample = wait(observed_resources)
        assert sample["failure"] is None, sample
        assert 0 <= sample["cpu_usage_percent"] <= 100, sample
        assert sample["memory_available_bytes"] > 0, sample
        assert sample["disk_path"] == str(root / "agent" / "data"), sample
        assert sample["sampled_at_ms"] > 0, sample
        # The installed native programs provide the complete local service path;
        # no remote node or cross-built Linux deployment resources are present.
        assert not Path(env["ASTERION_REMOTE_RESOURCES"]).exists()
        call(first, "market.local")
        call(first, "node.data.local.open")
        independent = call(first, "runtime.snapshot")["nodes"][0]["health"]["services"]
        assert {item["kind"] for item in independent} == {"market", "data"}, independent
        call(first, "node.research.local.open")

        def local_services(process):
            snapshot = call(process, "runtime.snapshot")
            assert [node["id"] for node in snapshot["nodes"]] == ["local"], snapshot
            health = snapshot["nodes"][0]["health"]
            services = {item["id"]: item for item in health["services"]}
            expected = {"market-data", "historical-data", "research"}
            if set(services) != expected or any(
                item["health"] not in {"ready", "awaiting_input"} or not item["pid"] for item in services.values()
            ):
                return None
            assert all(item["port"] == 0 for item in services.values()), services
            return services

        services = wait(lambda: local_services(first))
        assert "data_service" not in services["research"]
        assert "task_service" not in services["historical-data"]
        instance_policy = root / "research/config/research/research.json"
        assert instance_policy.read_bytes() == original_policy
        # Operator policy belongs to the instance and survives both supervisors.
        instance_policy.write_text('{"task_concurrency":2}\n')
        instance_policy.chmod(0o400)
        instance_contents = instance_policy.read_bytes()
        instance_mtime = instance_policy.stat().st_mtime_ns
        records = {name: (root / "agent/data/services" / name / "service.json").read_bytes()
                   for name in services}
        close(first)
        assert alive(manager_pid) and alive(agent_pid)

        second = terminal()
        call(second, "node.local")
        assert int((root / "manager/data/manager.pid").read_text()) == manager_pid
        os.kill(agent_pid, signal.SIGTERM)
        wait(lambda: not alive(agent_pid))
        wait(lambda: call(second, "runtime.snapshot")["nodes"][0]["state"] != "online")
        assert alive(manager_pid), "Agent exit terminated its Manager"

        endpoint = f"/tmp/ast-node-{(root / 'agent/data/ipc-id').read_text().strip()}/node.sock"
        assert policy.read_bytes() == original_policy and policy.stat().st_mtime_ns == original_mtime
        restarted = subprocess.Popen([str(root / "agent/bin/asterion_node_agent"), "--endpoint", endpoint],
                                     env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        pids.add(restarted.pid)
        wait(lambda: call(second, "runtime.snapshot")["nodes"][0]["state"] == "online")
        assert int((root / "manager/data/manager.pid").read_text()) == manager_pid

        os.kill(manager_pid, signal.SIGTERM)
        wait(lambda: not alive(manager_pid))
        assert restarted.poll() is None, "Manager exit terminated the independently owned Agent"
        close(second)
        third = terminal()
        state = call(third, "node.local")
        next_manager = int((root / "manager/data/manager.pid").read_text())
        pids.add(next_manager)
        assert next_manager != manager_pid
        assert int((root / "agent/data/agent.pid").read_text()) == restarted.pid
        assert state["nodes"][0]["state"] == "online"
        wait(lambda: local_services(third))
        assert instance_policy.read_bytes() == instance_contents
        assert instance_policy.stat().st_mtime_ns == instance_mtime
        for name, content in records.items():
            assert (root / "agent/data/services" / name / "service.json").read_bytes() == content
        assert not Path(env["ASTERION_REMOTE_RESOURCES"]).exists()
        close(third)
        assert policy.read_bytes() == original_policy and policy.stat().st_mtime_ns == original_mtime

        # Each program logs in its own installation, named <program>-<pid>.
        programs = set()
        for file in root.glob("*/logs/*.log"):
            match = re.fullmatch(r"(.+)-(\d+)_\d{4}-\d{2}-\d{2}\.log", file.name)
            assert match, file
            programs.add((file.parent.parent.name, match[1]))
            for line in file.read_text().splitlines():
                assert f" {match[1]} " in line, (file, line)
        assert {("terminal", "terminal"), ("agent", "asterion_node_agent"),
                ("manager", "asterion_node_manager")} <= programs, programs
    finally:
        for process in terminals:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        for pid in pids:
            if alive(pid):
                os.kill(pid, signal.SIGTERM)
        if restarted is not None:
            restarted.wait(timeout=10)
        for pid in pids:
            wait(lambda pid=pid: not alive(pid))

print("Local services work without remote resources; deployment survives independent Manager/Agent restarts")
