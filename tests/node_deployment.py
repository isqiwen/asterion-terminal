"""Native agent upload, supervision, background health and durable service ownership."""
import json
import os
from pathlib import Path
import signal
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
from bundle_fixture import make_bundle

if sys.platform != "linux":
    print("Remote deployment integration runs on Linux; local lifecycle is tested separately")
    raise SystemExit(77)

bridge, node, trading, certificates = map(str, sys.argv[1:])
def call(process, method, params=None, error=False):
    process.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n")
    process.stdin.flush()
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
    subprocess.run([certificates, folder], check=True)
    management, trade_port = port(), port()
    while trade_port == management:
        trade_port = port()
    transport_log = root / "agent-transport.jsonl"
    args = [node, "--transport-log", str(transport_log), "--bind", "127.0.0.1", "--port", str(management), "--directory", str(state), "--tls-ca", str(root / "ca.crt"), "--tls-cert", str(root / "server.crt"), "--tls-key", str(root / "server.key")]
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
    enrollment = root / "local/enrollments/research"; enrollment.mkdir(parents=True)
    for file in ("ca.crt", "client.crt", "client.key"): shutil.copyfile(root / file, enrollment / file)
    (enrollment / "enrollment.json").write_text(json.dumps({"version": 1, "id": "research", "host": "localhost", "port": management, "os": "linux"}))
    resources=make_bundle(root / "resources", Path(node).resolve().parent)
    terminal = subprocess.Popen([bridge], env=dict(os.environ, ASTERION_REMOTE_RESOURCES=str(resources), ASTERION_NODE_DIRECTORY=str(root / "local")), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding="utf-8")
    try:
        connection = {"id": "research", "host": "localhost", "port": str(management), "ca_file": str(root / "ca.crt"), "certificate_file": str(root / "client.crt"), "private_key_file": str(root / "client.key")}
        first = call(terminal, "node.connect", {"id": "research"})["nodes"][0]
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
        spec = {"kind":"paper", "id": "research", "service": "paper-test", "port": str(trade_port)}
        call(terminal, "node.deploy", {**spec, "os": "invalid"}, error=True)
        assert not list((state / "services").iterdir())
        deployed = call(terminal, "node.deploy", spec)["nodes"][0]["health"]["services"][0]
        assert deployed["state"] == "running"
        firewall={"id":"research","service":"paper-test","action":"allow","token":""}
        plan=call(terminal,"node.service_firewall",firewall)["firewall_plan"]
        assert plan["port"]==trade_port and plan["source"]=="127.0.0.1" and plan["transport"]=="agent"
        call(terminal,"node.service_firewall",dict(firewall,action="apply",token="not-confirmed"),error=True)
        removed=call(terminal,"node.service_firewall",dict(firewall,action="remove"))["firewall_plan"]
        assert not removed["can_apply"], "no ownership record can authorize deletion"
        assert not (state/"firewall").exists(), "inspection must not register rules"
        call(terminal, "node.deploy", spec, error=True)  # No replacement of an existing ledger.
        wait(lambda: (state / "services/paper-test/service.json").exists())
        def trading_listening():
            try:
                with socket.create_connection(("127.0.0.1", trade_port), timeout=0.1):
                    return True
            except OSError:
                return False
        wait(trading_listening)
        attached = call(terminal, "node.attach", {"id": "research", "service": "paper-test"})
        assert attached["connection"]["health"]["phase"] == "awaiting_input"
        source = root / "ticks.csv"
        source.write_text("timestamp_ns,price,quantity\n100,100,1\n200,101,1\n")
        call(terminal, "futures.inspect_csv", {"path": str(source), "venue": "SHFE", "symbol": "rb2610", "product": "rb", "delivery_month": "2026-10", "currency": "CNY", "price_increment": "1", "quantity_increment": "1", "multiplier": "10"})
        call(terminal, "paper.create", {"deposit": "1000", "margin_per_lot": "100", "open_fee": "2", "close_today_fee": "3", "close_yesterday_fee": "4", "margin_rate": "0", "open_fee_rate": "0", "close_today_fee_rate": "0", "close_yesterday_fee_rate": "0", "max_order_quantity":"100", "max_gross_quantity":"100", "max_working_orders":"100"})
        expected = call(terminal, "paper.act", {"request_id": "managed.tick", "action": "advance"})["paper"]
        # A page-free connection survives the server's 30 second idle timeout.
        time.sleep(32)
        kept = call(terminal, "runtime.snapshot")
        assert kept["connection"]["state"] == "connected" and kept["paper"] == expected
        assert kept["connection"]["last_heartbeat_ms"] > attached["connection"]["last_heartbeat_ms"]
        call(terminal, "paper.close")
        old_pid = deployed["pid"]
        os.kill(old_pid, signal.SIGTERM if os.name == "nt" else signal.SIGKILL)
        def restarted():
            n = call(terminal, "runtime.snapshot")["nodes"][0]
            s = n["health"]["services"][0]
            return s if s["state"] == "running" and s["pid"] != old_pid and s["restarts"] >= 1 else None
        restarted_service = wait(restarted)
        call(terminal, "node.action", {"id": "research", "service": "paper-test", "action": "stop"})
        stopped = call(terminal, "runtime.snapshot")["nodes"][0]["health"]["services"][0]
        assert stopped["state"] == "stopped" and not stopped["desired_running"]
        updated=call(terminal,"node.update",{"id":"research","service":"paper-test","revision":stopped["revision"]})
        retained=updated["nodes"][0]["health"]["services"][0]
        assert retained["state"]=="stopped" and retained["port"]==stopped["port"]
        time.sleep(6)
        assert call(terminal, "runtime.snapshot")["nodes"][0]["health"]["services"][0]["state"] == "stopped"
        call(terminal, "node.action", {"id": "research", "service": "paper-test", "action": "start"})
        # Agent crash makes status unknown, its owned child exits; restart loads desired state.
        stop(agent)
        wait(lambda: call(terminal, "runtime.snapshot")["nodes"][0]["state"] == "unreachable", 25)
        agent = launch_node()
        recovered = wait(lambda: (n if (n := call(terminal, "runtime.snapshot")["nodes"][0])["state"] == "online" and n["health"]["services"][0]["state"] == "running" else None), 25)
        assert recovered["health"]["instance_id"] != first["health"]["instance_id"]
        wait(trading_listening)
        assert call(terminal, "node.attach", {"id": "research", "service": "paper-test"})["paper"] == expected
        assert call(terminal, "paper.act", {"request_id": "managed.tick", "action": "advance"})["paper"] == expected
        call(terminal, "paper.close")
        call(terminal, "node.action", {"id": "research", "service": "paper-test", "action": "stop"})
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
            call(terminal, "node.action", {"id": "research", "service": "blocked-port", "action": "stop"})
        # Research uses the same verified bundle, but its own service and worker.
        research_port = port()
        while research_port in {management, trade_port}:
            research_port = port()
        call(terminal, "node.deploy", {"id": "research", "kind": "research", "service": "backtests", "port": str(research_port)})
        call(terminal, "research.attach", {"id": "research", "service": "backtests"})
        source.write_text("timestamp_ns,price,quantity\n" + "".join(
            f"{1790298000000000000 + (0 if i < 4 else 259200000000000) + (i % 4) * 1000000000},{price},1\n"
            for i, price in enumerate([100, 101, 102, 101, 104, 103, 102, 103])))
        call(terminal, "futures.inspect_csv", {"path": str(source), "venue": "SHFE", "symbol": "rb2610", "product": "rb", "delivery_month": "2026-10", "currency": "CNY", "price_increment": "1", "quantity_increment": "1", "multiplier": "10"})
        call(terminal, "research.submit", {"calendar_task":"","id": "remote-backtest", "days":[{"trading_day": "2026-09-25", "schedule_source":"test fixture", "settlement_price":"105", "settlement_source":"test settlement", "sessions":[{"begin_ns":"1790298000000000000","end_ns":"1790298010000000000"}]},{"trading_day":"2026-09-28","schedule_source":"second fixture","settlement_price":"110","settlement_source":"second settlement","sessions":[{"begin_ns":"1790557200000000000","end_ns":"1790557210000000000"}]}], "fast": 1, "slow": 3, "quantity": "1", "deposit": "10000", "margin_per_lot": "100", "open_fee": "2", "close_today_fee": "3", "close_yesterday_fee": "4", "margin_rate": "0", "open_fee_rate": "0", "close_today_fee_rate": "0", "close_yesterday_fee_rate": "0", "max_order_quantity":"100", "max_gross_quantity":"100", "max_working_orders":"100"})
        def research_done(task_id="remote-backtest"):
            tasks = [task for task in call(terminal, "runtime.snapshot")["research"]["tasks"] if task["id"] == task_id]
            return tasks and tasks[0]["state"] == "succeeded"
        wait(research_done)
        result = call(terminal, "research.result", {"id": "remote-backtest"})["research_result"]
        assert result["result"]["account"]["equity"] == "10014", result
        assert [day["equity"] for day in result["result"]["settlements"]] == ["10038", "10014"]
        source.write_text("timestamp_ns,price,quantity\n" + "".join(
            f"{1790298000000000000 + i * 1000000000},{100 + i + i % 3},1\n"
            for i in range(160)))
        call(terminal, "futures.inspect_csv", {"path": str(source), "venue": "SHFE", "symbol": "rb2610", "product": "rb", "delivery_month": "2026-10", "currency": "CNY", "price_increment": "1", "quantity_increment": "1", "multiplier": "10"})
        call(terminal, "research.factor.submit", {"id": "remote-factor", "lookbacks": [2, 5, 10], "horizon": 1, "evaluation": {"mode": "walk_forward", "training_events": 80, "validation_events": 40}})
        wait(lambda: research_done("remote-factor"))
        factor_result = call(terminal, "research.result", {"id": "remote-factor"})["research_result"]
        assert factor_result["kind"] == "factor" and len(factor_result["result"]["samples"]) == 78, factor_result
        assert len(factor_result["result"]["folds"]) == 2, factor_result
        assert factor_result["experiment"]["evaluation"] == {"mode": "walk_forward", "training_events": 80, "validation_events": 40}, factor_result
        call(terminal, "research.data.submit", {"id": "remote-data"})
        wait(lambda: research_done("remote-data"))
        publication = call(terminal, "research.result", {"id": "remote-data"})["research_result"]
        assert publication["kind"] == "data_import" and publication["result"]["dataset"]["revision"] == factor_result["result"]["dataset_revision"], publication
        source.unlink()
        stopped=call(terminal,"node.action",{"id":"research","service":"backtests","action":"stop"})
        current=next(s for n in stopped["nodes"] for s in n["health"]["services"] if s["id"]=="backtests")
        call(terminal,"node.update",{"id":"research","service":"backtests","revision":current["revision"]})
        call(terminal,"node.action",{"id":"research","service":"backtests","action":"start"})
        call(terminal, "research.attach", {"id": "research", "service": "backtests"})
        assert call(terminal, "research.result", {"id": "remote-backtest"})["research_result"] == result
        assert call(terminal, "research.result", {"id": "remote-factor"})["research_result"] == factor_result
        assert call(terminal, "research.result", {"id": "remote-data"})["research_result"] == publication
        selected = call(terminal, "research.data.use", {"id": "remote-data"})["dataset"]
        assert selected["persistent"] and selected["revision"] == factor_result["result"]["dataset_revision"], selected
        call(terminal, "node.action", {"id": "research", "service": "backtests", "action": "stop"})
        call(terminal, "node.disconnect", {"id": "research"})
        assert agent.poll() is None
    finally:
        failed = sys.exc_info()[0] is not None
        stop(terminal); stop(agent)
        if failed and transport_log.exists():
            print("Agent transport diagnostics (bounded tail):", file=sys.stderr)
            print("\n".join(transport_log.read_text().splitlines()[-100:]), file=sys.stderr)
print("Agent deployment, checksum transfer, supervision, native heartbeat, independent lifecycle and restart verified")
