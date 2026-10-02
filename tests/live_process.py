"""Agent-owned live CTP session through the Terminal ABI with the test-only trader SDK.

Covers the execution chain (authorization, catalog allowlist, risk), credentials
that never reach disk, logs or replies, and recovery after the session process is
killed: the restarted service waits for credentials and never resends orders.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

build = Path(sys.argv[1]).resolve()
suffix = ".dylib" if sys.platform == "darwin" else ".so"
env = dict(os.environ,
           ASTERION_CTP_LIBRARY=str(build / f"libasterion_test_ctp{suffix}"),
           ASTERION_CTP_CATALOG_LIBRARY=str(build / f"libasterion_test_ctp_trader{suffix}"))
SECRET = "live-secret-only"
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
    process = launch()
    try:
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
        create = dict(
                      max_order_quantity="5", max_gross_quantity="10", max_working_orders="1",
                      max_price_deviation="0.02")
        refused = trade(process, "live.create",
                       dict(create, contracts=[dict(venue="SHFE", symbol="rb2611")]), error=True)
        assert "catalog" in refused["message"], refused
        state = trade(process, "live.create",
                     dict(create, contracts=[dict(venue="SHFE", symbol="rb2610")]))
        live = session(state)
        assert live["phase"] == "disconnected" and live["authorization"] is None
        assert live["contracts"][0]["price_increment"] == "0.5"
        assert live["contracts"][0]["multiplier"] == "10"
        denied = trade(process, "live.act", submit("early", "1"), error=True)
        assert "authorize" in denied["message"], denied

        trade(process, "live.connect", dict(password="bad", auth_code=AUTH))
        wait(process, lambda s: session(s)["phase"] == "error")
        trade(process, "live.connect", dict(password=SECRET, auth_code=AUTH))
        wait(process, lambda s: session(s)["phase"] == "ready")
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
        trade(process, "live.create", dict(create, contracts=[dict(venue="SHFE", symbol="rb2610")]),
             account="second")
        state = call(process, "runtime.snapshot")
        assert set(state["live"]) == {"account", "second"}, list(state["live"])
        assert session(state)["authorization"] and session(state, "second")["phase"] == "disconnected"
        denied = trade(process, "live.act", submit("other", "1"), error=True, account="second")
        assert "authorize" in denied["message"], denied
        assert order(call(process, "runtime.snapshot"), "other") is None
        trade(process, "live.act", submit("stray", "1"), error=True, account="missing")
        # An account that trades keeps its counter details.
        fixed = call(process, "ctp.connections.save", dict(
            id="second", name="Second", revision=next(c["revision"] for c in state["ctp_connections"] if c["id"] == "second"),
            broker_id="9999", user_id="000003", app_id="client_app",
            trade_front="tcp://127.0.0.1:41205", market_front="tcp://127.0.0.1:1"), error=True)
        assert "fixed" in fixed["message"], fixed
        call(process, "ctp.connections.remove", dict(id="second", revision=next(
            c["revision"] for c in state["ctp_connections"] if c["id"] == "second")), error=True)
        trade(process, "live.close", account="second")

        # The session service is killed; the Agent restarts it without credentials.
        node = next(n for n in state["nodes"] if n["id"] == "local")
        service = next(s for s in node["health"]["services"]
                       if s["id"] == state["live"]["account"]["connection"]["session"])
        os.kill(service["pid"], signal.SIGKILL)
        wait(process, lambda s: any(
            v["id"] == service["id"] and v["pid"] != service["pid"] and v["health"] == "ready"
            for n in s["nodes"] if n["health"] for v in n["health"]["services"]), 40)
        trade(process, "live.close")
        state = trade(process, "live.open")
        live = session(state)
        assert live["phase"] == "disconnected" and live["authorization"] is None, live
        trade(process, "live.act", submit("after", "1"), error=True)
        trade(process, "live.connect", dict(password=SECRET, auth_code=AUTH))
        state = wait(process, lambda s: session(s)["phase"] == "ready")
        # The SDK double keeps exchange state in the killed process, so the
        # broker no longer reports the order: it is listed, never resent.
        live = session(state)
        assert [u["id"] for u in live["unconfirmed"]] == ["filled"], live
        assert live["orders"] == [], live
        trade(process, "live.act", submit("filled", "2"))
        assert session(call(process, "runtime.snapshot"))["orders"] == []
        trade(process, "live.disconnect")
        trade(process, "live.close")
    finally:
        stop(process)
    for path in Path(os.environ["ASTERION_NODE_DIRECTORY"]).rglob("*"):
        if path.is_file():
            data = path.read_bytes()
            assert SECRET.encode() not in data and AUTH.encode() not in data, path
print("Live CTP chain: catalog allowlist, authorization, risk, credential redaction and "
      "restart without resending verified")
