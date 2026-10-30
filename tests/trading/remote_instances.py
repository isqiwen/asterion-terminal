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


if sys.platform != "linux":
    raise SystemExit(77)
build = Path(sys.argv[1]).resolve()


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


with tempfile.TemporaryDirectory(prefix="trading-instances-", dir=os.environ["ASTERION_ROOT"], ignore_cleanup_errors=True) as folder:
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

        terminal = launch()
        call(terminal, "market.local")
        fingerprint = call(terminal, "trading.identity.prepare")["trading_fingerprint"]
        for name in ("real", "simulation"):
            port = host.account(name, terminals=[fingerprint], environment=name, user="000001" if name == "real" else "000002")
            children[name] = host.start(name)
            call(terminal, "trading.connections.save", dict(id=name, name=name, revision="",
                host="127.0.0.1", port=port, instance=name, fingerprint=host.server))
        for account in ("real", "simulation"):
            call(terminal, "ctp.connections.save", dict(id=account, name=account, revision="",
                broker_id="9999", user_id="000001" if account == "real" else "000002",
                app_id="test", trade_front="tcp://127.0.0.1:41205", market_front="tcp://127.0.0.1:1"))
        call(terminal, "market.catalog", dict(account="real", password="test-only", auth_code="test-only"))
        wait(terminal, lambda s: s["market"]["catalog"]["phase"] == "ready")
        create = dict(max_order_quantity="5", max_gross_quantity="10", max_working_orders="2",
                      max_price_deviation="0.02", contracts=[dict(venue="SHFE", symbol="rb2610")])
        # A mistyped server pin cannot reserve an account or reach creation.
        saved = next(c for c in call(terminal, "runtime.snapshot")["trading_connections"] if c["id"] == "real")
        call(terminal, "trading.connections.save", dict(saved, fingerprint=host.stranger))
        call(terminal, "live.open", dict(account="real"), error=True)
        inspected = call(terminal, "runtime.snapshot")
        assert "trading" not in next(a for a in inspected["ctp_connections"] if a["id"] == "real")
        revision = next(c["revision"] for c in inspected["trading_connections"] if c["id"] == "real")
        call(terminal, "trading.connections.save", dict(saved, revision=revision))
        call(terminal, "live.open", dict(account="real"))
        observed = call(terminal, "live.open", dict(account="simulation"))
        assert observed["live"]["real"]["account"]["environment"] == "real"
        assert observed["live"]["simulation"]["account"]["broker"]["user_id"] == "000002"
        assert observed["live"]["simulation"]["session"] is None
        call(terminal, "live.create", dict(create, account="real"))
        state = call(terminal, "live.create", dict(create, account="simulation"))
        assert state["live"]["real"]["connection"]["transport"] == "tcp_tls"
        assert state["live"]["simulation"]["connection"]["transport"] == "tcp_tls"
        for account in ("real", "simulation"):
            assert state["live"][account]["session"]["environment"] == account
            assert state["live"][account]["session"]["authorization"] is None
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
        assert (host.root / "data/simulation/journal.sqlite").is_file()
        assert not (local / "accounts/ctp/simulation/journal.sqlite").exists()
        real_pid = children["real"].pid
        # Counter settings cannot enter the create command from a Terminal.
        call(terminal, "live.create", dict(create, account="real", environment="simulation"), error=True)
        call(terminal, "live.close", dict(account="simulation"))
        state = call(terminal, "live.open", dict(account="simulation"))
        assert state["live"]["simulation"]["account"]["environment"] == "simulation"
        children["simulation"].terminate()
        children["simulation"].wait(timeout=15)
        os.kill(real_pid, 0)
        state = call(terminal, "runtime.snapshot")
        assert state["live"]["real"]["session"]["phase"] == "ready"
        call(terminal, "live.close", dict(account="simulation"))
        call(terminal, "live.open", dict(account="simulation"), error=True)
        children["simulation"] = host.start("simulation")
        state = call(terminal, "live.open", dict(account="simulation"))
        assert state["live"]["simulation"]["session"]["environment"] == "simulation"
        assert state["live"]["simulation"]["session"]["authorization"] is None
        # A new Terminal process reads the same reference; remote records stay remote.
        terminal.communicate(timeout=15)
        terminal = launch()
        state = call(terminal, "live.open", dict(account="simulation"))
        assert state["live"]["simulation"]["connection"]["transport"] == "tcp_tls"
        assert state["live"]["simulation"]["session"]["account_id"] == "simulation"
        # Changing a configured account must not silently reuse an existing journal.
        call(terminal, "live.close", dict(account="real"))
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
