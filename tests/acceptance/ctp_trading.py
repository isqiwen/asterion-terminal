"""Acceptance: CTP trading against a provider's SIMULATION counter.

Runs the Terminal against independently deployed Market and Trading services.
All provider credentials stay on their service hosts. This script sends orders;
use it only after checking that Trading is configured for simulation.

Checks, in order: market quotes; account synchronization; an order refused
before authorization; one resting limit order accepted and cancelled; the
authorization kept across a disconnect and reconnect; revoking it. Run it
during trading hours. The contract's position is reported at the end and left as
it is.
"""
import argparse
from decimal import Decimal, ROUND_FLOOR
import getpass
import json
import re
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid


ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("--simulation", action="store_true",
                    help="required: you checked that both fronts are a simulation environment")
parser.add_argument("--build", default=str(ROOT / "build/Debug"))
parser.add_argument("--trading-connection", required=True, help="saved Terminal trading connection id")
parser.add_argument("--terminal-directory", required=True, help="Terminal installation containing data/tls and data/trading-connections")
parser.add_argument("--market-host", required=True)
parser.add_argument("--market-port", type=int, default=7450)
parser.add_argument("--market-fingerprint", required=True)
parser.add_argument("--trade-front", required=True)
parser.add_argument("--broker", required=True)
parser.add_argument("--user", required=True)
parser.add_argument("--instrument", required=True, help="VENUE:SYMBOL")
parser.add_argument("--deviation", default="0.02", help="price deviation limit of the test policy")
parser.add_argument("--timeout", type=int, default=90, help="seconds to wait for each step")
parser.add_argument("--report", default="", help="write a JSON report (no credentials)")
args = parser.parse_args()
if not args.simulation:
    sys.exit("this tool sends orders; pass --simulation after checking the fronts are a "
             "simulation environment")

build = Path(args.build).resolve()
bridge = build / "asterion_terminal_dev_bridge"
if not bridge.exists():
    sys.exit(f"missing {bridge}; build the project first")
venue, _, symbol = args.instrument.partition(":")
if not venue or not symbol:
    sys.exit(f"instrument must be VENUE:SYMBOL, got {args.instrument!r}")
report = {"warehouse_host": args.warehouse_host, "trade_front": args.trade_front,
          "broker": args.broker, "instrument": args.instrument,
          "started_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "passed": False, "steps": []}
if args.report:
    # A startup or cleanup failure must never leave a previous passing report.
    Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
with tempfile.TemporaryDirectory(prefix="asterion-ctp-", delete=False) as folder:
    env = dict(os.environ, ASTERION_ROOT=folder, ASTERION_TEST_NODE_ISOLATED="1")
    source = Path(args.terminal_directory).resolve() / "data"
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.trading_connection):
        sys.exit("invalid trading connection id")
    destination = Path(folder) / "terminal/data"
    (destination / "trading-connections").mkdir(parents=True)
    (destination / "tls").mkdir(mode=0o700)
    shutil.copy2(source / "trading-connections" / (args.trading_connection + ".json"),
                 destination / "trading-connections" / (args.trading_connection + ".json"))
    for name in ("client.crt", "client.key"):
        shutil.copy2(source / "tls" / name, destination / "tls" / name)
    core = subprocess.Popen([str(bridge)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, env=env)
    begin = time.monotonic()

    def call(method, params=None):
        request = {"version": 1, "method": method, "params": params or {}}
        core.stdin.write(json.dumps({"id": 1, "request": request}) + "\n")
        core.stdin.flush()
        response = json.loads(core.stdout.readline())["reply"]
        if "error" in response:
            raise RuntimeError(f"{method}: {response['error']['code']}: {response['error']['message']}")
        return response["result"]

    def step(name, ok, detail=""):
        report["steps"].append({"step": name, "ok": bool(ok), "detail": str(detail),
                                "at": round(time.monotonic() - begin, 1)})
        print(f"[{'ok' if ok else 'FAIL'}] {name} {detail}", flush=True)
        return ok

    def trading(request):
        """One trading.proto Request; the Response as ProtoJSON (defaults omitted)."""
        return call("trading.call", {"account": ACCOUNT, "request": request})["trading"]

    def account():
        return trading({"getAccount": {}})["getAccount"]

    def session():
        return account().get("session")

    def number(value):
        return Decimal((value or {}).get("text", "0"))

    def decimal(value):
        return {"text": str(value)}

    def state(live):
        return live["brokerState"].removeprefix("BROKER_STATE_").lower()

    def order_state(item):
        return item["state"].removeprefix("ORDER_STATE_").lower()

    def wait(predicate, seconds=None):
        """The session once the predicate holds, or None after the deadline."""
        deadline = time.monotonic() + (seconds or args.timeout)
        while time.monotonic() < deadline:
            live = session()
            if live and predicate(live):
                return live
            time.sleep(0.5)
        return None

    def act(command):
        current = account()
        return trading({"sendCommand": dict(command, requestId=str(uuid.uuid4()), accountId=current["id"],
                                            policyRevision=current["session"]["policyRevision"])})

    def refused(name, expected, command):
        try:
            act(command)
        except RuntimeError as error:
            return step(name, expected in str(error), str(error))
        return step(name, False, "the command was accepted")

    def order(live, order_id):
        return next((item for item in live.get("orders", []) if item.get("id") == order_id), None)

    def held(live):
        return sum(number(p.get("todayLots")) + number(p.get("yesterdayLots"))
                   for p in live.get("positions", [])
                   if p["instrument"] == {"exchange": venue, "symbol": symbol} and p["side"] == "SIDE_BUY")

    def market():
        """Market's quotes and catalog through the Terminal relay, as ProtoJSON."""
        return call("market.call", {"request": {"getBoard": {}}})["market"]["getBoard"]

    def quote():
        return next((row.get("quote") for row in market().get("rows", [])
                     if row["instrument"] == {"exchange": venue, "symbol": symbol}), None)

    try:
        call("market.endpoint.save", {"host": args.warehouse_host, "port": args.market_port, "fingerprint": args.market_fingerprint})
        step("remote Market connection", bool(market().get("feed")))
        deadline = time.monotonic() + args.timeout
        while not (quote() or {}).get("last") and time.monotonic() < deadline:
            time.sleep(1)
        first = quote() or {}
        if not step("market quote", bool(first.get("last")),
                    f"last={first.get('last', {}).get('text')} trading_day={first.get('tradingDay')}"):
            raise RuntimeError("no market quote: run during trading hours with a listed contract")

        # The counter may pause for two minutes before or between instruments.
        deadline = time.monotonic() + max(args.timeout, 600)
        while time.monotonic() < deadline:
            catalog = market().get("catalog", {})
            state = catalog.get("status", {}).get("state")
            if state in ("CATALOG_STATE_READY", "CATALOG_STATE_FAILED"):
                break
            time.sleep(1)
        # The counter's refusal is the only evidence: the node and its logs are temporary.
        detail = " ".join(str(value) for value in catalog.get("status", {}).values())
        if not step("contract catalog", state == "CATALOG_STATE_READY", detail):
            raise RuntimeError("the contract catalog did not load")
        configured = account()
        broker = configured.get("broker", {})
        if configured.get("environment") != "ENVIRONMENT_SIMULATION" or broker.get("userId") != args.user or broker.get("brokerId") != args.broker or broker.get("front") != args.trade_front:
            raise RuntimeError("configured trading service does not match the explicitly selected simulation counter")
        # Exchange terms from the CTP catalog, as the trading page builds them.
        row = next(item for item in catalog["contracts"]
                   if item["instrument"] == {"exchange": venue, "symbol": symbol})
        _, product, delivery_month = row["contract"].split("/")
        trading({"openAccount": {
            "limits": {"maxOrderLots": decimal(1), "maxGrossLots": decimal(1), "maxWorkingOrders": 1},
            "maxPriceDeviation": decimal(args.deviation),
            "contracts": [{"instrument": row["instrument"], "product": product,
                           "deliveryMonth": delivery_month, "multiplier": decimal(row["multiplier"]),
                           "priceTick": row["priceTick"], "lotSize": decimal(1)}]}})
        live = wait(lambda s: state(s) in ("ready", "failed"))
        step("account record created", bool(live), "one lot per order, one lot held, one working order")
        tick = number(live["policy"]["contracts"][0]["priceTick"])

        live = wait(lambda s: state(s) in ("ready", "failed"))
        if not step("account synchronized", live and state(live) == "ready",
                    f"state={live and state(live)} ctp_error={live and live.get('ctpError', 0)} "
                    f"trading_day={live and live.get('tradingDay')}"):
            raise RuntimeError("the account did not synchronize")
        step("funds and positions reported", "funds" in live,
             f"available={number(live.get('funds', {}).get('available'))} long={held(live)}")
        step("market and account trading day", first.get("tradingDay") == live.get("tradingDay"),
             f"market={first.get('tradingDay')} account={live.get('tradingDay')}")

        last = number(quote()["last"])
        # Well below the market yet inside the policy's deviation and the exchange limit.
        resting = (last * (1 - Decimal(args.deviation) * Decimal("0.8")) / tick).to_integral_value(
            ROUND_FLOOR) * tick
        if quote().get("lowerLimit"):
            resting = max(resting, number(quote()["lowerLimit"]))
        def manual(order_id):
            return {"submitOrder": {"orderId": order_id, "instrument": {"exchange": venue, "symbol": symbol},
                                    "side": "SIDE_BUY", "offset": "OFFSET_OPEN", "lots": decimal(1),
                                    "price": decimal(resting)}}
        refused("order refused before authorization", "authorize", manual("early"))
        act({"authorize": {"userId": args.user}})
        step("authorized", "authorizedMs" in session())

        if held(live) == 0 and not any(order_state(o) in ("submitted", "accepted", "partially_filled")
                                       for o in live.get("orders", [])):
            act(manual("resting"))
            live = wait(lambda s: order(s, "resting") and order_state(order(s, "resting")) != "submitted")
            status = live and order_state(order(live, "resting"))
            step("resting order accepted by the counter", status == "accepted",
                 f"price={resting} status={status}")
            if status == "accepted":
                act({"cancelOrder": {"id": "resting"}})
                live = wait(lambda s: order_state(order(s, "resting")) == "cancelled")
                step("resting order cancelled", bool(live))
        else:
            step("resting order", False, "skipped: the contract already has a position or orders")

        trading({"disconnectBroker": {}})
        live = wait(lambda s: state(s) == "disconnected")
        step("authorization kept while disconnected", live and "authorizedMs" in live)
        trading({"connectBroker": {}})
        live = wait(lambda s: state(s) in ("ready", "failed"))
        if not step("reconnected without a new authorization",
                    live and state(live) == "ready" and "authorizedMs" in live,
                    f"state={live and state(live)}"):
            raise RuntimeError("the account did not synchronize again")

        live = session()
        step("no unconfirmed orders", not live.get("unconfirmed"), live.get("unconfirmed"))
        print(f"       position kept: long {held(live)} {args.instrument}", flush=True)
        report["position_kept"] = str(held(live))
        act({"revoke": {}})
        step("authorization revoked", "authorizedMs" not in session())
        trading({"disconnectBroker": {}})
    except Exception as error:  # report, never the credentials
        step("run", False, str(error))
    finally:
        try:
            # The services the core started exit with it.
            core.stdin.close()
            core.terminate()
            core.wait(timeout=15)
        except Exception as error:
            step("cleanup", False, str(error))
    report["passed"] = all(item["ok"] for item in report["steps"])
    if args.report:
        Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    if report["passed"]:
        shutil.rmtree(folder, ignore_errors=True)
    else:
        print(f"node kept for its logs: {folder}/<program>/logs")
    print("PASSED" if report["passed"] else "FAILED")
    sys.exit(0 if report["passed"] else 1)
