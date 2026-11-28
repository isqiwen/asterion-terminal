"""Live Trading <-> Lab backtest reconciliation tests.

Run with the bundled interpreter:
python3 -I -B reconcile_test.py <source> <generated-python> <engine-directory>
"""
from dataclasses import replace
import asyncio
from decimal import Decimal
import json
from pathlib import Path
import shutil
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "apps/services/common/python"))
sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1), sys.argv.pop(1), str(Path(__file__).parent)]

from asterion.common_pb2 import Decimal as WireDecimal, Instrument, Offset, Period, Side  # noqa: E402
from asterion.lab_pb2 import ReconcileFill, ReconcileLive, ReconcileStep, Request  # noqa: E402
from asterion.strategy_pb2 import (  # noqa: E402
    Bar as WireBar,
    Bars as WireBars,
    Decision,
    DominantSeries,
    Instance,
    SeriesContract,
)
from asterion_lab import (  # noqa: E402
    backtest,
    data as lab_data,
    log as lab_log,
    package,
    reconcile,
    reconcile_diff,
    reconcile_live,
    repository,
)
from asterion_strategy import Strategy  # noqa: E402
from service_fixture import Service  # noqa: E402
from warehouse_fixture import DAYS, warehouse  # noqa: E402
from warehouse_writer import Warehouse  # noqa: E402

EXAMPLES = Path(__file__).resolve().parents[2] / "research"


def _wdec(value) -> WireDecimal:
    text = format(Decimal(str(value)), "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return WireDecimal(text="0" if text in ("", "-0") else text)


def _native_symbol(canonical: str) -> Instrument:
    ex, prod, month = canonical.split("/")
    suffix = month[3:4] + month[5:] if ex == "CZCE" else month[2:4] + month[5:]
    return Instrument(exchange=ex, symbol=f"{prod}{suffix}")


def _write_trading_artifacts(
    account_dir: Path,
    instance: Instance,
    steps: list,
    recorded_bars: list,
    orders: list,
    fills: list,
    archived_orders: list | None = None,
    archived_fills: list | None = None,
):
    strat_dir = account_dir / "strategies"
    (strat_dir / "runs").mkdir(parents=True, exist_ok=True)
    run_db = strat_dir / "runs" / f"{instance.id}.sqlite"
    with sqlite3.connect(run_db) as conn:
        conn.execute("CREATE TABLE configuration (value BLOB NOT NULL)")
        conn.execute("CREATE TABLE steps (sequence INTEGER PRIMARY KEY, input BLOB NOT NULL, decision BLOB)")
        conn.execute("CREATE TABLE events (time_ms INTEGER NOT NULL, kind INTEGER NOT NULL, event TEXT NOT NULL)")
        conn.execute("INSERT INTO configuration VALUES (?)", (instance.SerializeToString(),))
        for seq, wire_bars, decision in steps:
            conn.execute(
                "INSERT INTO steps VALUES (?, ?, ?)",
                (seq, wire_bars.SerializeToString(), None if decision is None else decision.SerializeToString()),
            )

    bars_db = strat_dir / "bars.sqlite"
    with sqlite3.connect(bars_db) as conn:
        conn.execute(
            "CREATE TABLE bars (period INTEGER NOT NULL, end_ms INTEGER NOT NULL, "
            "exchange TEXT NOT NULL, symbol TEXT NOT NULL, bar BLOB NOT NULL, "
            "PRIMARY KEY(period, end_ms, exchange, symbol))"
        )
        for period, end_ms, bar in recorded_bars:
            conn.execute(
                "INSERT OR REPLACE INTO bars VALUES (?, ?, ?, ?, ?)",
                (int(period), end_ms, bar.instrument.exchange, bar.instrument.symbol, bar.SerializeToString()),
            )

    def _populate_journal(db_path: Path, ord_rows: list, fill_rows: list, segments: list | None = None):
        with sqlite3.connect(db_path) as conn:
            conn.execute("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)")
            conn.execute("CREATE TABLE records(sequence INTEGER PRIMARY KEY, body TEXT NOT NULL)")
            conn.execute(
                "CREATE TABLE segments(first INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, "
                "digest TEXT NOT NULL, records INTEGER NOT NULL, bytes INTEGER NOT NULL)"
            )
            conn.execute("INSERT INTO meta VALUES ('format', 'asterion.journal')")
            for first, name in (segments or []):
                conn.execute("INSERT INTO segments VALUES (?, ?, '0', 1, 1)", (first, name))
            seq = 1
            for o in ord_rows:
                conn.execute("INSERT INTO records VALUES (?, ?)", (seq, json.dumps(o)))
                seq += 1
            if fill_rows:
                conn.execute("INSERT INTO records VALUES (?, ?)", (seq, json.dumps({"trades_recorded": fill_rows})))

    segments = []
    if archived_orders or archived_fills:
        (account_dir / "archives").mkdir(parents=True, exist_ok=True)
        _populate_journal(
            account_dir / "archives" / "00000001.sqlite",
            archived_orders or [],
            archived_fills or [],
        )
        segments.append((1, "00000001.sqlite"))
    _populate_journal(account_dir / "journal.sqlite", orders, fills, segments)


class ReconcileTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.lab_dir = self.root / "lab"
        self.lab_dir.mkdir()
        warehouse(self.lab_dir)
        self.version = Warehouse(self.lab_dir).current()
        self.repo = self.root / "repo"
        shutil.copytree(EXAMPLES, self.repo)
        self.account_dir = self.root / "trading-account"
        self.account_dir.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def _run_reference_backtest(self, run_id="bt-1", **overrides):
        cfg = backtest.Config(
            repository=str(self.repo),
            strategy="ma_cross.MaCross",
            params={"fast": 3, "slow": 6, "lots": 2},
            symbols=("SHFE/rb",),
            begin_day=DAYS[0].isoformat(),
            end_day=DAYS[-1].isoformat(),
            deposit=Decimal("1000000"),
            costs=None,
            slippage_ticks=0,
            max_order_lots=Decimal("10"),
            max_gross_lots=Decimal("20"),
            max_contract_lots=Decimal("20"),
            max_working_orders=10,
            dataset_version=self.version.identity,
            source=str(self.repo),
        )
        cfg = replace(cfg, **overrides)
        backtest.run(self.lab_dir, run_id, cfg)
        strat = repository.load(self.repo, cfg.strategy, cfg.params)
        replayed = reconcile_diff.replay_backtest(Warehouse(self.lab_dir), cfg, strat)
        return cfg, replayed

    def _build_matching_live(self, cfg, replayed, instance_id="inst-1"):
        pkg = package.build(self.repo, cfg.strategy)
        pkg_version = pkg.manifest.version
        pkg_dir = self.account_dir / "strategies" / "packages" / pkg.manifest.name / pkg_version
        pkg_dir.mkdir(parents=True, exist_ok=True)
        for f in pkg.files:
            target = pkg_dir / f.path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(f.content)
        (pkg_dir / "strategy.json").write_text(json.dumps({
            "name": pkg.manifest.name,
            "version": pkg_version,
            "entry": pkg.manifest.entry,
            "runtime": pkg.manifest.runtime,
            "files": list(pkg.manifest.files),
        }))

        instance = Instance(
            id=instance_id,
            package_name=pkg.manifest.name,
            package_version=pkg_version,
            period=Period.PERIOD_DAY,
            series=[
                DominantSeries(
                    exchange="SHFE",
                    product="rb",
                    contracts=[
                        SeriesContract(instrument=_native_symbol("SHFE/rb/2026-05"), delivery_month="2026-05"),
                        SeriesContract(instrument=_native_symbol("SHFE/rb/2026-10"), delivery_month="2026-10"),
                    ],
                )
            ],
        )
        instance.params["fast"].integer = 3
        instance.params["slow"].integer = 6
        instance.params["lots"].integer = 2
        instance.warmup.dataset_version = cfg.dataset_version
        instance.warmup_steps = 0

        steps = []
        recorded_bars = []
        data = replayed["data"]
        stored_rb = data.product("SHFE", "rb")
        for seq, (end_ms, dec) in enumerate(sorted(replayed["decisions"].items()), 1):
            b = replayed["bars"][(end_ms, "SHFE/rb")]
            wire_bar = WireBar(
                instrument=_native_symbol(b["contract"]),
                trading_day=b["trading_day"],
                end_ms=end_ms,
                open=_wdec(b["open"]),
                high=_wdec(b["high"]),
                low=_wdec(b["low"]),
                close=_wdec(b["close"]),
                volume=int(b["volume"]),
                open_interest=_wdec(b["open_interest"]),
            )
            if b["roll_ratio"] is not None:
                wire_bar.roll_ratio.CopyFrom(_wdec(b["roll_ratio"]))
            if b["roll_yield"] is not None:
                wire_bar.roll_yield.CopyFrom(_wdec(b["roll_yield"]))
            decision = Decision(sequence=seq, ready=dec["ready"])
            for sym, target in dec["targets"].items():
                decision.targets.add(instrument=_native_symbol(b["contract"]), lots=int(target["value"]))
            steps.append((seq, WireBars(sequence=seq, bars=[wire_bar]), decision))
            # Record both contracts' raw daily bars in bars.sqlite.
            for c_name in ("SHFE/rb/2026-05", "SHFE/rb/2026-10"):
                row = stored_rb[c_name].days.get(b["trading_day"])
                if row and row["auction"] and row["close"] is not None:
                    raw_bar = WireBar(
                        instrument=_native_symbol(c_name),
                        trading_day=b["trading_day"],
                        end_ms=end_ms,
                        open=_wdec(row["open"]),
                        high=_wdec(row["high"]),
                        low=_wdec(row["low"]),
                        close=_wdec(row["close"]),
                        volume=int(row["volume"]),
                        open_interest=_wdec(row["open_interest"]),
                    )
                    recorded_bars.append((Period.PERIOD_DAY, end_ms, raw_bar))

        orders, fills = [], []
        for idx, row in enumerate(replayed["fills"].iter_rows(named=True), 1):
            inst = _native_symbol(row["contract"])
            oid = f"ord-{idx}"
            day = row["trading_day"].isoformat()
            orders.append({
                "command": {
                    "action": "submit",
                    "order_id": oid,
                    "venue": inst.exchange,
                    "symbol": inst.symbol,
                    "side": row["side"],
                    "offset": row["offset"],
                    "quantity": str(Decimal(row["quantity"]).normalize()),
                    "price": str(Decimal(row["price"]).normalize()),
                },
                "policy_revision": "p1",
                "broker_key": f"bk-{idx}",
                "trading_day": day,
                "strategy": {"instance": instance_id, "target_sequence": idx, "leg": 0},
            })
            fills.append({
                "id": f"trd-{idx}",
                "order_id": oid,
                "venue": inst.exchange,
                "symbol": inst.symbol,
                "side": row["side"],
                "offset": row["offset"],
                "quantity": str(Decimal(row["quantity"]).normalize()),
                "price": str(Decimal(row["price"]).normalize()),
                "trading_day": day,
                "trade_time": "09:00:01",
                "received_ms": idx * 1000,
            })
        return instance, steps, recorded_bars, orders, fills

    def test_clean_reconciliation_passes_with_and_without_run_id_including_archives(self):
        cfg, replayed = self._run_reference_backtest("bt-clean")
        instance, steps, recorded_bars, orders, fills = self._build_matching_live(cfg, replayed)
        # Split the first order/fill into an archived journal segment to verify archive traversal.
        _write_trading_artifacts(
            self.account_dir, instance, steps, recorded_bars,
            orders[1:], fills[1:], archived_orders=orders[:1], archived_fills=fills[:1],
        )
        # Mode 1: against saved Lab run_id
        report_run = reconcile.reconcile(
            self.account_dir, self.lab_dir, instance_id="inst-1", run_id="bt-clean",
            strategy_source=self.repo,
        )
        self.assertTrue(report_run["ok"], report_run)
        self.assertGreater(report_run["summary"]["bars"]["matched"], 20)
        self.assertGreater(report_run["summary"]["recorded_bars"]["matched"], 40)
        self.assertGreater(report_run["summary"]["decisions"]["matched"], 20)
        self.assertGreater(report_run["summary"]["fills"]["matched"], 0)
        self.assertEqual(report_run["summary"]["fills"]["live_fill_count"], len(fills))

        # Mode 2: directly from Trading package without --run-id
        report_pkg = reconcile.reconcile(
            self.account_dir, self.lab_dir, instance_id="inst-1",
        )
        self.assertTrue(report_pkg["ok"], report_pkg)
        self.assertIn("通过", reconcile.format_report(report_pkg))

    def test_fill_corrections_and_missing_archives_follow_the_account_journal(self):
        cfg, replayed = self._run_reference_backtest("bt-attribution")
        instance, steps, bars, orders, fills = self._build_matching_live(cfg, replayed)
        other = {**orders[0], "command": {**orders[0]["command"], "order_id": "other"},
                 "strategy": {**orders[0]["strategy"], "instance": "other-instance"}}
        corrected = {**fills[0], "order_id": "other"}
        _write_trading_artifacts(self.account_dir, instance, steps, bars, [other], [corrected],
                                 archived_orders=orders[:1], archived_fills=fills[:1])
        self.assertEqual(reconcile_live.read_live_fills(self.account_dir, instance.id)["fills"], [])
        other_fills = reconcile_live.read_live_fills(self.account_dir, "other-instance")["fills"]
        self.assertEqual([trade["order_id"] for trade in other_fills], ["other"])
        (self.account_dir / "archives" / "00000001.sqlite").unlink()
        with self.assertRaisesRegex(FileNotFoundError, "00000001.sqlite"):
            reconcile_live.read_live_fills(self.account_dir, instance.id)

    def test_replay_fails_when_a_position_is_held_through_a_month_without_prices(self):
        cfg, _ = self._run_reference_backtest("bt-unpriced")
        store = Warehouse(self.lab_dir)
        writer = store.writer()
        rows = store.read(writer.base, "daily/SHFE/2026-01").to_dicts()
        for row in rows:
            if row["contract"] == "SHFE/rb/2026-05" and row["day"] >= DAYS[18]:
                row["open"] = None
            row["day"] = row["day"].isoformat()
        writer.put_rows("daily/SHFE/2026-01", "daily", rows,
                        days=[day.isoformat() for day in DAYS])
        writer.commit()

        class Pause(Strategy):
            def on_bar(self, ctx, bar):
                if bar.time.date() == DAYS[2]:
                    ctx.target(bar.symbol, 1)

            def ready(self, ctx):
                return ctx.time.date() <= DAYS[2]

        config = replace(cfg, dataset_version=Warehouse(self.lab_dir).current().identity)
        with self.assertRaisesRegex(ValueError, "prices of SHFE/rb/2026-05 are missing on .*while it is held"):
            reconcile_diff.replay_backtest(Warehouse(self.lab_dir), config, Pause())

    def test_live_steps_with_no_recorded_contract_bars_report_missing_evidence(self):
        cfg, replayed = self._run_reference_backtest("bt-missing-raw")
        instance, steps, _, orders, fills = self._build_matching_live(cfg, replayed)
        _write_trading_artifacts(self.account_dir, instance, steps, [], orders, fills)
        report = reconcile.reconcile(self.account_dir, self.lab_dir, instance_id=instance.id,
                                     run_id="bt-missing-raw", strategy_source=self.repo)
        self.assertFalse(report["ok"])
        self.assertFalse(report["metadata"]["warmup_only"])
        self.assertGreater(report["summary"]["recorded_bars"]["missing_in_live"], 0)

    def test_detects_bar_decision_and_fill_discrepancies_and_respects_tolerances(self):
        cfg, replayed = self._run_reference_backtest("bt-diff")
        instance, steps, recorded_bars, orders, fills = self._build_matching_live(cfg, replayed, "inst-diff")

        # 1) Corrupt close and volume on step index 8, and change decision target on step index 10.
        seq8, wb8, dec8 = steps[8]
        bad_bar = WireBar()
        bad_bar.CopyFrom(wb8.bars[0])
        bad_bar.close.text = str(Decimal(bad_bar.close.text) + Decimal("5"))
        bad_bar.volume += 7
        steps[8] = (seq8, WireBars(sequence=seq8, bars=[bad_bar]), Decision(sequence=seq8, ready=not dec8.ready))

        seq10, wb10, dec10 = steps[10]
        bad_dec = Decision(sequence=seq10, ready=dec10.ready)
        bad_dec.targets.add(instrument=wb10.bars[0].instrument, lots=99)
        steps[10] = (seq10, wb10, bad_dec)

        # 2) Drop one recorded bar from bars.sqlite and add fill slippage of +2 ticks on first fill.
        recorded_bars = recorded_bars[:-1]
        fills[0]["price"] = str(Decimal(fills[0]["price"]) + Decimal("2"))

        _write_trading_artifacts(self.account_dir, instance, steps, recorded_bars, orders, fills)

        out_json = self.root / "report.json"
        with self.assertRaises(SystemExit) as ctx:
            reconcile.main([
                "--trading-account", str(self.account_dir),
                "--lab-dir", str(self.lab_dir),
                "--instance", "inst-diff",
                "--run-id", "bt-diff",
                "--strategy-source", str(self.repo),
                "--output", str(out_json),
            ])
        self.assertEqual(ctx.exception.code, 1)
        report = json.loads(out_json.read_text())
        self.assertFalse(report["ok"])
        self.assertEqual(report["summary"]["bars"]["mismatched"], 1)
        self.assertEqual(report["bar_diffs"][0]["fields"]["close"]["delta"], "5")
        self.assertEqual(report["bar_diffs"][0]["fields"]["volume"]["delta"], "7")
        self.assertEqual(report["summary"]["recorded_bars"]["missing_in_live"], 1)
        # Step 8 decision diff is tagged caused_by_bar_diff=True; step 10 is False.
        by_seq = {d["sequence"]: d for d in report["decision_diffs"]}
        self.assertTrue(by_seq[seq8]["caused_by_bar_diff"])
        self.assertFalse(by_seq[seq10]["caused_by_bar_diff"])
        self.assertEqual(report["summary"]["fills"]["mismatched"], 1)
        self.assertEqual(report["fill_diffs"][0]["price_delta"], "2")

        # Allowing fill_price_tol=2 clears fill diffs while keeping bar/decision diffs.
        relaxed = reconcile.reconcile(
            self.account_dir, self.lab_dir, instance_id="inst-diff", run_id="bt-diff",
            strategy_source=self.repo, fill_price_tol=Decimal("2"),
        )
        self.assertTrue(relaxed["summary"]["fills"]["ok"])
        self.assertFalse(relaxed["ok"])

    def test_run_id_replays_live_window_and_skips_warmup_execution_and_normalizes_non_shfe_offsets(self):
        # Backtest run_id covers only the first half of DAYS, while live ran later with warmup.
        early_cfg = backtest.Config(
            repository=str(self.repo),
            strategy="ma_cross.MaCross",
            params={"fast": 3, "slow": 6, "lots": 2},
            symbols=("SHFE/rb",),
            begin_day=DAYS[0].isoformat(),
            end_day=DAYS[12].isoformat(),
            deposit=Decimal("1000000"),
            costs=None,
            slippage_ticks=0,
            max_order_lots=Decimal("10"),
            max_gross_lots=Decimal("20"),
            max_contract_lots=Decimal("20"),
            max_working_orders=10,
            dataset_version=self.version.identity,
            source=str(self.repo),
        )
        backtest.run(self.lab_dir, "bt-early", early_cfg)

        full_cfg = backtest.Config(**{**early_cfg.__dict__, "end_day": DAYS[-1].isoformat()})
        strat = repository.load(self.repo, full_cfg.strategy, full_cfg.params)
        probe = reconcile_diff.replay_backtest(Warehouse(self.lab_dir), full_cfg, strat)
        sorted_times = sorted(probe["decisions"])
        # Treat first 12 steps as warmup (execute=false), remaining steps as live (in bars.sqlite).
        live_start_ms = sorted_times[12]
        replayed = reconcile_diff.replay_backtest(
            Warehouse(self.lab_dir), full_cfg,
            repository.load(self.repo, full_cfg.strategy, full_cfg.params),
            execute_from_ms=live_start_ms,
        )
        instance, steps, recorded_bars, orders, fills = self._build_matching_live(
            full_cfg, replayed, "inst-warmup"
        )
        instance.local_bars = True
        instance.warmup_steps = 12
        _write_trading_artifacts(self.account_dir, instance, steps, recorded_bars, orders, fills)

        report = reconcile.reconcile(
            self.account_dir, self.lab_dir, instance_id="inst-warmup", run_id="bt-early",
            strategy_source=self.repo,
        )
        self.assertTrue(report["ok"], report)
        self.assertGreater(report["summary"]["bars"]["matched"], 20)
        self.assertEqual(report["summary"]["fills"]["live_only"], 0)
        self.assertEqual(report["summary"]["fills"]["backtest_only"], 0)
        self.assertEqual(report["summary"]["recorded_bars"]["matched"],
                         sum(row[1] >= live_start_ms for row in recorded_bars))

        # Non-SHFE/INE close_yesterday matches live generic close.
        live_dce = [{
            "trading_day": "2026-09-28", "contract": "DCE/m/2026-09",
            "side": "sell", "offset": "close", "quantity": Decimal("2"), "price": Decimal("3000"),
        }]
        bt_dce = [{
            "trading_day": "2026-09-28", "contract": "DCE/m/2026-09",
            "side": "sell", "offset": "close_yesterday", "quantity": Decimal("2"), "price": Decimal("3000"),
        }]
        fill_sum, fill_diffs = reconcile_diff.compare_fills(live_dce, bt_dce, Decimal(0))
        self.assertTrue(fill_sum["ok"], fill_diffs)

    def test_warmup_only_instance_starts_fresh_inside_the_reference_run_without_fills(self):
        day = DAYS[1].isoformat()
        start = lab_data.close_ns(day) // 10**6 - 6 * 60 * 60 * 1000
        writer = Warehouse(self.lab_dir).writer()
        for month in ("2026-05", "2026-10"):
            writer.put_rows(f"minutes/SHFE/{day}/rb/{month}", "minutes", [
                dict(exchange="SHFE", product="rb", contract=f"SHFE/rb/{month}", day=day,
                     start_ms=start + minute * 60_000, partial=False,
                     open=3000 + minute, high=3001 + minute, low=2999 + minute, close=3000 + minute,
                     volume=1, average_price=None, open_interest=1000)
                for minute in range(60)
            ], days=[day])
        self.version = writer.commit()
        cfg, reference = self._run_reference_backtest(
            "bt-before-warmup", minutes=5, begin_day=day, end_day=day
        )
        times = sorted(reference["decisions"])
        warmup = reconcile_diff.replay_backtest(
            Warehouse(self.lab_dir), cfg,
            repository.load(self.repo, cfg.strategy, cfg.params),
            begin_ms=times[3], end_ms=times[8], execute_from_ms=times[8] + 1,
        )
        instance, steps, _, orders, fills = self._build_matching_live(cfg, warmup, "only-warmup")
        instance.period = Period.PERIOD_MINUTES_5
        instance.warmup_steps = len(steps)
        self.assertEqual(len(steps), 6)
        self.assertEqual(fills, [])
        _write_trading_artifacts(self.account_dir, instance, steps, [], orders, fills)
        report = reconcile.reconcile(
            self.account_dir, self.lab_dir, instance_id="only-warmup", run_id="bt-before-warmup",
            strategy_source=self.repo,
        )
        self.assertTrue(report["ok"], report)
        self.assertEqual(report["summary"]["decisions"]["matched"], len(steps))
        self.assertEqual(report["summary"]["fills"]["backtest_fill_count"], 0)
        self.assertTrue(report["metadata"]["warmup_only"])

    def test_realized_slippage_ticks_accounts_for_side_and_percentiles(self):
        # Tick = 2; buy higher (+4 => +2 ticks adverse), sell lower (-2 => +1 tick adverse),
        # buy lower (-2 => -1 tick favorable), sell higher (+0 => 0 ticks).
        live = [
            {"trading_day": "2026-01-05", "contract": "SHFE/rb/2026-05", "side": "buy", "offset": "open",
             "quantity": Decimal("1"), "price": Decimal("3504")},
            {"trading_day": "2026-01-06", "contract": "SHFE/rb/2026-05", "side": "sell", "offset": "close",
             "quantity": Decimal("1"), "price": Decimal("3518")},
            {"trading_day": "2026-01-07", "contract": "SHFE/rb/2026-05", "side": "buy", "offset": "open",
             "quantity": Decimal("1"), "price": Decimal("3498")},
            {"trading_day": "2026-01-08", "contract": "SHFE/rb/2026-05", "side": "sell", "offset": "close",
             "quantity": Decimal("1"), "price": Decimal("3510")},
        ]
        bt = [
            {"trading_day": "2026-01-05", "contract": "SHFE/rb/2026-05", "side": "buy", "offset": "open",
             "quantity": Decimal("1"), "price": Decimal("3500")},
            {"trading_day": "2026-01-06", "contract": "SHFE/rb/2026-05", "side": "sell", "offset": "close",
             "quantity": Decimal("1"), "price": Decimal("3520")},
            {"trading_day": "2026-01-07", "contract": "SHFE/rb/2026-05", "side": "buy", "offset": "open",
             "quantity": Decimal("1"), "price": Decimal("3500")},
            {"trading_day": "2026-01-08", "contract": "SHFE/rb/2026-05", "side": "sell", "offset": "close",
             "quantity": Decimal("1"), "price": Decimal("3510")},
        ]
        summary, diffs = reconcile_diff.compare_fills(live, bt, Decimal(0), tick_lookup=lambda _c, _d: Decimal("2"))
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["slippage_samples"], 4)
        self.assertAlmostEqual(summary["mean_slippage_ticks"], 0.5)
        self.assertAlmostEqual(summary["median_slippage_ticks"], 0.5)
        self.assertAlmostEqual(summary["p90_slippage_ticks"], 1.7)
        self.assertAlmostEqual(summary["max_slippage_ticks"], 2.0)
        by_day = {item["trading_day"]: item for item in diffs}
        self.assertEqual(by_day["2026-01-05"]["adverse_ticks"], 2.0)
        self.assertEqual(by_day["2026-01-06"]["adverse_ticks"], 1.0)
        self.assertEqual(by_day["2026-01-07"]["adverse_ticks"], -1.0)

    def test_reconcile_live_rpc_round_trip_and_slippage_reporting(self):
        cfg, replayed = self._run_reference_backtest("bt-rpc")
        instance, steps, recorded_bars, _orders, fills = self._build_matching_live(cfg, replayed, "inst-rpc")

        side_enum = {"buy": Side.SIDE_BUY, "sell": Side.SIDE_SELL}
        offset_enum = {
            "open": Offset.OFFSET_OPEN,
            "close": Offset.OFFSET_CLOSE,
            "close_today": Offset.OFFSET_CLOSE_TODAY,
            "close_yesterday": Offset.OFFSET_CLOSE_YESTERDAY,
        }

        def make_request(fill_rows):
            return Request(
                id=1,
                reconcile_live=ReconcileLive(
                    instance=instance,
                    steps=[
                        ReconcileStep(sequence=seq, input=wb, decision=dec)
                        for seq, wb, dec in steps
                    ],
                    recorded_bars=[bar for _, _, bar in recorded_bars],
                    live_fills=[
                        ReconcileFill(
                            trading_day=f["trading_day"],
                            instrument=Instrument(exchange=f["venue"], symbol=f["symbol"]),
                            side=side_enum[f["side"]],
                            offset=offset_enum[f["offset"]],
                            lots=_wdec(f["quantity"]),
                            price=_wdec(f["price"]),
                        )
                        for f in fill_rows
                    ],
                    run_id="bt-rpc",
                    repository=str(self.repo),
                ),
            )

        logger = lab_log.ProcessLog("lab", self.root)
        service = Service("lab", self.lab_dir, logger)
        try:
            reply_ok = asyncio.run(service.reply(make_request(fills)))
            self.assertEqual(reply_ok.WhichOneof("result"), "reconcile_live", reply_ok)
            self.assertTrue(reply_ok.reconcile_live.ok, reply_ok.reconcile_live.report)
            report_ok = json.loads(reply_ok.reconcile_live.report)
            self.assertGreater(report_ok["summary"]["fills"]["slippage_samples"], 0)
            self.assertEqual(report_ok["summary"]["fills"]["mean_slippage_ticks"], 0.0)

            slipped = [dict(f) for f in fills]
            delta = Decimal("2") if slipped[0]["side"] == "buy" else Decimal("-2")
            slipped[0]["price"] = str(Decimal(slipped[0]["price"]) + delta)
            reply_diff = asyncio.run(service.reply(make_request(slipped)))
            self.assertEqual(reply_diff.WhichOneof("result"), "reconcile_live", reply_diff)
            self.assertFalse(reply_diff.reconcile_live.ok)
            report_diff = json.loads(reply_diff.reconcile_live.report)
            self.assertEqual(report_diff["summary"]["fills"]["max_slippage_ticks"], 2.0)
            self.assertEqual(report_diff["fill_diffs"][0]["adverse_ticks"], 2.0)
        finally:
            service.reads.shutdown(wait=True)


if __name__ == "__main__":
    unittest.main()
