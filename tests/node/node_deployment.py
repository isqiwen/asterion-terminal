"""Native agent upload, supervision, background health and durable service ownership."""
import json
import os
from pathlib import Path
import signal
import select
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
from bundle_fixture import make_bundle
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "data"))
from history_fixture import contracts

if sys.platform != "linux":
    print("Remote deployment integration runs on Linux; local lifecycle is tested separately")
    raise SystemExit(77)

bridge, node, trading, certificates = map(str, sys.argv[1:])
def call(process, method, params=None, error=False, timeout=None):
    process.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n")
    process.stdin.flush()
    if timeout is not None:
        assert select.select([process.stdout], [], [], timeout)[0], "business reply waited on management"
    response = json.loads(process.stdout.readline())
    assert ("error" in response) == error, response
    return response if error else response["result"]
def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
def wait(check, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        result = check()
        if result:
            return result
        time.sleep(0.2)
    raise AssertionError("condition did not become true")
def stop(p):
    if p.poll() is None:
        p.kill()
    _, diagnostic = p.communicate(timeout=15)
    if diagnostic:
        print(diagnostic, file=sys.stderr)
with tempfile.TemporaryDirectory(prefix="asterion-agent-中文-", ignore_cleanup_errors=True) as folder:
    root = Path(folder); state = root / "node"; state.mkdir()
    (state / "config").mkdir()
    (state / "config/task.json").write_text(json.dumps({"task_concurrency": 2}))
    subprocess.run([certificates, folder], check=True)
    management, trade_port = port(), port()
    while trade_port == management:
        trade_port = port()
    transport_log = state / "logs/agent.log"
    args = [node, "--bind", "127.0.0.1", "--port", str(management), "--directory", str(state), "--tls-ca", str(root / "ca.crt"), "--tls-cert", str(root / "server.crt"), "--tls-key", str(root / "server.key")]
    def launch_node():
        p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        def listening():
            assert p.poll() is None, p.stderr.read()
            try:
                with socket.create_connection(("127.0.0.1", management), timeout=0.1):
                    return True
            except OSError:
                return False
        wait(listening)
        return p
    agent = launch_node()
    enrollment = root / "local/enrollments/task"; enrollment.mkdir(parents=True)
    for file in ("ca.crt", "client.crt", "client.key"): shutil.copyfile(root / file, enrollment / file)
    (enrollment / "enrollment.json").write_text(json.dumps({"id": "task", "host": "localhost", "port": management, "os": "linux"}))
    resources=make_bundle(root / "resources", Path(node).resolve().parent, strip_debug=True)
    terminal = subprocess.Popen([bridge], env=dict(os.environ, ASTERION_REMOTE_RESOURCES=str(resources), ASTERION_NODE_DIRECTORY=str(root / "local")), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding="utf-8")
    try:
        connection = {"id": "task", "host": "localhost", "port": str(management), "ca_file": str(root / "ca.crt"), "certificate_file": str(root / "client.crt"), "private_key_file": str(root / "client.key")}
        first = call(terminal, "node.connect", {"id": "task"})["nodes"][0]
        assert first["state"] == "online" and first["health"]["services"] == []
        # Invalid transfer cannot publish a runnable artifact.
        def varint(n):
            out = b""
            while n >= 128:
                out += bytes([(n & 127) | 128]); n >>= 7
            return out + bytes([n])
        def field(number, value):
            if isinstance(value, int):
                return varint(number << 3) + varint(value)
            if isinstance(value, str):
                value = value.encode()
            return varint((number << 3) | 2) + varint(len(value)) + value
        ctx = ssl.create_default_context(cafile=connection["ca_file"])
        ctx.load_cert_chain(connection["certificate_file"], connection["private_key_file"])
        def rpc(operation, payload):
            request = field(1, 1) + field(2, "test-transfer") + field(operation, payload)
            with socket.create_connection(("127.0.0.1", management), timeout=5) as sock, ctx.wrap_socket(sock, server_hostname="localhost") as channel:
                channel.sendall(struct.pack("!I", len(request)) + request)
                def read(size):
                    data = b""
                    while len(data) < size:
                        chunk = channel.recv(size - len(data)); assert chunk; data += chunk
                    return data
                return read(struct.unpack("!I", read(4))[0])
        digest = "0" * 64
        rpc(11, field(1, digest) + field(2, 1) + field(3, first["health"]["os"]) + field(4, first["health"]["arch"]))
        rpc(12, field(1, digest) + field(2, 0) + field(3, b"x"))
        assert b"checksum mismatch" in rpc(13, field(1, digest))
        assert not list((state / "artifacts").iterdir())
        assert b"invalid service id" in rpc(14, field(1, "../escape") + field(2, digest) + field(3, trade_port))
        # No UI/API polling for > one heartbeat period: native monitor must run.
        time.sleep(6)
        assert call(terminal, "runtime.snapshot")["nodes"][0]["last_heartbeat_ms"] > first["last_heartbeat_ms"]
        spec = {"kind":"live", "id": "task", "service": "account-test", "port": str(trade_port)}
        call(terminal, "node.deploy", {**spec, "os": "invalid"}, error=True)
        assert not list((state / "services").iterdir())
        deployed = call(terminal, "node.deploy", spec)["nodes"][0]["health"]["services"][0]
        assert deployed["state"] == "running"
        firewall={"id":"task","service":"account-test","action":"allow","token":""}
        plan=call(terminal,"node.service_firewall",firewall)["firewall_plan"]
        assert plan["port"]==trade_port and plan["source"]=="127.0.0.1" and plan["transport"]=="agent"
        call(terminal,"node.service_firewall",dict(firewall,action="apply",token="not-confirmed"),error=True)
        removed=call(terminal,"node.service_firewall",dict(firewall,action="remove"))["firewall_plan"]
        assert not removed["can_apply"], "no ownership record can authorize deletion"
        assert not (state/"firewall").exists(), "inspection must not register rules"
        call(terminal, "node.deploy", spec, error=True)  # No replacement of an existing ledger.
        wait(lambda: (state / "services/account-test/service.json").exists())
        def trading_listening():
            try:
                with socket.create_connection(("127.0.0.1", trade_port), timeout=0.1):
                    return True
            except OSError:
                return False
        wait(trading_listening)
        def account_service(node):
            return next(s for s in node["health"]["services"] if s["id"] == "account-test")
        # The paired Data service owns archived bars. The test-only provider
        # seeds that warehouse while Data is stopped.
        task_port = port()
        while task_port in {management, trade_port}:
            task_port = port()
        call(terminal, "node.deploy", {"id": "task", "kind": "task", "service": "task", "port": str(task_port)})
        history = str(Path(bridge).resolve().parent / "asterion_test_history")
        def seed(prices, identity):
            call(terminal, "node.action", {"id": "task", "service": "task-data", "action": "stop"})
            # A remote node does not report its directories; this is the Agent's own layout.
            seeded = subprocess.run(
                [history, "--directory", str(state / "services/task-data/ledger"), "--id", identity,
                 "--data-instance", "task-data", "--price", *map(str, prices)],
                env=dict(os.environ, ASTERION_NODE_DIRECTORY=str(state), ASTERION_TEST_NODE_ISOLATED="1"),
                capture_output=True, text=True)
            assert seeded.returncode == 0, seeded.stderr
            call(terminal, "node.action", {"id": "task", "service": "task-data", "action": "start"})
            call(terminal, "node.data_tasks.attach", {"id": "task", "service": "task"})
            wait(lambda: call(terminal, "runtime.snapshot")["data"]["online"])
            # What data.dataset.select takes of what the fixture program prints.
            return {key: value for key, value in json.loads(seeded.stdout).items()
                    if key in {"source_dataset_ids", "daily", "begin_day", "end_day"}}
        # A deployed CTP account service waits for its account.
        wait(lambda: account_service(call(terminal, "runtime.snapshot")["nodes"][0])["health"] == "awaiting_input")
        old_pid = deployed["pid"]
        os.kill(old_pid, signal.SIGKILL)
        def restarted():
            n = call(terminal, "runtime.snapshot")["nodes"][0]
            s = account_service(n)
            return s if s["state"] == "running" and s["pid"] != old_pid and s["restarts"] >= 1 else None
        restarted_service = wait(restarted)
        call(terminal, "node.action", {"id": "task", "service": "account-test", "action": "stop"})
        stopped = account_service(call(terminal, "runtime.snapshot")["nodes"][0])
        assert stopped["state"] == "stopped" and not stopped["desired_running"]
        updated=call(terminal,"node.update",{"id":"task","service":"account-test","revision":stopped["revision"]})
        retained=account_service(updated["nodes"][0])
        assert retained["state"]=="stopped" and retained["port"]==stopped["port"]
        time.sleep(6)
        assert account_service(call(terminal, "runtime.snapshot")["nodes"][0])["state"] == "stopped"
        # A verified CTP runtime copy does not depend on its unused source artifact.
        account_config=json.loads((state/"services/account-test/service.json").read_text())
        catalog_source=state/"artifacts"/(account_config["catalog_artifact"]+".bin")
        source_size=catalog_source.stat().st_size
        with catalog_source.open("ab") as file: file.write(b"unused source corruption")
        try:
            call(terminal, "node.action", {"id": "task", "service": "account-test", "action": "start"})
            wait(trading_listening)
        finally:
            with catalog_source.open("r+b") as file: file.truncate(source_size)
        # Agent crash makes status unknown, its owned child exits; restart loads desired state.
        stop(agent)
        wait(lambda: call(terminal, "runtime.snapshot")["nodes"][0]["state"] == "unreachable", 25)
        agent = launch_node()
        recovered = wait(lambda: (n if (n := call(terminal, "runtime.snapshot")["nodes"][0])["state"] == "online" and account_service(n)["state"] == "running" else None), 25)
        assert recovered["health"]["instance_id"] != first["health"]["instance_id"]
        wait(trading_listening)
        call(terminal, "node.action", {"id": "task", "service": "account-test", "action": "stop"})
        # A port conflict reaches a bounded failure; no infinite restart storm.
        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", 0)); occupied.listen()
            call(terminal, "node.deploy", {**spec, "service": "blocked-port", "port": str(occupied.getsockname()[1])})
            def bounded_failure():
                services = call(terminal, "runtime.snapshot")["nodes"][0]["health"]["services"]
                failed = next(s for s in services if s["id"] == "blocked-port")
                return failed if failed["state"] == "failed" else None
            failure = wait(bounded_failure, 30)
            assert failure["restarts"] == 3
            call(terminal, "node.action", {"id": "task", "service": "blocked-port", "action": "stop"})
        # Task results survive a program update of the stopped service.
        call(terminal, "data.dataset.clear")
        call(terminal, "data.dataset.select", seed([100, 101, 102, 101, 104, 103, 102, 103], "remote-backtest"))
        backtest_request = {"id": "remote-backtest", "strategies": [{"quantity": "1", "sides": "long", "rule": {"kind": "moving_average", "fast": 1, "slow": 3}}], "holdout_from": "", "walk_forward": None, "deposit": "10000", "contracts": contracts(), "max_order_quantity":"100", "max_gross_quantity":"100", "max_working_orders":"100"}
        for task_id in ("remote-backtest", "parallel-one", "parallel-two"):
            call(terminal, "backtest.submit", {**backtest_request, "id": task_id})
        def task_done(task_id):
            tasks = [task for task in call(terminal, "runtime.snapshot")["task_service"]["tasks"] if task["id"] == task_id]
            return tasks and tasks[0]["state"] == "succeeded"
        for task_id in ("remote-backtest", "parallel-one", "parallel-two"):
            wait(lambda task_id=task_id: task_done(task_id))
        result = call(terminal, "task.result", {"id": "remote-backtest"})["task_result"]
        assert result["kind"] == "backtest" and result["result"]["settlements"], result
        call(terminal, "data.dataset.clear")
        call(terminal, "data.dataset.select", seed([100 + i + i % 3 for i in range(160)], "remote-factor"))
        call(terminal, "factor.submit", {"id": "remote-factor", "series": {"kind": "bars"}, "factor": "momentum", "lookbacks": [2, 5, 10], "horizon": 1, "evaluation": {"mode": "walk_forward", "training_events": 80, "validation_events": 40}})
        wait(lambda: task_done("remote-factor"))
        factor_result = call(terminal, "task.result", {"id": "remote-factor"})["task_result"]
        assert factor_result["kind"] == "factor" and len(factor_result["result"]["samples"]) == 78, factor_result
        assert len(factor_result["result"]["folds"]) == 2, factor_result
        assert factor_result["experiment"]["evaluation"] == {"mode": "walk_forward", "training_events": 80, "validation_events": 40}, factor_result
        # Business replies come directly from the service even while both
        # management processes are suspended. No management command runs here.
        manager_pid = int((root / "local/manager/manager.pid").read_text())
        suspended = []
        try:
            for pid in (manager_pid, agent.pid):
                os.kill(pid, signal.SIGSTOP)
                suspended.append(pid)
            assert call(terminal, "task.result", {"id": "remote-backtest"}, timeout=5)["task_result"] == result
            assert call(terminal, "task.result", {"id": "remote-factor"}, timeout=5)["task_result"] == factor_result
        finally:
            for pid in suspended:
                os.kill(pid, signal.SIGCONT)
        stopped=call(terminal,"node.action",{"id":"task","service":"task","action":"stop"})
        current=next(s for n in stopped["nodes"] for s in n["health"]["services"] if s["id"]=="task")
        call(terminal,"node.update",{"id":"task","service":"task","revision":current["revision"]})
        call(terminal,"node.action",{"id":"task","service":"task","action":"start"})
        call(terminal, "node.data_tasks.attach", {"id": "task", "service": "task"})
        assert call(terminal, "task.result", {"id": "remote-backtest"})["task_result"] == result
        assert call(terminal, "task.result", {"id": "remote-factor"})["task_result"] == factor_result
        call(terminal, "node.action", {"id": "task", "service": "task", "action": "stop"})
        call(terminal, "node.disconnect", {"id": "task"})
        assert agent.poll() is None
    finally:
        failed = sys.exc_info()[0] is not None
        stop(terminal); stop(agent)
        transport_logs = sorted(transport_log.parent.glob(f"{transport_log.stem}-*{transport_log.suffix}"))
        if failed and transport_logs:
            print("Agent transport diagnostics (bounded tail):", file=sys.stderr)
            print("\n".join(transport_logs[-1].read_text().splitlines()[-100:]), file=sys.stderr)
print("Agent deployment, checksum transfer, supervision, native heartbeat, independent lifecycle and restart verified")
