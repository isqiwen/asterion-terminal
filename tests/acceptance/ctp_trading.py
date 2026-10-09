"""Acceptance: CTP trading and a strategy run against a provider's SIMULATION counter.

Runs the real Terminal core (dev bridge) with the packaged vendor SDKs inside an
isolated node directory and sends real orders to the counter you name. Use it
only with a simulation environment: --simulation states that you checked this.
Passwords and the authentication code are read with getpass, passed only to the
local core over its private pipe, and never written to disk, logs or the report.
The market password and the trading password are asked for separately.

    python3 tests/acceptance/ctp_trading.py --simulation [--build build/Debug] \
        --market-front tcp://<host>:<port> --trade-front tcp://<host>:<port> \
        --broker <broker-id> --user <user-id> --app-id <app-id> \
        --instrument SHFE:rb2610 [--minutes 8] [--fast 1 --slow 2]

Checks, in order: market quotes; account synchronization; an order refused
before authorization; one resting limit order accepted and cancelled; the
authorization kept across a disconnect and reconnect; a strategy run that takes
the account, refuses manual orders, receives minute bars and places its orders
at the deciding bar's close; stopping it. Run it during trading hours. The
contract's position is reported at the end and left as it is.
"""
import argparse
from decimal import Decimal, ROUND_FLOOR
import getpass
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import uuid


def stop_test_agent(directory):
    """Stop only the Agent created in this run's private temporary directory."""
    pidfile = Path(directory) / "agent.pid"
    if not pidfile.exists():
        return
    pid = int(pidfile.read_text())
    if pid <= 1 or pid == os.getpid():
        raise RuntimeError("invalid isolated Agent process identity")
    try:
        os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            os.kill(pid, 0)
            time.sleep(0.05)
    except ProcessLookupError:
        return
    raise RuntimeError("isolated Agent did not exit")


ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("--simulation", action="store_true",
                    help="required: you checked that both fronts are a simulation environment")
parser.add_argument("--build", default=str(ROOT / "build/Debug"))
parser.add_argument("--market-front", required=True)
parser.add_argument("--trade-front", required=True)
parser.add_argument("--broker", required=True)
parser.add_argument("--user", required=True)
parser.add_argument("--app-id", required=True)
parser.add_argument("--instrument", required=True, help="VENUE:SYMBOL")
parser.add_argument("--minutes", type=int, default=8, help="minutes to watch the strategy run")
parser.add_argument("--fast", type=int, default=1)
parser.add_argument("--slow", type=int, default=2)
parser.add_argument("--deviation", default="0.02", help="price deviation limit of the test policy")
parser.add_argument("--timeout", type=int, default=90, help="seconds to wait for each step")
parser.add_argument("--report", default="", help="write a JSON report (no credentials)")
parser.add_argument("--market-sdk", default="", help="default: the packaged vendor SDK")
parser.add_argument("--trader-sdk", default="", help="default: the packaged vendor SDK")
parser.add_argument("--credentials-stdin", action="store_true",
                    help="read market password, trading password and authentication code "
                         "from three lines of standard input (automation only)")
args = parser.parse_args()
if not args.simulation:
    sys.exit("this tool sends orders; pass --simulation after checking the fronts are a "
             "simulation environment")

build = Path(args.build).resolve()
bridge = build / "asterion_terminal_dev_bridge"
suffix = ".dylib" if sys.platform == "darwin" else ".so"
market_sdk = Path(args.market_sdk).resolve() if args.market_sdk else build / ("ctp-md" + suffix)
trader_sdk = Path(args.trader_sdk).resolve() if args.trader_sdk else build / ("ctp-trader" + suffix)
for required in (bridge, market_sdk, trader_sdk):
    if not required.exists():
        sys.exit(f"missing {required}; build the project first")
venue, _, symbol = args.instrument.partition(":")
if not venue or not symbol:
    sys.exit(f"instrument must be VENUE:SYMBOL, got {args.instrument!r}")
contract = {"venue": venue, "symbol": symbol}
report = {"market_front": args.market_front, "trade_front": args.trade_front,
          "broker": args.broker, "instrument": args.instrument,
          "started_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "passed": False, "steps": []}
if args.report:
    # A startup or cleanup failure must never leave a previous passing report.
    Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
if args.credentials_stdin:
    market_password, trading_password, auth_code = (sys.stdin.readline().rstrip("\n")
                                                    for _ in range(3))
else:
    market_password = getpass.getpass("Market password (not echoed, not stored): ")
    trading_password = getpass.getpass("Trading password (not echoed, not stored): ")
    auth_code = getpass.getpass("Authentication code (not echoed, not stored): ")
ACCOUNT = "acceptance"

with tempfile.TemporaryDirectory(prefix="asterion-ctp-", ignore_cleanup_errors=True) as folder:
    env = dict(os.environ, ASTERION_NODE_DIRECTORY=folder, ASTERION_TEST_NODE_ISOLATED="1",
               ASTERION_CTP_LIBRARY=str(market_sdk), ASTERION_CTP_CATALOG_LIBRARY=str(trader_sdk))
    core = subprocess.Popen([str(bridge)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, env=env)
    begin = time.monotonic()

    def call(method, params=None):
        core.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n")
        core.stdin.flush()
        response = json.loads(core.stdout.readline())
        if "error" in response:
            raise RuntimeError(f"{method}: {response['error']['code']}: {response['error']['message']}")
        return response["result"]

    def step(name, ok, detail=""):
        report["steps"].append({"step": name, "ok": bool(ok), "detail": str(detail),
                                "at": round(time.monotonic() - begin, 1)})
        print(f"[{'ok' if ok else 'FAIL'}] {name} {detail}", flush=True)
        return ok

    def session():
        entry = call("runtime.snapshot")["live"].get(ACCOUNT)
        return entry["session"] if entry else None

    def wait(predicate, seconds=None):
        """The session once the predicate holds, or None after the deadline."""
        deadline = time.monotonic() + (seconds or args.timeout)
        while time.monotonic() < deadline:
            live = session()
            if live and predicate(live):
                return live
            time.sleep(0.5)
        return None

    def act(**command):
        live = session()
        return call("live.act", dict(command, account=ACCOUNT, request_id=str(uuid.uuid4()),
                                     account_id=live["account_id"],
                                     policy_revision=live["policy_revision"]))

    def refused(name, expected, **command):
        try:
            act(**command)
        except RuntimeError as error:
            return step(name, expected in str(error), str(error))
        return step(name, False, "the command was accepted")

    def order(live, order_id):
        return next((item for item in live["orders"] if item["id"] == order_id), None)

    def held(live):
        return sum(Decimal(p["today"]) + Decimal(p["yesterday"]) for p in live["positions"]
                   if p["venue"] == venue and p["symbol"] == symbol and p["side"] == "buy")

    def quote():
        market = call("runtime.snapshot")["market"] or {}
        return next((sub.get("quote") for sub in market.get("subscriptions", [])
                     if sub["venue"] == venue and sub["symbol"] == symbol), None)

    try:
        step("local market service", bool(call("market.local").get("market")))
        call("ctp.connections.save", {"id": ACCOUNT, "name": ACCOUNT, "revision": "",
                                      "broker_id": args.broker, "user_id": args.user,
                                      "app_id": args.app_id, "trade_front": args.trade_front,
                                      "market_front": args.market_front})
        call("market.connect", {"password": market_password, "instruments": [contract]})
        market_password = ""
        deadline = time.monotonic() + args.timeout
        while not (quote() or {}).get("last") and time.monotonic() < deadline:
            time.sleep(1)
        first = quote() or {}
        if not step("market quote", bool(first.get("last")),
                    f"last={first.get('last')} trading_day={first.get('trading_day')}"):
            raise RuntimeError("no market quote: run during trading hours with a listed contract")

        call("market.catalog", {"account": ACCOUNT, "password": trading_password,
                                "auth_code": auth_code})
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            phase = (call("runtime.snapshot")["market"] or {}).get("catalog", {}).get("phase")
            if phase in ("ready", "error"):
                break
            time.sleep(1)
        if not step("contract catalog", phase == "ready", phase):
            raise RuntimeError("the contract catalog did not load")
        call("live.create", {"account": ACCOUNT, "max_order_quantity": "1",
                             "max_gross_quantity": "1", "max_working_orders": "1",
                             "max_price_deviation": args.deviation, "contracts": [contract]})
        live = wait(lambda s: s["phase"] == "disconnected")
        step("account record created", bool(live), "one lot per order, one lot held, one working order")
        tick = Decimal(live["contracts"][0]["price_increment"])

        call("live.connect", {"account": ACCOUNT, "password": trading_password,
                              "auth_code": auth_code})
        live = wait(lambda s: s["phase"] in ("ready", "error"))
        if not step("account synchronized", live and live["phase"] == "ready",
                    f"phase={live and live['phase']} error_code={live and live['error_code']} "
                    f"trading_day={live and live['trading_day']}"):
            raise RuntimeError("the account did not synchronize")
        step("funds and positions reported", live["funds"] is not None,
             f"available={live['funds'] and live['funds']['available']} long={held(live)}")
        step("market and account trading day", first.get("trading_day") == live["trading_day"],
             f"market={first.get('trading_day')} account={live['trading_day']}")

        last = Decimal(quote()["last"])
        # Well below the market yet inside the policy's deviation and the exchange limit.
        resting = (last * (1 - Decimal(args.deviation) * Decimal("0.8")) / tick).to_integral_value(
            ROUND_FLOOR) * tick
        if quote().get("lower_limit"):
            resting = max(resting, Decimal(quote()["lower_limit"]))
        manual = dict(action="submit", venue=venue, symbol=symbol, side="buy", offset="open",
                      quantity="1", price=str(resting))
        refused("order refused before authorization", "authorize", order_id="early", **manual)
        act(action="live_authorize", user_id=args.user)
        step("authorized", session()["authorization"] is not None)

        if held(live) == 0 and not any(o["status"] in ("submitted", "accepted", "partially_filled")
                                       for o in live["orders"]):
            act(order_id="resting", **manual)
            live = wait(lambda s: order(s, "resting") and order(s, "resting")["status"] != "submitted")
            status = live and order(live, "resting")["status"]
            step("resting order accepted by the counter", status == "accepted",
                 f"price={resting} status={status}")
            if status == "accepted":
                act(action="cancel", order_id="resting")
                live = wait(lambda s: order(s, "resting")["status"] == "cancelled")
                step("resting order cancelled", bool(live))
        else:
            step("resting order", False, "skipped: the contract already has a position or orders")

        call("live.disconnect", {"account": ACCOUNT})
        live = wait(lambda s: s["phase"] == "disconnected")
        step("authorization kept while disconnected", live and live["authorization"] is not None)
        call("live.connect", {"account": ACCOUNT, "password": trading_password,
                              "auth_code": auth_code})
        trading_password = auth_code = ""
        live = wait(lambda s: s["phase"] in ("ready", "error"))
        if not step("reconnected without a new authorization",
                    live and live["phase"] == "ready" and live["authorization"] is not None,
                    f"phase={live and live['phase']}"):
            raise RuntimeError("the account did not synchronize again")

        call("live.strategy.start", {"account": ACCOUNT, "request_id": str(uuid.uuid4()),
                                     "account_id": live["account_id"],
                                     "policy_revision": live["policy_revision"],
                                     "venue": venue, "symbol": symbol, "fast": args.fast,
                                     "slow": args.slow, "quantity": "1"})
        run = session()["strategy"]
        step("strategy started", run and run["state"] == "running", f"run={run and run['id']}")
        refused("manual order refused while the strategy runs", "strategy controls",
                order_id="manual", **manual)
        seen, targets, bars = {}, [], 0
        deadline = time.monotonic() + args.minutes * 60
        while time.monotonic() < deadline:
            live = session()
            run = live["strategy"]
            if run["state"] != "running":
                step("strategy still running", False, run["reason"])
                break
            if run["bars"] != bars:
                bars = run["bars"]
                print(f"       bars={bars} bar_ms={run['bar_ms']} target={run['target']} "
                      f"long={held(live)} account_day={live['trading_day']}", flush=True)
            if run["target"] is not None and (not targets or targets[-1] != run["target"]):
                targets.append(run["target"])
            for item in live["orders"]:
                if not item["id"].startswith(run["id"] + "."):
                    continue
                if seen.get(item["id"]) != item["status"]:
                    if item["id"] not in seen:
                        bar_ms = int(item["id"][len(run["id"]) + 1:].split(".")[0])
                        series = call("market.minutes", contract)["intraday"]
                        close = next((b["close"] for b in series.get("bars", [])
                                      if b["start_ms"] == bar_ms), None)
                        step(f"strategy order {item['side']} {item['offset']} at the bar close",
                             close is not None and Decimal(close) == Decimal(item["limit_price"]),
                             f"limit={item['limit_price']} bar_close={close}")
                    seen[item["id"]] = item["status"]
                    print(f"       order {item['id'][-22:]} {item['status']} "
                          f"filled={item['filled']}", flush=True)
            time.sleep(2)
        step("minute bars received", bars > args.slow, f"{bars} bars")
        step("strategy produced a target", bool(targets), f"targets={targets}")
        step("strategy orders", True, f"{len(seen)} placed: {sorted(set(seen.values()))}")

        if session()["strategy"]["state"] == "running":
            act(action="strategy_stop")
        live = wait(lambda s: s["strategy"]["state"] == "stopped" and not any(
            o["id"].startswith(s["strategy"]["id"] + ".") and
            o["status"] in ("submitted", "accepted", "partially_filled") for o in s["orders"]), 20)
        step("strategy stopped and its orders left the book", bool(live))
        live = session()
        step("authorization kept after the stop", live["authorization"] is not None)
        step("no unconfirmed orders", not live["unconfirmed"], live["unconfirmed"])
        print(f"       position kept: long {held(live)} {args.instrument}", flush=True)
        report["position_kept"] = str(held(live))
        act(action="live_revoke")
        step("authorization revoked", session()["authorization"] is None)
        call("live.disconnect", {"account": ACCOUNT})
        call("market.disconnect")
    except Exception as error:  # report, never the credentials
        step("run", False, str(error))
    finally:
        market_password = trading_password = auth_code = ""
        try:
            try:
                core.stdin.close()
                core.terminate()
                core.wait(timeout=15)
            finally:
                stop_test_agent(folder)
        except Exception as error:
            step("cleanup", False, str(error))
    report["passed"] = all(item["ok"] for item in report["steps"])
    if args.report:
        Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("PASSED" if report["passed"] else "FAILED")
    sys.exit(0 if report["passed"] else 1)
