"""Independent real-mode and simulation-mode instances, using only test SDKs."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
from trading_installation import install, Programs
from warehouse_installation import market_host
from trading_requests import account_policy, market_catalog, submit
import warehouse_tls


if sys.platform != "linux":
    raise SystemExit(77)
build = Path(sys.argv[1]).resolve()


def call(process, method, params=None, error=False):
    request = dict(version=1, method=method, params=params or {})
    process.stdin.write(json.dumps(dict(id=1, request=request)) + "\n")
    process.stdin.flush()
    reply = json.loads(process.stdout.readline())["reply"]
    assert ("error" in reply) == error, reply
    return reply["error"] if error else reply["result"]


def trading(process, account, request, error=False):
    """One trading.proto Request for the named account; the Response as ProtoJSON."""
    reply = call(process, "trading.call", dict(account=account, request=request), error=error)
    return reply if error else reply["trading"]


def account_of(process, account):
    return trading(process, account, {"getAccount": {}})["getAccount"]


def session(process, account):
    return account_of(process, account).get("session")


def wait(condition):
    end = time.monotonic() + 20
    while time.monotonic() < end:
        if condition():
            return
        time.sleep(.1)
    raise AssertionError("condition not reached")


with tempfile.TemporaryDirectory(prefix="trading-instances-", dir=os.environ["ASTERION_ROOT"], ignore_cleanup_errors=True) as folder, market_host(build, Path(folder) / "market") as market_port:
    root = Path(folder)
    host = install(root / "trading-host", Programs(str(build / "asterion_trading"),
        str(build / "asterion_test_certificates"), str(build / "libasterion_test_ctp_trader.so")))
    terminal = None
    children = {}
    try:
        local = root / "local"
        env = dict(os.environ, ASTERION_ROOT=str(local),
                   ASTERION_CTP_LIBRARY=str(build / "libasterion_test_ctp.so"),
                   ASTERION_CTP_CATALOG_LIBRARY=str(build / "libasterion_test_ctp_trader.so"))

        def launch():
            return subprocess.Popen([str(build / "asterion_terminal_dev_bridge")], env=env,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.DEVNULL, text=True)

        tls = root / "market/config/tls"
        warehouse_tls.install_client(tls, local)
        terminal = launch()
        call(terminal, "market.endpoint.save", dict(host="127.0.0.1", port=market_port, fingerprint=warehouse_tls.fingerprint(tls)))
        call(terminal, "market.open")
        fingerprint = call(terminal, "runtime.snapshot")["client_fingerprint"]
        for name in ("real", "simulation"):
            port = host.account(name, terminals=[fingerprint], environment=name, user="000001" if name == "real" else "000002")
            children[name] = host.start(name)
            call(terminal, "trading.connections.save", dict(id=name, revision="",
                host="127.0.0.1", port=port, instance=name, fingerprint=host.server))
        create = account_policy(market_catalog(lambda *a: call(terminal, *a)))
        # A mistyped server pin cannot reach the account.
        saved = next(c for c in call(terminal, "runtime.snapshot")["trading_connections"] if c["id"] == "real")
        call(terminal, "trading.connections.save", dict(saved, fingerprint=host.stranger))
        trading(terminal, "real", {"getAccount": {}}, error=True)
        inspected = call(terminal, "runtime.snapshot")
        revision = next(c["revision"] for c in inspected["trading_connections"] if c["id"] == "real")
        call(terminal, "trading.connections.save", dict(saved, revision=revision))
        real = account_of(terminal, "real")
        simulation = account_of(terminal, "simulation")
        assert real["environment"] == "ENVIRONMENT_REAL"
        assert simulation["broker"]["userId"] == "000002"
        assert "session" not in simulation
        environments = {"real": "ENVIRONMENT_REAL", "simulation": "ENVIRONMENT_SIMULATION"}
        for account in ("real", "simulation"):
            opened = trading(terminal, account, create)["openAccount"]
            assert opened["environment"] == environments[account]
            assert "authorizedMs" not in opened["session"]
        wait(lambda: all(session(terminal, a)["brokerState"] == "BROKER_STATE_READY"
                         for a in ("real", "simulation")))
        # A different instance, port, front and environment still owns the same funds.
        port = host.account("duplicate", terminals=[fingerprint], environment="simulation", user="000001")
        duplicate_config = host.root / "config/accounts/duplicate.json"
        duplicate = json.loads(duplicate_config.read_text())
        duplicate["broker"].update(front="tcp://127.0.0.1:41206", app_id="another_app")
        duplicate_config.write_text(json.dumps(duplicate))
        children["duplicate"] = host.start("duplicate")
        call(terminal, "trading.connections.save", dict(id="duplicate", revision="",
            host="127.0.0.1", port=port, instance="duplicate", fingerprint=host.server))
        rejected = trading(terminal, "duplicate", create, error=True)
        assert "account execution owner" in str(rejected), rejected
        assert not (host.root / "data/duplicate/journal.sqlite").exists()
        # Authorizing one account cannot authorize the other.
        reviewed = {"accountId": "simulation",
                    "policyRevision": session(terminal, "simulation")["policyRevision"]}
        trading(terminal, "simulation", {"sendCommand": dict(reviewed,
            requestId="allow.simulation", authorize={"userId": "000002"})})
        assert "authorizedMs" in session(terminal, "simulation")
        assert "authorizedMs" not in session(terminal, "real")
        # Desktop exit is not a trading control: the independent host retains
        # authorization and can continue to execute after a fresh client attaches.
        terminal.communicate(timeout=15)
        assert all(process.poll() is None for process in children.values())
        terminal = launch()
        assert "authorizedMs" in session(terminal, "simulation")
        assert "authorizedMs" not in session(terminal, "real")
        trading(terminal, "simulation", {"sendCommand": dict(reviewed, **submit("remote-order", 1))})
        wait(lambda: any(o.get("id") == "remote-order" and o["state"] == "ORDER_STATE_FILLED"
                         for o in session(terminal, "simulation").get("orders", [])))
        assert "orders" not in session(terminal, "real")
        assert (host.root / "data/simulation/journal.sqlite").is_file()
        assert not (local / "accounts/ctp/simulation/journal.sqlite").exists()
        real_pid = children["real"].pid
        # Counter settings cannot enter the create request from a Terminal.
        trading(terminal, "real", {"openAccount": dict(create["openAccount"],
                                                       environment="ENVIRONMENT_SIMULATION")}, error=True)
        # The fill is in the record before the service stops.
        today = session(terminal, "simulation")["tradingDay"]
        wait(lambda: trading(terminal, "simulation", {"getDay": {"tradingDay": today}})
             ["getDay"].get("trades"))
        children["simulation"].terminate()
        children["simulation"].wait(timeout=15)
        os.kill(real_pid, 0)
        assert session(terminal, "real")["brokerState"] == "BROKER_STATE_READY"
        trading(terminal, "simulation", {"getAccount": {}}, error=True)
        # The same account behind another front and AppID keeps its record.
        moved = host.root / "config/accounts/simulation.json"
        account = json.loads(moved.read_text())
        account["broker"].update(front="tcp://127.0.0.1:41207", app_id="moved_app")
        moved.write_text(json.dumps(account))
        children["simulation"] = host.start("simulation")
        restarted = account_of(terminal, "simulation")
        assert restarted["environment"] == "ENVIRONMENT_SIMULATION"
        assert restarted["broker"]["front"] == "tcp://127.0.0.1:41207"
        assert restarted["session"]["policyRevision"] == reviewed["policyRevision"]
        assert "authorizedMs" not in restarted["session"]
        kept = trading(terminal, "simulation", {"getDay": {"tradingDay": today}})["getDay"]
        assert [trade["orderId"] for trade in kept["trades"]] == ["remote-order"], kept
        # A new Terminal process reads the same reference; remote records stay remote.
        terminal.communicate(timeout=15)
        terminal = launch()
        assert account_of(terminal, "simulation")["id"] == "simulation"
        children["real"].terminate(); children["real"].wait(timeout=15)
        rejected = trading(terminal, "duplicate", create, error=True)
        assert str(host.root / "data/real") in str(rejected), rejected
        assert not (host.root / "data/duplicate/journal.sqlite").exists()
        # Changing a configured account must not silently reuse an existing journal.
        config = host.root / "config/accounts/real.json"
        original = config.read_text()
        changed = json.loads(original); changed["broker"]["user_id"] = "000009"
        config.write_text(json.dumps(changed))
        rejected = subprocess.run([str(host.root / "bin/asterion_trading"), "--account", "real"], capture_output=True, text=True, timeout=15, env=dict(os.environ, CREDENTIALS_DIRECTORY=str(host.root / "test-credentials/real")))
        assert rejected.returncode != 0 and "differs from its journal" in rejected.stderr
        assert str(config) in rejected.stderr
        config.write_text(original)
    finally:
        if terminal and terminal.poll() is None:
            terminal.communicate(timeout=15)
        for child in children.values():
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=15)
print("Independent trading connections isolate accounts, lifecycle and authorization")
