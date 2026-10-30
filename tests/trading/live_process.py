"""Independent live CTP session through the Terminal ABI with the test-only trader SDK.

Covers the execution chain (authorization, catalog allowlist, risk), credentials
that never reach disk, logs or replies, and recovery after the session process is
killed: the restarted service waits for credentials and never resends orders.
"""
import json
import os
import re
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
from trading_installation import install, Programs
if sys.platform != "linux":
    raise SystemExit(77)

build = Path(sys.argv[1]).resolve()
suffix = ".dylib" if sys.platform == "darwin" else ".so"
env = dict(os.environ,
           ASTERION_CTP_LIBRARY=str(build / f"libasterion_test_ctp{suffix}"),
           ASTERION_CTP_CATALOG_LIBRARY=str(build / f"libasterion_test_ctp_trader{suffix}"))
SECRET = "test-only-password"
AUTH = "auth-code"
bridge = str(build / "asterion_terminal_dev_bridge")


def launch():
    return subprocess.Popen([bridge], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)


def call(process, method, params=None, error=False):
    process.stdin.write(json.dumps(dict(version=1, method=method, params=params or {})) + "\n")
    process.stdin.flush()
    line = process.stdout.readline()
    assert SECRET not in line and AUTH not in line, line
    reply = json.loads(line)
    if error:
        assert "error" in reply, reply
        return reply["error"]
    assert "error" not in reply, reply
    return reply["result"]


def wait(process, predicate, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        state = call(process, "runtime.snapshot")
        if predicate(state):
            return state
        time.sleep(0.2)
    raise AssertionError(json.dumps(state)[:2000])


def session(state, account="account"):
    entry = state["live"].get(account)
    return entry["session"] if entry else None


def trade(process, method, params=None, error=False, account="account"):
    """Every trading command names the CTP account it is for."""
    if method == "live.act" and "account_id" not in (params or {}):
        current = session(call(process, "runtime.snapshot"), account)
        params = dict(params or {}, account_id=current["account_id"] if current else "missing")
    if method == "live.act" and "policy_revision" not in (params or {}):
        current = session(call(process, "runtime.snapshot"), account)
        params = dict(params or {}, policy_revision=current["policy_revision"] if current else "missing")
    return call(process, method, dict(params or {}, account=account), error=error)


def order(state, order_id):
    return next((o for o in session(state)["orders"] if o["id"] == order_id), None)


def submit(order_id, quantity, price="3500", symbol="rb2610"):
    return dict(request_id="submit." + order_id, action="submit", order_id=order_id,
                venue="SHFE", symbol=symbol, side="buy", offset="open",
                quantity=quantity, price=price)


def stop(process):
    stdout, stderr = process.communicate(timeout=15)  # closes stdin
    assert SECRET not in stdout + stderr and AUTH not in stdout + stderr


with tempfile.TemporaryDirectory(prefix="asterion-live-", ignore_cleanup_errors=True) as folder:
    # The application owns the log path; inherited logger settings cannot split
    # Terminal events from its services.
    env["ASTERION_LOG_DIRECTORY"] = str(Path(folder) / "terminal-logs")
    process = launch()
    host = install(Path(os.environ["ASTERION_ROOT"]) / "independent-trading", Programs(
        str(build / "asterion_trading"), str(build / "asterion_test_certificates"),
        str(build / "libasterion_test_ctp_trader.so")))
    children = {}
    try:
        fingerprint = call(process, "trading.identity.prepare")["trading_fingerprint"]
        for name in ("account", "second"):
            port = host.account(name, terminals=[fingerprint], user="000001" if name == "account" else "000002")
            (host.root / "test-credentials" / name / "auth_code").write_text(AUTH)
            children[name] = host.start(name)
            call(process, "trading.connections.save", dict(id=name, name=name, revision="",
                host="127.0.0.1", port=port, instance=name, fingerprint=host.server))
        call(process, "market.local")
        # The counter details are entered once; commands refer to them by id.
        call(process, "ctp.connections.save", dict(
            id="catalog", name="Catalog", revision="", broker_id="test", user_id="catalog",
            app_id="client_app", trade_front="tcp://127.0.0.1:1", market_front="tcp://127.0.0.1:1"))
        call(process, "ctp.connections.save", dict(
            id="account", name="Account", revision="", broker_id="9999", user_id="000001",
            app_id="client_app", trade_front="tcp://127.0.0.1:41205", market_front="tcp://127.0.0.1:1"))
        call(process, "market.catalog", dict(account="catalog", password="catalog-only",
                                             auth_code=AUTH))
        wait(process, lambda s: s["market"]["catalog"]["phase"] == "ready")
        call(process, "live.open", dict(account="account"))
        create = dict(
                      max_order_quantity="5", max_gross_quantity="10", max_working_orders="1",
                      max_price_deviation="0.02")
        refused = trade(process, "live.create",
                       dict(create, contracts=[dict(venue="SHFE", symbol="rb2611")]), error=True)
        assert "catalog" in refused["message"], refused
        state = trade(process, "live.create",
                     dict(create, contracts=[dict(venue="SHFE", symbol="rb2610")]))
        live = session(state)
        assert live["account_id"] == "account"
        assert live["authorization"] is None
        assert live["contracts"][0]["price_increment"] == "0.5"
        assert live["contracts"][0]["multiplier"] == "10"
        # Opening the same account twice must not create another record.
        duplicate = trade(process, "live.create", dict(create, contracts=[dict(venue="SHFE", symbol="rb2610")]))
        assert session(duplicate)["account_id"] == "account"
        # A created account logs in by itself with its host's credentials; each
        # step of that login renews the account's control, so wait for its outcome
        # before the next order.
        wait(process, lambda s: session(s)["phase"] in ("ready", "error"))
        denied = trade(process, "live.act", submit("early", "1"), error=True)
        assert "authorize" in denied["message"], denied

        rejected = trade(process, "live.connect", dict(password="bad", auth_code=AUTH), error=True)
        assert rejected, "Terminal must reject credentials on the trading API"
        state = wait(process, lambda s: s["live"]["account"]["connection"]["health"]["phase"] == "ready")
        observed = state["live"]["account"]["connection"]["health"]
        assert observed["execution"]["business_ready"], observed
        state = wait(process, lambda s: session(s)["phase"] == "ready"
                     and session(s)["positions_reconciled"])
        broker_observation = session(state)
        assert isinstance(broker_observation["broker_sequence"], str) and int(broker_observation["broker_sequence"]) > 0, broker_observation
        assert isinstance(broker_observation["connection_generation"], str), broker_observation
        assert int(broker_observation["connection_generation"]) >= 0, broker_observation
        assert all(observed["execution"][key]["observed"] for key in ("io", "state", "persistence"))
        trade(process, "live.act", dict(request_id="authorize", action="live_authorize",
                                       user_id="000001"))
        trade(process, "live.act", submit("tick", "1", "3500.25"), error=True)
        far = trade(process, "live.act", submit("far", "1", "3600"), error=True)
        assert "deviates" in far["message"], far
        risk = trade(process, "live.act", submit("large", "6"), error=True)
        assert "order_quantity" in risk["message"], risk
        trade(process, "live.act", submit("filled", "2"))
        state = wait(process, lambda s: order(s, "filled") and order(s, "filled")["status"] ==
                     "filled" and session(s)["positions"])
        assert session(state)["positions"][0]["today"] == "2"

        # A second account trades at the same time with its own record, service
        # and authorization: an order names its account and cannot use another's.
        call(process, "ctp.connections.save", dict(
            id="second", name="Second", revision="", broker_id="9999", user_id="000002",
            app_id="client_app", trade_front="tcp://127.0.0.1:41205", market_front="tcp://127.0.0.1:1"))
        call(process, "live.open", dict(account="second"))
        trade(process, "live.create", dict(create, contracts=[dict(venue="SHFE", symbol="rb2610")]),
             account="second")
        state = wait(process, lambda s: session(s, "second")["phase"] == "ready")
        assert set(state["live"]) == {"account", "second"}, list(state["live"])
        assert session(state)["authorization"] and session(state, "second")["authorization"] is None
        denied = trade(process, "live.act", submit("other", "1"), error=True, account="second")
        assert "authorize" in denied["message"], denied
        assert order(call(process, "runtime.snapshot"), "other") is None
        trade(process, "live.act", submit("stray", "1"), error=True, account="missing")
        # Market registration is independent of the service-owned trading identity.
        changed = next(c for c in state["ctp_connections"] if c["id"] == "second")
        call(process, "ctp.connections.save", dict(changed, user_id="000003"))
        assert session(call(process, "runtime.snapshot"), "second")["broker"]["user_id"] == "000002"
        call(process, "ctp.connections.remove", dict(id="second", revision=next(c["revision"] for c in call(process, "runtime.snapshot")["ctp_connections"] if c["id"] == "second")))
        trade(process, "live.close", account="second")

        # Restart the independently managed host: local login resumes, authorization does not.
        children["account"].kill()
        children["account"].wait(timeout=15)
        children["account"] = host.start("account")
        trade(process, "live.close")
        state = trade(process, "live.open")
        live = session(state)
        assert live["authorization"] is None, live
        assert isinstance(live["broker_sequence"], str), live
        assert isinstance(live["connection_generation"], str), live
        trade(process, "live.act", submit("after", "1"), error=True)
        state = wait(process, lambda s: session(s)["phase"] == "ready")
        # The restarted SDK double has no exchange state. Durable terminal
        # evidence still retires the filled intent; recovery never resends it.
        live = session(state)
        assert live["unconfirmed"] == [], live
        assert live["orders"] == [], live
        trade(process, "live.act", submit("filled", "2"))
        assert session(call(process, "runtime.snapshot"))["orders"] == []
        trade(process, "live.disconnect")
        trade(process, "live.close")
    finally:
        stop(process)
        for child in children.values():
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=15)
    # Each program logs in its own installation: <root>/<program>/logs, one text
    # line per record whose message ends with key=value fields.
    root = Path(os.environ["ASTERION_ROOT"])
    assert not (Path(folder) / "terminal-logs").exists()
    assert list((root / "terminal" / "logs").glob("terminal-*.log")), "Terminal logs missing"
    records = []
    for path in root.glob("*/logs/*.log"):
        for line in path.read_text(errors="replace").splitlines():
            records.append((line, dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))))

    def find(message, **expected):
        return next(fields for line, fields in records if message in line
                    and all(fields.get(key) == value for key, value in expected.items()))

    committed = find("journal committed:", request_id="submit.filled")
    rpc = find("rpc completed:", correlation_id=committed["trace_id"])
    assert rpc["request_id"] == "submit.filled", rpc
    link = find("rpc started:", correlation_id=committed["trace_id"])
    assert any("live.act completed:" in line and fields.get("trace_id") == link["trace_id"]
               for line, fields in records), "RPC was not joined to its Terminal command"
    assert any("broker order observed:" in line and fields.get("broker_key") == committed["broker_key"]
               and fields.get("order_id") == "filled" for line, fields in records)
    assert any("broker trade observed:" in line and fields.get("order_id") == "filled"
               for line, fields in records)
    for path in Path(os.environ["ASTERION_ROOT"]).rglob("*"):
        if path.is_file() and "test-credentials" not in path.parts:
            data = path.read_bytes()
            assert SECRET.encode() not in data and AUTH.encode() not in data, path
print("Live CTP chain: catalog allowlist, authorization, risk, credential redaction and "
      "restart without resending verified")
