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
from market_installation import market_host
from trading_requests import account_policy, market_catalog, submit, units
import market_tls
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


def trading(process, request, error=False, account="account"):
    """One trading.proto Request for the named account; the Response as ProtoJSON."""
    reply = call(process, "trading.call", dict(account=account, request=request), error=error)
    return reply if error else reply["trading"]


def session(process, account="account"):
    return trading(process, {"attach": {}}, account=account).get("live")


def wait(process, predicate, seconds=20, account="account"):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        live = session(process, account)
        if live and predicate(live):
            return live
        time.sleep(0.2)
    raise AssertionError(json.dumps(live)[:2000])


def act(process, command, error=False, account="account"):
    """A trading command names the account record and the policy it was reviewed under."""
    live = session(process, account) or {}
    return trading(process, {"accountId": live.get("accountId", "missing"),
                             "policyRevision": live.get("policyRevision", "missing"),
                             "command": command}, error=error, account=account)


def order(live, order_id):
    return next((o for o in live.get("orders", []) if o.get("id") == order_id), None)


def stop(process):
    stdout, stderr = process.communicate(timeout=15)  # closes stdin
    assert SECRET not in stdout + stderr and AUTH not in stdout + stderr


with tempfile.TemporaryDirectory(prefix="asterion-live-", ignore_cleanup_errors=True) as folder, market_host(build, Path(folder) / "market") as market_port:
    # The application owns the log path; inherited logger settings cannot split
    # Terminal events from its services.
    env["ASTERION_LOG_DIRECTORY"] = str(Path(folder) / "terminal-logs")
    tls = Path(folder) / "market/config/tls"
    market_tls.install_client(tls, env["ASTERION_ROOT"])
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
        call(process, "market.endpoint.save", dict(host="127.0.0.1", port=market_port, fingerprint=market_tls.fingerprint(tls)))
        call(process, "market.open")
        create = account_policy(market_catalog(lambda *a: call(process, *a)), working_orders="1")
        live = trading(process, create)["live"]
        assert live["accountId"] == "account"
        assert "authorization" not in live
        assert live["contracts"][0]["priceIncrement"] == units("0.5")
        # An account has one record: it cannot be created again.
        duplicate = trading(process, create, error=True)
        assert "already initialized" in duplicate["message"], duplicate
        # A created account logs in by itself with its host's credentials; each
        # step of that login renews the account's control, so wait for its outcome
        # before the next order.
        wait(process, lambda s: s["phase"] in ("ready", "error"))
        denied = act(process, submit("early", "1"), error=True)
        assert "authorize" in denied["message"], denied

        rejected = trading(process, {"liveConnect": {"password": "bad", "authCode": AUTH}},
                           error=True)
        assert rejected, "Terminal must reject credentials on the trading API"
        health = trading(process, {"heartbeat": {}})["health"]
        assert health["initialized"] and health["execution"]["businessReady"], health
        assert all(health["execution"][key]["observed"] for key in ("io", "state", "persistence"))
        broker_observation = wait(process, lambda s: s["phase"] == "ready"
                                  and s.get("positionsReconciled"))
        assert int(broker_observation["brokerSequence"]) > 0, broker_observation
        act(process, {"requestId": "authorize", "liveAuthorize": {"userId": "000001"}})
        act(process, submit("tick", "1", "3500.25"), error=True)
        far = act(process, submit("far", "1", "3600"), error=True)
        assert "deviates" in far["message"], far
        risk = act(process, submit("large", "6"), error=True)
        assert "order_quantity" in risk["message"], risk
        act(process, submit("filled", "2"))
        live = wait(process, lambda s: order(s, "filled") and order(s, "filled")["status"] ==
                    "filled" and s.get("positions"))
        assert live["positions"][0]["today"] == units(2)

        # A second account trades at the same time with its own record, service
        # and authorization: an order names its account and cannot use another's.
        trading(process, create, account="second")
        wait(process, lambda s: s["phase"] == "ready", account="second")
        assert "authorization" in session(process)
        assert "authorization" not in session(process, "second")
        denied = act(process, submit("other", "1"), error=True, account="second")
        assert "authorize" in denied["message"], denied
        assert order(session(process), "other") is None
        trading(process, {"attach": {}}, error=True, account="missing")

        # Restart the independently managed host: local login resumes, authorization does not.
        children["account"].kill()
        children["account"].wait(timeout=15)
        children["account"] = host.start("account")
        assert "authorization" not in session(process)
        act(process, submit("after", "1"), error=True)
        live = wait(process, lambda s: s["phase"] == "ready")
        # The restarted SDK double has no exchange state. Durable terminal
        # evidence still retires the filled intent; recovery never resends it.
        assert "unconfirmed" not in live and "orders" not in live, live
        act(process, submit("filled", "2"))
        assert "orders" not in session(process)
        trading(process, {"liveDisconnect": {}})
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
    assert any("trading.call completed:" in line and fields.get("trace_id") == link["trace_id"]
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
