"""Backtests from the warehouse through asterion_engine, and the service's runs.

Run with the bundled interpreter:
python3 -I -B backtest_test.py <source> <generated-python> <engine-directory>
"""
import asyncio
import datetime
from decimal import Decimal
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "apps/services/common/python"))
import tempfile
import time
import unittest

sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1), sys.argv.pop(1), str(Path(__file__).parent)]
from asterion.lab_pb2 import Request  # noqa: E402
from asterion_lab import backtest, chart, repository  # noqa: E402
from asterion_strategy import Bar, FIELDS, step  # noqa: E402
from asterion_lab.runs import Runs  # noqa: E402
from service_fixture import Service  # noqa: E402
from warehouse_writer import Dataset, Warehouse, dataset_of  # noqa: E402
from warehouse_fixture import DAYS, warehouse  # noqa: E402

EXAMPLES = Path(__file__).parents[2] / "apps/services/lab/examples"


def git(root, *arguments):
    subprocess.run(["git", "-C", str(root), "-c", "user.name=test", "-c", "user.email=test@invalid",
                    *arguments], check=True, capture_output=True)


COSTS = {"SHFE/rb": [Decimal(0), Decimal(3), Decimal(3), Decimal(3), Decimal("0.1"),
                     Decimal(0), Decimal(0), Decimal(0)]}


class Chart(unittest.TestCase):
    def test_periods_gather_bars_as_the_old_chart_did(self):
        d = datetime.date
        rows = [(d(2026, 1, 5), *[Decimal(10)] * 4, Decimal(1)), (d(2026, 1, 6), None, None, None,
                                                                 Decimal(9), Decimal(0)),
                (d(2026, 3, 31), Decimal(11), Decimal(14), Decimal(8), Decimal(12), Decimal(2)),
                (d(2026, 4, 1), *[Decimal(13)] * 4, Decimal(3))]
        quarters = chart.calendar_bars(rows, "quarter")
        # The day without an auction is left out; a quarter ends on its last day.
        self.assertEqual([(bar["trading_day"], bar["open"], bar["high"], bar["low"], bar["close"],
                           bar["volume"]) for bar in quarters],
                         [(d(2026, 3, 31), 10, 14, 8, 12, 3), (d(2026, 4, 1), 13, 13, 13, 13, 3)])
        minutes = [("2026-01-06", 60_000 * n, n == 2, *[Decimal(n)] * 4, Decimal(1)) for n in range(4)]
        two = chart.minute_bars(minutes, 5)
        self.assertEqual([(bar["time_ms"], bar["open"], bar["close"], bar["volume"], bar["partial"])
                          for bar in two], [(300_000, 0, 3, 4, True)])

    def test_macd_waits_for_33_bars(self):
        values = chart.macd([Decimal(n) for n in range(40)])
        self.assertIsNone(values[32])
        diff, signal, histogram = values[33]
        self.assertAlmostEqual(histogram, 2 * (diff - signal))
        self.assertGreater(diff, 0)


class Backtests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name) / "lab"
        self.directory.mkdir()
        warehouse(self.directory)
        self.repository = Path(self.temporary.name) / "strategies"
        shutil.copytree(EXAMPLES, self.repository)
        git(self.repository, "init", "-q")
        git(self.repository, "add", ".")
        git(self.repository, "commit", "-qm", "examples")

    def tearDown(self):
        self.temporary.cleanup()

    def config(self, symbols, **changes):
        values = dict(repository=str(self.repository), strategy="ma_cross.MaCross",
                      params={"fast": 2, "slow": 4}, provider="tushare", symbols=symbols,
                      begin_day=DAYS[0].isoformat(), end_day=DAYS[-1].isoformat(),
                      deposit=Decimal(1_000_000), costs=COSTS, slippage_ticks=0,
                      max_order_lots=Decimal(10), max_gross_lots=Decimal(10), max_working_orders=10)
        values.update(changes)
        values.setdefault("dataset_version", Dataset(self.directory).current().identity)
        if values.get("minutes"):
            values.setdefault("minute_dataset_version", dataset_of(
                self.directory, values["minute_provider"]).current().identity)
        return backtest.Config(**values)

    def publish_incomplete(self):
        writer = Warehouse(self.directory).writer()
        writer.put_rows("daily/SHFE/2026-01", "daily", [], days=[])
        return writer.commit().identity

    def test_a_run_reads_its_pinned_version_when_new_data_are_published(self):
        config = self.config(("SHFE/rb/2026-05",))
        current = self.publish_incomplete()
        self.assertNotEqual(config.dataset_version, current)
        backtest.run(self.directory, "pinned", config)
        record = Runs(self.directory).record("pinned")
        self.assertEqual(record["data"]["dataset_version"], config.dataset_version)
        self.assertEqual(Runs(self.directory).list()[0]["state"], "succeeded")
        self.assertEqual(len(Runs(self.directory).table("pinned", "settlements")), len(DAYS))
        with self.assertRaisesRegex(ValueError, "unreadable dataset version"):
            backtest.run(self.directory, "unavailable", self.config(
                ("SHFE/rb/2026-05",), dataset_version="a" * 64))
        self.assertFalse((Runs(self.directory).root / "unavailable").exists())

    def test_admission_pins_both_sources_and_rejects_conflicting_same_source_versions(self):
        service = Service("lab", self.directory, type("Log", (), {"write": lambda *a, **k: None})())
        minute_store = Warehouse(self.directory / "sources/recorder", "recorder")
        writer = minute_store.writer()
        bar = dict(exchange="SHFE", product="rb", contract="SHFE/rb/2026-05",
                   day=DAYS[0].isoformat(), start_ms=1767574800000, partial=False,
                   open=Decimal(3000), high=Decimal(3005), low=Decimal(2995), close=Decimal(3000),
                   volume=Decimal(100), average_price=None, open_interest=Decimal(100))
        key = f"minutes/SHFE/{DAYS[0].isoformat()}/rb/2026-05"
        writer.put_rows(key, "minutes", [bar], days=[bar["day"]])
        minute_version = writer.commit().identity
        request = self.sweep_request("sources", [2], [4], 1).sweep.base
        request.minutes, request.minute_provider = 1, "recorder"
        pinned = service.pin_backtest(request)
        self.assertEqual(pinned.minute_dataset_version, minute_version)
        self.publish_incomplete()
        writer.put_rows(key, "minutes", [{**bar, "close": Decimal(3010)}], days=[bar["day"]])
        self.assertNotEqual(writer.commit().identity, minute_version)
        data = backtest.open_data(self.directory, backtest.config_of(pinned))
        self.assertEqual(data.version.identity, pinned.dataset_version)
        self.assertEqual(data.minute_version.identity, minute_version)
        self.assertEqual(data.minute_bars(bar["contract"], bar["day"], bar["day"], 1)[0]["close"],
                         Decimal(3000))
        request.minute_provider = "tushare"
        request.dataset_version = pinned.dataset_version
        same = service.pin_backtest(request)
        self.assertEqual(same.dataset_version, same.minute_dataset_version)
        request.minute_dataset_version = Dataset(self.directory).current().identity
        with self.assertRaisesRegex(ValueError, "same dataset version"):
            service.pin_backtest(request)

    def test_the_data_are_read_as_the_old_data_service_reads_them(self):
        data = backtest.Data(Warehouse(self.directory), DAYS[0].isoformat(), DAYS[-1].isoformat())
        self.assertEqual(data.multiplier("SHFE", "SHFE/rb/2026-05"), Decimal(10))
        self.assertEqual(set(data.increments("SHFE", "rb").values()), {5 * 10 ** 8})
        contracts, series, first = backtest.prepare(data, self.config(("SHFE/rb",)))
        # 2605 until day 15 rolls into 2610, which joins a day before the roll.
        self.assertEqual([item["symbol"] for item in contracts], ["SHFE/rb/2026-05", "SHFE/rb/2026-10"])
        (rolls,) = series
        self.assertEqual([(day, index) for day, index, _ in rolls],
                         [(DAYS[1].isoformat(), 0), (DAYS[16].isoformat(), 1)])
        self.assertEqual(contracts[1]["bars"][0][0], DAYS[15].isoformat())
        self.assertEqual(contracts[0]["bars"][-1][0], DAYS[17].isoformat())

    def test_a_contract_and_a_series_from_start_to_record(self):
        for identity, symbols in (("contract", ("SHFE/rb/2026-05",)), ("series", ("SHFE/rb",))):
            with self.subTest(symbols=symbols):
                backtest.run(self.directory, identity, self.config(symbols))
                runs = Runs(self.directory)
                (row,) = [row for row in runs.list() if row["id"] == identity]
                self.assertEqual(row["state"], "succeeded", row["error"])
                fills = runs.table(identity, "fills")
                orders = runs.table(identity, "orders")
                self.assertGreater(fills.height, 1)
                self.assertEqual(set(orders["symbol"]), set(symbols))
                # Long while rising, short while falling: a buy, then sells.
                self.assertEqual(fills["side"][0], "buy")
                self.assertIn("sell", fills["side"].to_list())
                settlements = runs.table(identity, "settlements")
                self.assertEqual(settlements.height, len(DAYS) - (1 if symbols == ("SHFE/rb",) else 0))
                self.assertIsNotNone(row["metrics"]["total_return"])
        self.assertIn("SHFE/rb/2026-10", set(Runs(self.directory).table("series", "fills")["contract"]))

    def test_minute_bars_are_read_gathered_and_traded(self):
        # Ten minutes 09:00-09:10 Beijing on each of the first ten days, up
        # then down, on top of the days' settlements.
        writer = Warehouse(self.directory).writer()
        bj = datetime.timezone(datetime.timedelta(hours=8))
        for n, day in enumerate(DAYS[:10]):
            start = int(datetime.datetime.combine(day, datetime.time(9), bj).timestamp()) * 1000
            rows = []
            for minute in range(10):
                # On the 5-yuan grid the days' prices imply.
                close = Decimal(3000 + 5 * (n * 10 + minute if n < 5 else 100 - n * 10 - minute))
                rows.append(dict(exchange="SHFE", product="rb", contract="SHFE/rb/2026-05",
                                 day=day.isoformat(), start_ms=start + minute * 60000, partial=False,
                                 open=close, high=close + 5, low=close - 5, close=close,
                                 volume=Decimal(100), average_price=None, open_interest=Decimal(900 + minute)))
            writer.put_rows(f"minutes/SHFE/{day.isoformat()}/rb/2026-05", "minutes", rows,
                            days=[day.isoformat()])
        writer.commit()
        for minutes, marks in ((1, 10), (5, 2)):
            with self.subTest(minutes=minutes):
                identity = f"minutes-{minutes}"
                backtest.run(self.directory, identity, self.config(
                    ("SHFE/rb/2026-05",), end_day=DAYS[9].isoformat(), minutes=minutes,
                    minute_provider="tushare"))
                runs = Runs(self.directory)
                (row,) = [row for row in runs.list() if row["id"] == identity]
                self.assertEqual(row["state"], "succeeded", row["error"])
                self.assertEqual(runs.record(identity)["data"]["bars"]["period"], f"{minutes}m")
                equity = runs.table(identity, "equity")
                self.assertEqual((equity["event"] == "mark").sum(), 10 * marks)
                self.assertEqual(runs.table(identity, "settlements").height, 10)
                fills = runs.table(identity, "fills")
                self.assertGreater(fills.height, 0)
                # A fill happens at a minute's end, never at the days' 15:00 label.
                self.assertTrue(all(t % (60 * 10 ** 9) == 0 for t in fills["time_ns"]))
                self.assertFalse(any(t == backtest.close_ns(d.isoformat())
                                     for t in fills["time_ns"] for d in DAYS[:10]))
                import asterion_engine
                config = self.config(("SHFE/rb/2026-05",), end_day=DAYS[9].isoformat(),
                                     minutes=minutes, minute_provider="tushare")
                contracts, series, first = backtest.prepare(backtest.open_data(self.directory, config), config)
                engine = asterion_engine.Replay(backtest.raw(config.deposit), contracts, series,
                                               (10 * backtest.R, 10 * backtest.R, 10))
                context = backtest.warmup(engine, repository.load(
                    self.repository, config.strategy, config.params),
                    {engine.unit(index): symbol for symbol, index in first.items()}, contracts)
                self.assertEqual(len(context.history("SHFE/rb/2026-05", "close", 1000)), 10 * marks)
                self.assertEqual(engine.orders(), [])
                self.assertEqual(engine.fills(), [])
        with self.assertRaisesRegex(ValueError, "minute periods are"):
            backtest.minutes_of(7)

    def test_every_example_rule_trades(self):
        examples = {"ma_cross.MaCross": {"fast": 2, "slow": 4}, "breakout.Breakout": {"entry": 3, "exit": 2},
                    "momentum.Momentum": {"lookback": 3}, "reversion.Reversion": {"window": 4, "width": 0.5}}
        for strategy, params in examples.items():
            with self.subTest(strategy=strategy):
                identity = strategy.split(".")[0]
                backtest.run(self.directory, identity, self.config(
                    ("SHFE/rb/2026-05",), strategy=strategy, params=params))
                (row,) = [row for row in Runs(self.directory).list() if row["id"] == identity]
                self.assertEqual(row["state"], "succeeded", row["error"])
                self.assertGreater(Runs(self.directory).table(identity, "fills").height, 0)

    def test_warmup_rebuilds_the_same_history_without_orders(self):
        import asterion_engine

        cases = [(("SHFE/rb/2026-05",), {}), (("SHFE/rb",), {}),
                 (("SHFE/rb/2026-05", "SHFE/rb/2026-10"),
                  dict(strategy="momentum_rank.MomentumRank",
                       params={"lookback": 2, "rebalance": 1, "weight": 0.05},
                       max_gross_weight=Decimal(1), max_net_weight=Decimal(1)))]
        for symbols, settings in cases:
            with self.subTest(symbols=symbols):
                config = self.config(symbols, **settings)
                contracts, series, first = backtest.prepare(backtest.open_data(self.directory, config), config)

                def engine():
                    return asterion_engine.Replay(backtest.raw(config.deposit), contracts, series,
                                                 (10 * backtest.R, 10 * backtest.R, 10))

                trading, warming = engine(), engine()
                units = {trading.unit(index): symbol for symbol, index in first.items()}
                strategy = repository.load(self.repository, config.strategy, config.params)
                observed = []
                traded = backtest.replay(trading, strategy, units, contracts, config,
                                        lambda bars, wanted: observed.append((bars, dict(wanted))))
                self.assertGreater(len(trading.fills()), 0)
                warmed_strategy = repository.load(self.repository, config.strategy, config.params)
                warmed = backtest.warmup(warming, warmed_strategy, units, contracts)
                self.assertEqual(warming.orders(), [])
                self.assertEqual(warming.fills(), [])
                self.assertEqual(warmed.wanted, {})
                self.assertEqual((warmed.time, warmed.mode), (traded.time, traded.mode))
                self.assertTrue(any(wanted for _, wanted in observed))
                for symbol in symbols:
                    for field in FIELDS:
                        self.assertEqual(warmed.history(symbol, field, 1000).tolist(),
                                         traded.history(symbol, field, 1000).tolist())
                # Continue both strategies with the same next bar, preserving
                # internal counters as well as ctx.history and its target mode.
                following = [Bar(bar.symbol, bar.contract, bar.time + datetime.timedelta(days=1),
                                 bar.open, bar.high, bar.low, bar.close, bar.volume, bar.open_interest)
                             for bar in observed[-1][0]]
                self.assertEqual(step(warmed_strategy, warmed, following),
                                 step(strategy, traded, following))

    def test_a_ranking_holds_weights_of_equity(self):
        symbols = ("SHFE/rb/2026-05", "SHFE/rb/2026-10")
        config = self.config(symbols, strategy="momentum_rank.MomentumRank",
                             params={"lookback": 2, "rebalance": 1, "weight": 0.05})
        with self.assertRaisesRegex(ValueError, "budget"):
            backtest.run(self.directory, "unbudgeted", config)
        config.max_gross_weight = config.max_net_weight = Decimal(1)
        backtest.run(self.directory, "ranked", config)
        fills = Runs(self.directory).table("ranked", "fills")
        # 5% of a million at about 3000 times 10 a lot: one lot, toward zero.
        self.assertEqual(set(fills["quantity"].to_list()), {Decimal(1)})
        self.assertEqual(set(fills["contract"]), set(symbols))

    def test_statistics_of_the_run_and_of_its_holdout(self):
        config = self.config(("SHFE/rb/2026-05",), params={"fast": 2, "slow": 4}, annual_periods=252.0,
                             zero_risk_free=True, holdout_day=DAYS[20].isoformat())
        backtest.run(self.directory, "measured", config)
        (row,) = Runs(self.directory).list()
        metrics = row["metrics"]
        for name in backtest.STATISTICS:
            self.assertIn(f"development_{name}", metrics)
            self.assertIn(f"holdout_{name}", metrics)
        self.assertIsNotNone(metrics["sharpe"])
        # Shorter than a year: no annual return, as the old engine reports.
        self.assertIsNone(metrics["annual_return"])
        # The two parts compound into the whole.
        whole = (1 + metrics["development_total_return"]) * (1 + metrics["holdout_total_return"]) - 1
        self.assertAlmostEqual(whole, metrics["total_return"], places=9)
        self.assertEqual(Runs(self.directory).record("measured")["statistics"]["holdout_day"],
                         DAYS[20].isoformat())
        with self.assertRaisesRegex(ValueError, "holdout"):
            backtest.run(self.directory, "late", self.config(
                ("SHFE/rb/2026-05",), params={"fast": 2, "slow": 4}, holdout_day="2027-01-01"))

    def test_a_failed_run_is_recorded(self):
        with self.assertRaises(ValueError):
            backtest.run(self.directory, "missing", self.config(("SHFE/rb/2027-01",)))
        (row,) = Runs(self.directory).list()
        self.assertEqual((row["state"], row["error"]), ("failed", "no daily bars of SHFE/rb/2027-01"))
        with self.assertRaisesRegex(ValueError, "no costs"):
            backtest.run(self.directory, "costs", self.config(("SHFE/rb/2026-05",), costs={}))

    def test_a_missing_limit_or_cost_is_named(self):
        run = self.sweep_request("named", [2], [4], 1).sweep.base
        run.max_order_lots = ""
        with self.assertRaisesRegex(ValueError, "max_order_lots must be a number, not ''"):
            backtest.config_of(run)
        run.max_order_lots = "10"
        run.costs[0].values[1] = "3元"
        with self.assertRaisesRegex(ValueError, "SHFE/rb open_fee must be a number"):
            backtest.config_of(run)

    def test_stored_contracts_and_their_minutes_are_listed(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        writer = Warehouse(self.directory).writer()
        bar = dict(exchange="SHFE", product="rb", contract="SHFE/rb/2026-05", day=DAYS[1].isoformat(),
                   partial=False, open=Decimal(3000), high=Decimal(3002), low=Decimal(2999),
                   close=Decimal(3001), volume=Decimal(4), average_price=None,
                   open_interest=Decimal(100))
        writer.put_rows(f"minutes/SHFE/{DAYS[1].isoformat()}/rb/2026-05", "minutes",
                        [{**bar, "start_ms": 1767618000000}, {**bar, "start_ms": 1767618060000}],
                        days=[DAYS[1].isoformat()])
        writer.commit()

        async def scenario():
            service = Service("lab", self.directory, Log())
            listed = (await service.reply(Request(version=1, service_id="lab",
                                                  stored_contracts={"provider": "tushare"}))).stored_contracts
            stored = {item.contract: item for item in listed.contracts}
            self.assertEqual(sorted(stored), ["SHFE/rb/2026-05", "SHFE/rb/2026-10"])
            may = stored["SHFE/rb/2026-05"]
            self.assertEqual((may.name, may.daily_first, may.daily_days), ("SHFE/rb/2026-05", DAYS[0].isoformat(), 30))
            self.assertEqual((may.minute_first, may.minute_days), (DAYS[1].isoformat(), 1))
            self.assertEqual(stored["SHFE/rb/2026-10"].minute_days, 0)
            query = Request(version=1, service_id="lab", chart_bars={
                "contract": "SHFE/rb/2026-05", "provider": "tushare", "period": "5"})
            five = (await service.reply(query)).chart_bars.bars
            # Both minutes began within 21:00-21:05; the bar ends then.
            self.assertEqual([(item.time_ms, item.open, item.close, item.volume) for item in five],
                             [(1767618300000, "3000", "3001", "8")])
            query.chart_bars.period = "week"
            weeks = (await service.reply(query)).chart_bars.bars
            self.assertEqual(len(weeks), 6)
            self.assertEqual(weeks[0].trading_day, DAYS[4].isoformat())
            self.assertFalse(weeks[0].HasField("macd_diff"))
            query.chart_bars.period = "day"
            original = (await service.reply(query)).chart_bars
            days = original.bars
            # Thirty days: all within MACD's warm-up.
            self.assertEqual(len(days), 30)
            self.assertFalse(any(item.HasField("macd_diff") for item in days))
            self.publish_incomplete()
            query.chart_bars.dataset_version = original.dataset_version
            self.assertEqual((await service.reply(query)).chart_bars, original)
            query.chart_bars.dataset_version = "a" * 64
            self.assertEqual((await service.reply(query)).error.code, "invalid_request")
            query.chart_bars.dataset_version = ""
            current = (await service.reply(query)).chart_bars
            self.assertNotEqual(current.dataset_version, original.dataset_version)
            self.assertLess(len(current.bars), len(original.bars))
        asyncio.run(scenario())

    def test_the_service_runs_a_backtest_in_a_child_process(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            request = Request(version=1, service_id="lab")
            run = request.backtest
            run.id, run.repository, run.strategy = "child", str(self.repository), "ma_cross.MaCross"
            run.params["fast"].integer = 2
            run.params["slow"].integer = 4
            run.symbols.append("SHFE/rb/2026-05")
            run.begin_day, run.end_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
            run.deposit, run.max_order_lots, run.max_gross_lots = "1000000", "10", "10"
            run.max_working_orders = 10
            run.costs.add(product="SHFE/rb", values=[str(value) for value in COSTS["SHFE/rb"]])
            replies = await asyncio.gather(service.reply(request), service.reply(request))
            self.assertEqual(sorted(reply.WhichOneof("body") for reply in replies), ["accepted", "error"])
            self.assertEqual(next(reply for reply in replies if reply.HasField("error")).error.code,
                             "invalid_request")
            # Accepted means recorded: the run can be read at once.
            query = Request(version=1, service_id="lab", run={"id": "child"})
            self.assertEqual((await service.reply(query)).WhichOneof("body"), "run")
            self.assertEqual((await service.reply(request)).error.code, "invalid_request")
            deadline = time.monotonic() + 30
            while "child" in service.backtests and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            listed = await service.reply(Request(version=1, service_id="lab", runs={}))
            (row,) = listed.runs.runs
            self.assertEqual(row.state, "succeeded", row.error)
            self.assertIn("total_return", row.metrics)
            detail = (await service.reply(Request(version=1, service_id="lab", run={"id": "child"}))).run
            self.assertEqual(detail.summary.state, "succeeded")
            report_reply = await service.reply(Request(version=1, service_id="lab",
                                                       run_report={"id": "child"}))
            self.assertEqual(report_reply.WhichOneof("body"), "run_report", report_reply.error)
            report = report_reply.run_report
            self.assertTrue(report.quantstats_version)
            self.assertLessEqual({"tail_ratio", "ulcer_index", "value_at_risk",
                                  "conditional_value_at_risk"}, set(report.figures))
            missing = await service.reply(Request(version=1, service_id="lab",
                                                  run_report={"id": "nothing"}))
            self.assertEqual(missing.WhichOneof("body"), "error")
            self.assertIn('"commit"', detail.record)
            self.assertEqual(len(detail.days), len(DAYS))
            self.assertTrue(detail.fills and detail.equity[-1].settlement)
            self.assertEqual(Decimal(detail.days[-1].equity), Decimal(detail.equity[-1].equity))
            run.id, run.repository = "elsewhere", str(Path(self.temporary.name) / "missing")
            self.assertEqual((await service.reply(request)).error.code, "invalid_request")

        asyncio.run(scenario())

    def sweep_request(self, identity, fast, slow, processes):
        request = Request(version=1, service_id="lab")
        sweep = request.sweep
        sweep.id, sweep.processes = identity, processes
        run = sweep.base
        run.repository, run.strategy = str(self.repository), "ma_cross.MaCross"
        run.symbols.append("SHFE/rb/2026-05")
        run.begin_day, run.end_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
        run.deposit, run.max_order_lots, run.max_gross_lots = "1000000", "10", "10"
        run.max_working_orders = 10
        run.annual_periods, run.zero_risk_free, run.holdout_day = 252, True, DAYS[20].isoformat()
        run.costs.add(product="SHFE/rb", values=[str(value) for value in COSTS["SHFE/rb"]])
        for name, values in (("fast", fast), ("slow", slow)):
            item = sweep.values.add(name=name)
            for value in values:
                item.values.add(integer=value)
        return request

    def test_a_sweep_runs_every_combination_in_parallel_and_selects_by_development_sharpe(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            request = self.sweep_request("grid", [2, 3], [4, 5], 2)
            fixed_version = Dataset(self.directory).current().identity
            self.assertEqual((await service.reply(request)).WhichOneof("body"), "accepted")
            # The queued candidates must keep the inputs accepted for the whole sweep.
            self.assertNotEqual(self.publish_incomplete(), fixed_version)
            self.assertLessEqual(len(service.backtests), 2)
            self.assertEqual((await service.reply(request)).error.code, "invalid_request")
            query = Request(version=1, service_id="lab", sweep_runs={"id": "grid"})
            self.assertTrue((await service.reply(query)).sweep_runs.active)
            deadline = time.monotonic() + 60
            while (service.backtests or service.waiting) and time.monotonic() < deadline:
                self.assertLessEqual(len(service.backtests), 2)
                await asyncio.sleep(0.1)
            reply = (await service.reply(query)).sweep_runs
            self.assertFalse(reply.active)
            self.assertEqual([run.id for run in reply.runs], ["grid-0", "grid-1", "grid-2", "grid-3"])
            self.assertEqual([(run.params["fast"].integer, run.params["slow"].integer) for run in reply.runs],
                             [(2, 4), (2, 5), (3, 4), (3, 5)])
            self.assertEqual({run.state for run in reply.runs}, {"succeeded"})
            self.assertEqual({run.dataset_version for run in reply.runs}, {fixed_version})
            sharpe = {run.id: run.metrics["development_sharpe"] for run in reply.runs
                      if "development_sharpe" in run.metrics}
            self.assertEqual(reply.selected, max(sharpe, key=sharpe.get) if sharpe else "")

        asyncio.run(scenario())

    def test_rolling_validation_follows_each_rounds_choice(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            request = self.sweep_request("walk", [2, 3], [4], 2)
            request.sweep.base.holdout_day = ""
            request.sweep.walk_forward.training_days, request.sweep.walk_forward.validation_days = 20, 3
            fixed_version = Dataset(self.directory).current().identity
            self.assertEqual((await service.reply(request)).WhichOneof("body"), "accepted")
            self.assertNotEqual(self.publish_incomplete(), fixed_version)
            deadline = time.monotonic() + 60
            while (service.backtests or service.waiting or service.rolling) and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            reply = (await service.reply(Request(version=1, service_id="lab", sweep_runs={"id": "walk"}))).sweep_runs
            self.assertEqual(reply.rolling.state, "succeeded", reply.rolling.error)
            self.assertEqual(reply.rolling.dataset_version, fixed_version)
            self.assertEqual({run.dataset_version for run in reply.runs}, {fixed_version})
            self.assertIn("out_of_sample_total_return", reply.rolling.metrics)

        asyncio.run(scenario())
        runs = Runs(self.directory)
        rolling = runs.record("walk-rolling")["statistics"]["rolling"]
        self.assertEqual([item["first_day"] for item in rolling["rounds"]],
                         [DAYS[n].isoformat() for n in (20, 23, 26, 29)])
        for item in rolling["rounds"]:
            scores = [score for score in item["sharpe"] if score is not None]
            if scores:
                self.assertEqual(item["sharpe"][item["selected"]], max(scores))
            else:
                self.assertIsNone(item["selected"])
        # Nothing is held before the first round's validation days.
        first = backtest.close_ns(DAYS[20].isoformat())
        self.assertTrue(all(time_ns >= first for time_ns in runs.table("walk-rolling", "fills")["time_ns"]))
        request = self.sweep_request("refused", [2, 3], [4], 2)
        request.sweep.walk_forward.training_days, request.sweep.walk_forward.validation_days = 20, 3

        async def refused():
            service = Service("lab", self.directory, type("Log", (), {"write": lambda *a, **k: None})())
            self.assertEqual((await service.reply(request)).error.code, "invalid_request")

        asyncio.run(refused())
        with self.assertRaises(ValueError):
            Runs(self.directory).sweep("refused")

    def test_a_cancelled_sweep_starts_nothing_more_and_a_new_service_interrupts_leftovers(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            await service.reply(self.sweep_request("stopped", [2, 3, 4], [5, 6, 7], 1))
            self.assertEqual((len(service.backtests), len(service.waiting)), (1, 8))
            deadline = time.monotonic() + 30
            while not any(row["id"] == "stopped-0" for row in Runs(self.directory).list()) \
                    and time.monotonic() < deadline:
                await asyncio.sleep(0.01)
            cancel = Request(version=1, service_id="lab", cancel_sweep={"id": "stopped"})
            self.assertEqual((await service.reply(cancel)).WhichOneof("body"), "accepted")
            self.assertFalse(service.waiting)
            deadline = time.monotonic() + 30
            while service.backtests and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            reply = (await service.reply(Request(version=1, service_id="lab", sweep_runs={"id": "stopped"}))).sweep_runs
            self.assertEqual(len(reply.runs), 9)
            self.assertIn(reply.runs[0].state, ("interrupted", "succeeded"))
            self.assertEqual({run.state for run in reply.runs[1:]}, {""})
            # Reusing a never-started candidate would attach unrelated results
            # to the cancelled sweep, including after a service restart.
            reused = Request(version=1, service_id="lab")
            reused.backtest.CopyFrom(self.sweep_request("other", [2], [4], 1).sweep.base)
            reused.backtest.id = "stopped-1"
            for owner in (service, Service("lab", self.directory, Log())):
                response = await owner.reply(reused)
                self.assertEqual(response.error.code, "invalid_request")
                self.assertFalse(owner.backtests)
            self.assertFalse((self.directory / "runs/stopped-1").exists())

        asyncio.run(scenario())
        runs = Runs(self.directory)
        runs.index.execute("update runs set state = 'running' where id = 'stopped-0'")
        Service("lab", self.directory, type("Log", (), {"write": lambda *a, **k: None})())
        self.assertEqual(next(row for row in Runs(self.directory).list() if row["id"] == "stopped-0")["state"],
                         "interrupted")


if __name__ == "__main__":
    unittest.main()
