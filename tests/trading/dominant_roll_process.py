"""Isolated process verification: dominant-series warmup, live roll and policy allowlist."""
import datetime
from decimal import Decimal
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time

source, build = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
sys.path[:0] = [
    str(source / "tests/trading"),
    str(source / "tests/lab"),
    str(source / "tests/support"),
    str(source / "apps/services/lab"),
    str(source / "apps/services/common/python"),
    str(build / "generated-python"),
    str(build / "engine-python"),
]

from asterion import (  # noqa: E402
    common_pb2 as common,
    lab_pb2 as lab_pb,
    strategy_pb2 as wire,
    trading_pb2 as trading,
)
from asterion_lab import backtest  # noqa: E402
from service_fixture import Service  # noqa: E402
from strategy_process import (  # noqa: E402
    account_command,
    account_orders,
    account_ready,
    account_rpc,
    account_snapshot,
    rpc,
    terminate,
    until,
)
from trading_installation import Programs, install  # noqa: E402
from warehouse_fixture import contract_row, daily, price_tick, settlement  # noqa: E402
from warehouse_writer import Dataset as WrittenDataset, Warehouse  # noqa: E402


STRATEGY_CODE = """import math
from asterion_strategy import Strategy

class RollCheck(Strategy):
    params = {"window": 3, "lots": 2}

    def ready(self, ctx):
        return len(ctx.history("SHFE/rb", "close", self.params["window"])) == self.params["window"]

    def on_bars(self, ctx, bars):
        bar = bars["SHFE/rb"]
        assert bar.symbol == "SHFE/rb", bar.symbol
        closes = [round(float(x), 6) for x in ctx.history("SHFE/rb", "close", 6)]
        roll_yields = [
            round(float(x), 8) if math.isfinite(x) else None
            for x in ctx.history("SHFE/rb", "roll_yield", 6)
        ]
        if bar.contract == "SHFE/rb/2027-01":
            expected = [
                round(3300 * 1.02 * 1.02, 6),
                round(3400 * 1.02, 6),
                round(3420 * 1.02, 6),
                round(3480 * 1.02, 6),
                round(3500 * 1.02, 6),
                3580.0,
            ]
            assert closes == expected, (closes, expected)
            expected_ry = [
                round(12.0 * math.log(3300 / 3366) / 5, 8),
                round(12.0 * math.log(3400 / 3468) / 3, 8),
                round(12.0 * math.log(3420 / 3488) / 3, 8),
                round(12.0 * math.log(3480 / 3550) / 3, 8),
                round(12.0 * math.log(3500 / 3570) / 3, 8),
                None,
            ]
            assert roll_yields == expected_ry, (roll_yields, expected_ry)
            assert math.isnan(bar.roll_yield), bar.roll_yield
        ctx.target("SHFE/rb", self.params["lots"])
"""


def write_warehouse(directory: Path, bars_by_day: dict):
    contracts = ("SHFE/rb/2026-05", "SHFE/rb/2026-10", "SHFE/rb/2027-01")
    rows = []
    for day, specs in sorted(bars_by_day.items()):
        for contract, (close, oi) in zip(contracts, specs):
            rows.append(daily(contract, day, close, oi))
    writer = Warehouse(directory).writer()
    writer.partitions.clear()
    cal_start, cal_end = datetime.date(2026, 9, 1), datetime.date(2026, 10, 20)
    cal_days = [
        cal_start + datetime.timedelta(days=n)
        for n in range((cal_end - cal_start).days + 1)
    ]
    open_set = set(bars_by_day.keys())
    writer.put_rows("calendar/SHFE/2026", "calendar", [
        dict(exchange="SHFE", day=d.isoformat(), open=(d in open_set), previous_trading_day=None)
        for d in cal_days
    ])
    writer.put_rows("contracts/SHFE", "contracts", [
        {**contract_row(c), "delist_date": "2027-05-15"} for c in contracts
    ])
    writer.put_rows("price_ticks/SHFE", "price_ticks", [
        price_tick(c, begin="2026-09-01", end="2027-01-01") for c in contracts
    ])
    for d in sorted(open_set):
        writer.put_rows(
            f"settlement/SHFE/{d.isoformat()}",
            "settlement",
            [settlement(c, d) for c in contracts],
        )
    for ym in ("2026-09", "2026-10"):
        ym_days = [d.isoformat() for d in sorted(open_set) if d.isoformat().startswith(ym)]
        if ym_days:
            writer.add_days(
                "SHFE",
                ym,
                [r for r in rows if r["day"].startswith(ym)],
                ym_days,
            )
    writer.commit()
    return WrittenDataset(directory).current()


def setup_host(directory: Path, webhook_url: str | None = None):
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    host = install(
        directory,
        Programs(
            str(build / "asterion_trading"),
            str(build / "asterion_test_certificates"),
            str(build / f"libasterion_test_ctp_trader{suffix}"),
        ),
    )
    (host.root / "lib/ctp-md.so").symlink_to(build / f"libasterion_test_ctp{suffix}")
    code = host.root / "lib/strategy"
    (code / "asterion").mkdir(parents=True)
    for path in (
        source / "apps/services/trading/python/strategy_runner.py",
        source / "apps/services/common/python/asterion_strategy.py",
    ):
        shutil.copy2(path, code)
    for name in ("strategy_pb2.py", "common_pb2.py"):
        shutil.copy2(build / "generated-python/asterion" / name, code / "asterion")
    (host.root / "runtime").mkdir()
    (host.root / "runtime/python").symlink_to(Path(sys.executable).resolve().parents[1])
    host.account("account", user="script")
    if webhook_url:
        (host.root / "test-credentials/account/asterion.trading.account.webhook").write_text(
            webhook_url
        )
    return host


def open_policy(host, contracts):
    account_rpc(
        host,
        trading.Request(
            open_account=trading.Policy(
                limits=common.RiskLimits(
                    max_order_lots=common.Decimal(text="5"),
                    max_contract_lots=common.Decimal(text="10"),
                    max_gross_lots=common.Decimal(text="10"),
                    max_working_orders=4,
                ),
                max_price_deviation=common.Decimal(text="0.05"),
                contracts=contracts,
            )
        ),
    )
    account_ready(host)
    account_command(
        host,
        trading.Command(
            request_id="auth.1",
            authorize=trading.Authorize(user_id="script"),
        ),
    )


def append_ticks(flow: Path, rows):
    flow.mkdir(parents=True, exist_ok=True)
    with (flow / "script.tsv").open("a") as out:
        for exch, sym, tday, last, vol, oi, pre_oi, pre_settle in rows:
            out.write(
                f"{exch}\t{sym}\t{tday}\t{last}\t{last - 1}\t{last + 1}\t"
                f"{vol}\t{oi}\t{pre_oi}\t{pre_settle}\t3900\t3100\n"
            )


def roll_trader_day(trader_flow: Path, next_day: str, host):
    target = trader_flow / "next-day"
    tmp = trader_flow / "next-day.tmp"
    tmp.write_text(next_day)
    tmp.replace(target)
    until(
        lambda: account_snapshot(host),
        lambda s: (
            not target.exists()
            and s.trading_day == f"{next_day[:4]}-{next_day[4:6]}-{next_day[6:]}"
            and s.broker_state == trading.BROKER_STATE_READY
            and s.positions_reconciled
        ),
    )
    account_ready(host)


def verify_backtest_parity(lab_dir: Path, version_id: str):
    import asterion_engine

    cfg = backtest.Config(
        repository="",
        strategy="",
        params={"window": 3, "lots": 2},
        symbols=("SHFE/rb",),
        begin_day="2026-09-22",
        end_day="2026-10-09",
        deposit=Decimal("1000000"),
        costs=None,
        slippage_ticks=0,
        max_order_lots=Decimal("5"),
        max_gross_lots=Decimal("10"),
        max_contract_lots=Decimal("10"),
        max_working_orders=4,
        max_gross_weight=Decimal("0"),
        max_net_weight=Decimal("0"),
        annual_periods=None,
        zero_risk_free=True,
        holdout_day="",
        minutes=0,
        dataset_version=version_id,
    )
    data = backtest.Data(
        Service("test", lab_dir, None).dataset(),
        cfg.begin_day,
        cfg.end_day,
        version=version_id,
    )
    contracts, series, first = backtest.prepare(data, cfg)
    engine = asterion_engine.Replay(
        backtest.raw(cfg.deposit),
        contracts,
        series,
        (
            backtest.raw(cfg.max_order_lots),
            backtest.raw(cfg.max_gross_lots),
            cfg.max_working_orders,
            backtest.raw(cfg.max_contract_lots),
        ),
    )
    ns = {}
    exec(STRATEGY_CODE, ns)
    strat = ns["RollCheck"](window=3, lots=2)
    symbols = {engine.unit(index): symbol for symbol, index in first.items()}
    ctx = backtest.replay(engine, strat, symbols, contracts, cfg)
    final_closes = [round(float(x), 6) for x in ctx.history("SHFE/rb", "close", 6)]
    assert final_closes == [
        round(3300 * 1.02 * 1.02, 6),
        round(3400 * 1.02, 6),
        round(3420 * 1.02, 6),
        round(3480 * 1.02, 6),
        round(3500 * 1.02, 6),
        3580.0,
    ], final_closes
    return final_closes


def main():
    alerts = []
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode()
            with lock:
                alerts.append(json.loads(body))
            self.send_response(200)
            self.end_headers()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    webhook_url = f"http://127.0.0.1:{server.server_port}/alerts"

    with tempfile.TemporaryDirectory(prefix="ast-dom-roll-") as tmp:
        root_dir = Path(tmp)
        warmup_wh = root_dir / "warmup-wh"
        full_wh = root_dir / "full-wh"
        w_days = {
            datetime.date(2026, 9, 22): [(3290, 8000), (3350, 3000), (3420, 1000)],
            datetime.date(2026, 9, 23): [(3300, 6000), (3366, 7000), (3434, 1000)],
            datetime.date(2026, 9, 24): [(3310, 2000), (3400, 7200), (3468, 2000)],
            datetime.date(2026, 9, 25): [(3320, 1000), (3420, 4800), (3488, 2500)],
        }
        full_days = {
            **w_days,
            datetime.date(2026, 9, 29): [(3330, 800), (3480, 4200), (3550, 3500)],
            datetime.date(2026, 9, 30): [(3340, 500), (3500, 3000), (3570, 6000)],
            datetime.date(2026, 10, 9): [(3350, 300), (3510, 2500), (3580, 6500)],
        }
        full_version = write_warehouse(full_wh, full_days)
        bt_closes = verify_backtest_parity(full_wh, full_version.identity)
        write_warehouse(warmup_wh, w_days)

        candidates = [
            wire.SeriesContract(
                instrument=common.Instrument(exchange="SHFE", symbol="rb2605"),
                delivery_month="2026-05",
            ),
            wire.SeriesContract(
                instrument=common.Instrument(exchange="SHFE", symbol="rb2610"),
                delivery_month="2026-10",
            ),
            wire.SeriesContract(
                instrument=common.Instrument(exchange="SHFE", symbol="rb2701"),
                delivery_month="2027-01",
            ),
        ]
        warmup = Service("test", warmup_wh, None).build_warmup(
            lab_pb.BuildWarmup(
                instruments=[
                    lab_pb.WarmupInstrument(
                        instrument=candidates[0].instrument,
                        contract="SHFE/rb",
                        candidates=candidates,
                    )
                ],
                period=common.PERIOD_DAY,
                max_bars=3,
            )
        )
        assert len(warmup.bars) == 3, warmup
        assert warmup.bars[0].bars[0].instrument.symbol == "rb2605"
        assert warmup.bars[0].bars[0].roll_yield.text == "-0.04752631"
        assert warmup.bars[1].bars[0].instrument.symbol == "rb2610"
        assert warmup.bars[1].bars[0].roll_ratio.text == "1.02"
        assert warmup.bars[1].bars[0].roll_yield.text == "-0.07921051"
        assert warmup.bars[2].bars[0].instrument.symbol == "rb2610"
        assert warmup.bars[2].bars[0].roll_yield.text == "-0.07875182"

        # Case 1: Product-level policy SHFE/rb rolls seamlessly without disconnecting or re-authorizing.
        host = setup_host(root_dir / "trading-ok")
        proc = host.start("account")
        md_flow = host.root / "data/account/strategies/ctp-flow"
        td_flow = host.root / "data/account/ctp-flow"
        try:
            open_policy(
                host,
                [
                    trading.ContractTerms(
                        instrument=common.Instrument(exchange="SHFE", symbol="rb"),
                        product="rb",
                        delivery_month="",
                        multiplier=common.Decimal(text="10"),
                        price_tick=common.Decimal(text="1"),
                        lot_size=common.Decimal(text="1"),
                    )
                ],
            )
            initial_auth_ms = account_snapshot(host).authorized_ms
            assert initial_auth_ms > 0

            manifest = wire.Manifest(
                name="roll-check",
                version="1",
                entry="rule:RollCheck",
                runtime="python3.12-numpy2.3.5-scipy1.17.1",
                files=["rule.py"],
            )
            rpc(
                host,
                "publish_strategy",
                wire.Package(
                    manifest=manifest,
                    files=[wire.File(path="rule.py", content=STRATEGY_CODE.encode())],
                ),
            )
            config = wire.Instance(
                id="dom-live-1",
                package_name="roll-check",
                package_version="1",
                series=[
                    wire.DominantSeries(
                        exchange="SHFE",
                        product="rb",
                        contracts=candidates,
                    )
                ],
                period=common.PERIOD_DAY,
                warmup=warmup,
            )
            started = rpc(host, "start_strategy", config).instance
            assert started.state == trading.INSTANCE_STATE_READY and started.completed_steps == 3, started

            # Day 20260928: partial initial connection day in StrategyBars.
            append_ticks(
                md_flow,
                [
                    ("SHFE", "rb2610", "20260928", 3450, 10, 4500, 4800, 3420),
                    ("SHFE", "rb2701", "20260928", 3519, 10, 3000, 2500, 3488),
                ],
            )
            time.sleep(0.25)

            # Day 20260929: rb2610 remains dominant (PreOI 4500 > 3000).
            roll_trader_day(td_flow, "20260929", host)
            append_ticks(
                md_flow,
                [
                    ("SHFE", "rb2610", "20260929", 3480, 20, 4200, 4500, 3450),
                    ("SHFE", "rb2701", "20260929", 3550, 20, 3500, 3000, 3519),
                ],
            )
            time.sleep(0.25)

            # Day 20260930 opens -> seals 2026-09-29 bar (rb2610) -> opens 2 lots on rb2610!
            # During 20260930, rb2701 OI rises to 6000 > rb2610 OI 3000!
            roll_trader_day(td_flow, "20260930", host)
            append_ticks(
                md_flow,
                [
                    ("SHFE", "rb2610", "20260930", 3500, 30, 3000, 4200, 3480),
                    ("SHFE", "rb2701", "20260930", 3570, 30, 6000, 3500, 3550),
                ],
            )
            snap_before_roll = until(
                lambda: account_snapshot(host),
                lambda s: any(
                    p.instrument.symbol == "rb2610" and p.today_lots.text == "2"
                    for p in s.positions
                ),
            )
            assert snap_before_roll.authorized_ms == initial_auth_ms

            # Day 20261009 opens -> seals 2026-09-30 bar, rolls execution on 2026-10-09:
            # closes rb2610 yesterday 2 lots FIRST, then opens rb2701 2 lots!
            roll_trader_day(td_flow, "20261009", host)
            append_ticks(
                md_flow,
                [
                    ("SHFE", "rb2610", "20261009", 3510, 25, 2500, 3000, 3500),
                    ("SHFE", "rb2701", "20261009", 3580, 40, 6500, 6000, 3570),
                ],
            )
            snap_after_roll = until(
                lambda: account_snapshot(host),
                lambda s: (
                    any(
                        p.instrument.symbol == "rb2701" and p.today_lots.text == "2"
                        for p in s.positions
                    )
                    and not any(
                        p.instrument.symbol == "rb2610"
                        and (p.today_lots.text != "0" or p.yesterday_lots.text != "0")
                        for p in s.positions
                    )
                ),
            )
            assert snap_after_roll.authorized_ms == initial_auth_ms
            orders_on_roll = list(reversed(account_orders(host)))
            assert [
                (o.instrument.symbol, o.offset) for o in orders_on_roll
            ] == [
                ("rb2610", common.OFFSET_CLOSE_YESTERDAY),
                ("rb2701", common.OFFSET_OPEN),
            ], orders_on_roll

            # Day 20261012 opens -> seals 2026-10-09 bar (rb2701 with roll_ratio=1.02) ->
            # RollCheck.on_bars asserts exact adjusted ctx.history matching backtest.py!
            roll_trader_day(td_flow, "20261012", host)
            append_ticks(
                md_flow,
                [
                    ("SHFE", "rb2610", "20261012", 3520, 15, 2000, 2500, 3510),
                    ("SHFE", "rb2701", "20261012", 3590, 45, 7000, 6500, 3580),
                ],
            )
            final_inst = until(
                lambda: rpc(host, "get_strategies", {}).instance,
                lambda st: st.completed_steps >= 6,
            )
            assert final_inst.state == trading.INSTANCE_STATE_READY, final_inst
            run_record = rpc(host, "get_strategy_run", {})
            roll_events = [e.text for e in run_record.events if "rolled SHFE/rb" in e.text]
            assert roll_events == ["rolled SHFE/rb from rb2610 to rb2701 ratio=1.02"], roll_events
            assert bt_closes[-1] == 3580.0
            rpc(host, "stop_strategy", dict(id=config.id))

            (md_flow / "script.tsv").write_text("")
            config.id = "dom-no-settlement"
            rpc(host, "start_strategy", config)
            for day, pre_oi, next_pre_oi, next_settle in (
                ("20261012", 5000, 3000, 3580),
                ("20261013", 5000, 3000, 3590),
                ("20261014", 3000, 6000, 0),
            ):
                if day != "20261012":
                    roll_trader_day(td_flow, day, host)
                append_ticks(md_flow, [
                    ("SHFE", "rb2610", day, 3520, 30, 3000, pre_oi, 3510),
                    ("SHFE", "rb2701", day, 3590, 50, 6000, next_pre_oi, next_settle),
                ])
                time.sleep(0.25)
            failed = until(
                lambda: rpc(host, "get_strategies", {}).instance,
                lambda st: st.state == trading.INSTANCE_STATE_FAILED,
            )
            assert "lacks previous settlement price on 2026-10-14" in failed.error, failed
            assert not account_orders(host), account_orders(host)
        finally:
            terminate(proc)

        # Case 2: Specific-month policy (rb2610 only, omitting rb2701) stops the instance
        # and fires a Webhook alert BEFORE rolling into an unauthorized contract.
        host2 = setup_host(root_dir / "trading-deny", webhook_url=webhook_url)
        proc2 = host2.start("account")
        md_flow2 = host2.root / "data/account/strategies/ctp-flow"
        td_flow2 = host2.root / "data/account/ctp-flow"
        try:
            open_policy(
                host2,
                [
                    trading.ContractTerms(
                        instrument=common.Instrument(exchange="SHFE", symbol="rb2610"),
                        product="rb",
                        delivery_month="2026-10",
                        multiplier=common.Decimal(text="10"),
                        price_tick=common.Decimal(text="1"),
                        lot_size=common.Decimal(text="1"),
                    )
                ],
            )
            assert account_snapshot(host2).trading_day == "2026-09-28"
            manifest = wire.Manifest(
                name="roll-check",
                version="1",
                entry="rule:RollCheck",
                runtime="python3.12-numpy2.3.5-scipy1.17.1",
                files=["rule.py"],
            )
            rpc(
                host2,
                "publish_strategy",
                wire.Package(
                    manifest=manifest,
                    files=[wire.File(path="rule.py", content=STRATEGY_CODE.encode())],
                ),
            )
            rpc(
                host2,
                "start_strategy",
                wire.Instance(
                    id="dom-deny-1",
                    package_name="roll-check",
                    package_version="1",
                    series=[
                        wire.DominantSeries(
                            exchange="SHFE",
                            product="rb",
                            contracts=candidates,
                        )
                    ],
                    period=common.PERIOD_DAY,
                    warmup=warmup,
                ),
            )
            append_ticks(
                md_flow2,
                [
                    ("SHFE", "rb2610", "20260928", 3450, 10, 4500, 4800, 3420),
                    ("SHFE", "rb2701", "20260928", 3519, 10, 3000, 2500, 3488),
                ],
            )
            time.sleep(0.25)
            roll_trader_day(td_flow2, "20260929", host2)
            append_ticks(
                md_flow2,
                [
                    ("SHFE", "rb2610", "20260929", 3480, 20, 3000, 4500, 3450),
                    ("SHFE", "rb2701", "20260929", 3550, 20, 6000, 3000, 3519),
                ],
            )
            time.sleep(0.25)
            # 20260930 opens with rb2701 PreOI 6000 > rb2610 PreOI 3000, while rb2701 is NOT in policy!
            roll_trader_day(td_flow2, "20260930", host2)
            append_ticks(
                md_flow2,
                [
                    ("SHFE", "rb2610", "20260930", 3500, 30, 2500, 3000, 3480),
                    ("SHFE", "rb2701", "20260930", 3570, 30, 6500, 6000, 3550),
                ],
            )
            failed = until(
                lambda: rpc(host2, "get_strategies", {}).instance,
                lambda st: st.state == trading.INSTANCE_STATE_FAILED,
            )
            assert "SHFE/rb2701" in failed.error and "not allowed" in failed.error, failed
            assert not account_orders(host2), account_orders(host2)
            until(
                lambda: list(alerts),
                lambda items: any(
                    a["kind"] == "strategy_failed" and "SHFE/rb2701" in a["text"]
                    for a in items
                ),
            )
        finally:
            terminate(proc2)
    server.shutdown()


if __name__ == "__main__":
    main()
