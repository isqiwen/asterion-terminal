"""Local real-mode and remote simulation-mode instances, using only test SDKs."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "node"))
from bundle_fixture import make_bundle

if sys.platform != "linux":
    raise SystemExit(77)
build = Path(sys.argv[1]).resolve()


def port():
    with socket.socket() as channel:
        channel.bind(("127.0.0.1", 0))
        return channel.getsockname()[1]


def call(process, method, params=None, error=False):
    process.stdin.write(json.dumps(dict(version=1, method=method, params=params or {})) + "\n")
    process.stdin.flush()
    reply = json.loads(process.stdout.readline())
    assert ("error" in reply) == error, reply
    return reply["error"] if error else reply["result"]


def wait(process, condition):
    end = time.monotonic() + 20
    while time.monotonic() < end:
        state = call(process, "runtime.snapshot")
        if condition(state):
            return state
        time.sleep(.1)
    raise AssertionError(str(state)[:2000])


with tempfile.TemporaryDirectory(prefix="trading-instances-", dir=os.environ["ASTERION_NODE_DIRECTORY"], ignore_cleanup_errors=True) as folder:
    root = Path(folder)
    remote = root / "remote"
    remote.mkdir()
    subprocess.run([str(build / "asterion_test_certificates"), folder], check=True)
    management_port, trade_port = port(), port()
    while trade_port == management_port:
        trade_port = port()
    agent_env = dict(os.environ, ASTERION_NODE_DIRECTORY=str(remote), ASTERION_TEST_NODE_ISOLATED="1")
    agent = subprocess.Popen([
        str(build / "asterion_node_agent"), "--directory", str(remote), "--bind", "127.0.0.1",
        "--port", str(management_port), "--tls-ca", str(root / "ca.crt"),
        "--tls-cert", str(root / "server.crt"), "--tls-key", str(root / "server.key")],
        env=agent_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    terminal = None
    try:
        local = root / "local"
        enrollment = local / "enrollments/remote"
        enrollment.mkdir(parents=True)
        for name in ("ca.crt", "client.crt", "client.key"):
            shutil.copyfile(root / name, enrollment / name)
        (enrollment / "enrollment.json").write_text(json.dumps(dict(
            id="remote", host="localhost", port=management_port, os="linux")))
        resources = make_bundle(root / "resources", build, strip_debug=True)
        # Replace the disposable bundle's vendor SDKs with test-only providers.
        bundle = resources / "x86_64"
        for target, source in (("ctp-trader.so", "libasterion_test_ctp_trader.so"),
                               ("ctp-md.so", "libasterion_test_ctp.so")):
            shutil.copyfile(build / source, bundle / target)
        manifest_path = bundle / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        for name in ("ctp-trader.so", "ctp-md.so"):
            manifest["files"][name] = hashlib.sha256((bundle / name).read_bytes()).hexdigest()
        manifest_path.write_text(json.dumps(manifest))
        env = dict(os.environ, ASTERION_NODE_DIRECTORY=str(local), ASTERION_REMOTE_RESOURCES=str(resources),
                   ASTERION_CTP_LIBRARY=str(build / "libasterion_test_ctp.so"),
                   ASTERION_CTP_CATALOG_LIBRARY=str(build / "libasterion_test_ctp_trader.so"))

        def launch():
            return subprocess.Popen([str(build / "asterion_terminal_dev_bridge")], env=env,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.DEVNULL, text=True)

        terminal = launch()
        call(terminal, "market.local")
        call(terminal, "node.deploy", dict(id="local", service="trading", kind="live", port="0"))
        call(terminal, "node.connect", {"id": "remote"})
        call(terminal, "node.deploy", dict(id="remote", service="trading", kind="live", port=str(trade_port)))
        for account in ("real", "simulation"):
            call(terminal, "ctp.connections.save", dict(id=account, name=account, revision="",
                broker_id="9999", user_id="000001" if account == "real" else "000002",
                app_id="test", trade_front="tcp://127.0.0.1:41205", market_front="tcp://127.0.0.1:1"))
        call(terminal, "market.catalog", dict(account="real", password="test-only", auth_code="test-only"))
        wait(terminal, lambda s: s["market"]["catalog"]["phase"] == "ready")
        create = dict(max_order_quantity="5", max_gross_quantity="10", max_working_orders="2",
                      max_price_deviation="0.02", contracts=[dict(venue="SHFE", symbol="rb2610")])
        call(terminal, "live.create", dict(create, account="real", node="local", service="trading", environment="real"))
        state = call(terminal, "live.create", dict(create, account="simulation", node="remote",
                                                 service="trading", environment="simulation"))
        assert state["live"]["real"]["connection"]["transport"] == "local"
        assert state["live"]["simulation"]["connection"]["transport"] == "tcp_tls"
        for account in ("real", "simulation"):
            assert state["live"][account]["session"]["environment"] == account
            assert state["live"][account]["session"]["authorization"] is None
            call(terminal, "live.connect", dict(account=account, password="test-only", auth_code="test-only"))
        state = wait(terminal, lambda s: all(s["live"][a]["session"]["phase"] == "ready"
                                            for a in ("real", "simulation")))
        # Authorizing one account cannot authorize the other.
        live = state["live"]["simulation"]["session"]
        state = call(terminal, "live.act", dict(account="simulation", account_id=live["account_id"],
            policy_revision=live["policy_revision"], request_id="allow.simulation", action="live_authorize", user_id="000002"))
        assert state["live"]["simulation"]["session"]["authorization"]
        assert state["live"]["real"]["session"]["authorization"] is None
        order = dict(request_id="submit.remote", action="submit", order_id="remote-order",
                     venue="SHFE", symbol="rb2610", side="buy", offset="open", quantity="1", price="3500")
        call(terminal, "live.act", dict(order, account="simulation", account_id=live["account_id"],
                                       policy_revision=live["policy_revision"]))
        state = wait(terminal, lambda s: any(o["id"] == "remote-order" and o["status"] == "filled"
                     for o in s["live"]["simulation"]["session"]["orders"]))
        assert state["live"]["real"]["session"]["orders"] == []
        bindings = {a["id"]: a["trading"] for a in state["ctp_connections"]}
        assert bindings["simulation"] == dict(node="remote", service="trading")
        assert (remote / "services/trading/ledger/journal.sqlite").is_file()
        assert not (local / "accounts/ctp/simulation/journal.sqlite").exists()
        local_node = next(n for n in state["nodes"] if n["id"] == "local")
        local_service = next(s for s in local_node["health"]["services"] if s["id"] == bindings["real"]["service"])
        real_pid = local_service["pid"]
        call(terminal, "ctp.connections.save", dict(id="other", name="other", revision="",
            broker_id="9999", user_id="000003", app_id="test", trade_front="tcp://127.0.0.1:41205",
            market_front="tcp://127.0.0.1:1"))
        refused = call(terminal, "live.create", dict(create, account="other", node="remote",
            service="trading", environment="simulation"), error=True)
        assert "another account" in refused["message"]
        current = call(terminal, "runtime.snapshot")
        assert "trading" not in next(a for a in current["ctp_connections"] if a["id"] == "other")
        # Closing/reopening the account uses its persisted target, never a local fallback.
        call(terminal, "live.close", dict(account="simulation"))
        state = call(terminal, "live.open", dict(account="simulation"))
        assert state["live"]["simulation"]["connection"]["transport"] == "tcp_tls"
        call(terminal, "live.close", dict(account="simulation"))
        refused = call(terminal, "live.create", dict(create, account="simulation", node="local",
                                                    service="", environment="simulation"), error=True)
        assert "already bound" in refused["message"]
        call(terminal, "live.open", dict(account="simulation"))
        call(terminal, "node.action", dict(id="remote", service="trading", action="stop"))
        os.kill(real_pid, 0)
        state = call(terminal, "runtime.snapshot")
        assert state["live"]["real"]["session"]["phase"] == "ready"
        call(terminal, "live.close", dict(account="simulation"))
        state = call(terminal, "live.open", dict(account="simulation"))
        assert state["live"]["simulation"]["session"]["environment"] == "simulation"
        assert state["live"]["simulation"]["session"]["authorization"] is None
        # A new Terminal process reads the same reference; remote records stay remote.
        terminal.communicate(timeout=15)
        terminal = launch()
        state = call(terminal, "live.open", dict(account="simulation"))
        assert state["live"]["simulation"]["connection"]["transport"] == "tcp_tls"
        assert state["live"]["simulation"]["session"]["account_id"] == "simulation"
    finally:
        if terminal and terminal.poll() is None:
            terminal.communicate(timeout=15)
        agent.terminate()
        agent.wait(timeout=15)
        for name in ("manager.pid", "agent.pid"):
            pidfile = root / "local" / name
            if pidfile.exists():
                try:
                    os.kill(int(pidfile.read_text()), signal.SIGTERM)
                except ProcessLookupError:
                    pass
        time.sleep(1)
print("Trading instances preserve placement/environment, use direct mTLS, and isolate lifecycle/authorization")
