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
from market_installation import market_host
from trading_requests import account_policy, market_catalog, submit
import market_tls


if sys.platform != "linux":
    raise SystemExit(77)
build = Path(sys.argv[1]).resolve()


def call(process, method, params=None, error=False):
    process.stdin.write(json.dumps(dict(version=1, method=method, params=params or {})) + "\n")
    process.stdin.flush()
    reply = json.loads(process.stdout.readline())
    assert ("error" in reply) == error, reply
    return reply["error"] if error else reply["result"]


def trading(process, account, request, error=False):
    """One trading.proto Request for the named account; the Response as ProtoJSON."""
    reply = call(process, "trading.call", dict(account=account, request=request), error=error)
    return reply if error else reply["trading"]


def session(process, account):
    return trading(process, account, {"attach": {}}).get("live")


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
        market_tls.install_client(tls, local)
        terminal = launch()
        call(terminal, "market.endpoint.save", dict(host="127.0.0.1", port=market_port, fingerprint=market_tls.fingerprint(tls)))
        call(terminal, "market.open")
        fingerprint = call(terminal, "runtime.snapshot")["client_fingerprint"]
        for name in ("real", "simulation"):
            port = host.account(name, terminals=[fingerprint], environment=name, user="000001" if name == "real" else "000002")
            children[name] = host.start(name)
            call(terminal, "trading.connections.save", dict(id=name, name=name, revision="",
                host="127.0.0.1", port=port, instance=name, fingerprint=host.server))
        create = account_policy(market_catalog(lambda *a: call(terminal, *a)))
        # A mistyped server pin cannot reach the account.
        saved = next(c for c in call(terminal, "runtime.snapshot")["trading_connections"] if c["id"] == "real")
        call(terminal, "trading.connections.save", dict(saved, fingerprint=host.stranger))
        trading(terminal, "real", {"attach": {}}, error=True)
        inspected = call(terminal, "runtime.snapshot")
        revision = next(c["revision"] for c in inspected["trading_connections"] if c["id"] == "real")
        call(terminal, "trading.connections.save", dict(saved, revision=revision))
        real = trading(terminal, "real", {"attach": {}})
        simulation = trading(terminal, "simulation", {"attach": {}})
        assert real["account"]["environment"] == "REAL"
        assert simulation["account"]["broker"]["userId"] == "000002"
        assert "uninitialized" in simulation
        environments = {"real": "REAL", "simulation": "BROKER_SIMULATION"}
        for account in ("real", "simulation"):
            live = trading(terminal, account, create)["live"]
            assert live["environment"] == environments[account]
            assert "authorization" not in live
        wait(lambda: all(session(terminal, a)["phase"] == "ready" for a in ("real", "simulation")))
        # Authorizing one account cannot authorize the other.
        live = session(terminal, "simulation")
        reviewed = {"accountId": live["accountId"], "policyRevision": live["policyRevision"]}
        trading(terminal, "simulation", dict(reviewed, command={
            "requestId": "allow.simulation", "liveAuthorize": {"userId": "000002"}}))
        assert "authorization" in session(terminal, "simulation")
        assert "authorization" not in session(terminal, "real")
        # Desktop exit is not a trading control: the independent host retains
        # authorization and can continue to execute after a fresh client attaches.
        terminal.communicate(timeout=15)
        assert all(process.poll() is None for process in children.values())
        terminal = launch()
        assert "authorization" in session(terminal, "simulation")
        assert "authorization" not in session(terminal, "real")
        trading(terminal, "simulation", dict(reviewed, command=submit("remote-order", 1)))
        wait(lambda: any(o.get("id") == "remote-order" and o["status"] == "filled"
                         for o in session(terminal, "simulation").get("orders", [])))
        assert "orders" not in session(terminal, "real")
        assert (host.root / "data/simulation/journal.sqlite").is_file()
        assert not (local / "accounts/ctp/simulation/journal.sqlite").exists()
        real_pid = children["real"].pid
        # Counter settings cannot enter the create request from a Terminal.
        trading(terminal, "real", dict(create, environment="BROKER_SIMULATION"), error=True)
        children["simulation"].terminate()
        children["simulation"].wait(timeout=15)
        os.kill(real_pid, 0)
        assert session(terminal, "real")["phase"] == "ready"
        trading(terminal, "simulation", {"attach": {}}, error=True)
        children["simulation"] = host.start("simulation")
        live = session(terminal, "simulation")
        assert live["environment"] == "BROKER_SIMULATION"
        assert "authorization" not in live
        # A new Terminal process reads the same reference; remote records stay remote.
        terminal.communicate(timeout=15)
        terminal = launch()
        assert session(terminal, "simulation")["accountId"] == "simulation"
        # Changing a configured account must not silently reuse an existing journal.
        children["real"].terminate(); children["real"].wait(timeout=15)
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
