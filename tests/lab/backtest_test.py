"""Backtests from the warehouse through asterion_engine, and the service's runs.

Run with the bundled interpreter:
python3 -I -B backtest_test.py <source> <generated-python> <engine-directory>
"""
import asyncio
from contextlib import asynccontextmanager
import datetime
from decimal import Decimal
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "apps/services/common/python"))
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1), sys.argv.pop(1), str(Path(__file__).parent)]
from asterion.common_pb2 import ErrorCode, Period  # noqa: E402
from asterion.lab_pb2 import AdmittedRun, Request, RunState  # noqa: E402

INVALID = ErrorCode.ERROR_CODE_INVALID_REQUEST
SUCCEEDED = RunState.RUN_STATE_SUCCEEDED
from asterion_lab import backtest, chart, repository, rolling, worker  # noqa: E402
from asterion_strategy import Bar, FIELDS, Strategy, step  # noqa: E402
from asterion_lab.runs import Runs  # noqa: E402
from service_fixture import Service  # noqa: E402
from warehouse_writer import Dataset, Warehouse  # noqa: E402
from warehouse_fixture import DAYS, daily, price_tick, settlement, warehouse  # noqa: E402

EXAMPLES = Path(__file__).resolve().parents[2] / "research"


def git(root, *arguments):
    subprocess.run(["git", "-C", str(root), "-c", "user.name=test", "-c", "user.email=test@invalid",
                    *arguments], check=True, capture_output=True)


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
        two = chart.minute_bars(minutes, 5, "SHFE/rb/2026-05")
        self.assertEqual([(bar["time_ms"], bar["open"], bar["close"], bar["volume"], bar["partial"])
                          for bar in two], [(300_000, 0, 3, 4, True)])

    def test_missing_trading_minutes_and_the_scheduled_break(self):
        day = datetime.date(2026, 1, 6)
        midnight = int(datetime.datetime.combine(day, datetime.time(), chart.BEIJING).timestamp()) * 1000
        def minute(slot, traded=True):
            price = Decimal(10) if traded else None
            return (day, midnight + slot * 60_000, False, price, price, price, price, int(traded), slot)

        contract = "SHFE/rb/2026-05"
        slots = [540, 541, 543, 544, 545, 546, 547, 548, 550, 551, 552, 553, 554]
        bars = chart.minute_bars([minute(slot, slot != 554) for slot in slots], 5, contract)
        self.assertEqual([(bar["partial"], bar["volume"]) for bar in bars],
                         [(True, 4), (True, 4), (False, 4)])
        self.assertEqual(bars[-1]["open_interest"], 554)
        slots = [*range(600, 615), *range(630, 660)]
        bar, = chart.minute_bars([minute(slot) for slot in slots], 60, contract)
        self.assertEqual((bar["partial"], bar["volume"]), (False, 45))
        self.assertEqual([item["partial"] for item in chart.minute_bars([minute(539), minute(540)], 1, contract)],
                         [True, False])

    def test_macd_waits_for_33_bars(self):
        values = chart.macd([Decimal(n) for n in range(40)])
        self.assertIsNone(values[32])
        diff, signal, histogram = values[33]
        self.assertAlmostEqual(histogram, 2 * (diff - signal))
        self.assertGreater(diff, 0)

    def test_overnight_gaps_follow_the_session_date_and_exchange_calendar(self):
        day, contract = "2026-01-12", "SHFE/au/2026-06"
        def ms(local):
            return int(datetime.datetime.fromisoformat(local).replace(tzinfo=chart.BEIJING).timestamp()) * 1000
        row = (day, ms("2026-01-10 00:01"), False, *[Decimal(99)] * 4, Decimal(1))
        _, monday = chart.minute_coverage(day, [row], 30, contract, {})
        self.assertFalse(monday["unchecked"])
        self.assertIn(ms("2026-01-09 21:30"), monday["missing"])
        self.assertIn(ms("2026-01-10 02:30"), monday["missing"])
        self.assertEqual(monday["partial"], [ms("2026-01-10 00:30")])
        for calendar, listed, unchecked in (({"2026-01-09": False}, None, False),
                                            ({"2026-01-09": True}, datetime.date.fromisoformat(day), True),
                                            ({}, None, True)):
            with self.subTest(calendar=calendar, listed=listed):
                _, coverage = chart.minute_coverage(day, [], 30, contract, calendar, listed)
                self.assertEqual(len(coverage["missing"]), 8)
                self.assertEqual(coverage["unchecked"], unchecked)


class Readiness(unittest.TestCase):
    def contract(self, symbol, days, closes, times=None):
        r = backtest.R
        times = times or [backtest.close_ns(day) for day in days]
        bars = [(day, stamp, close * r, (close + 10) * r, (close - 10) * r,
                 close * r, 1000 * r) for day, stamp, close in zip(days, times, closes)]
        return dict(venue="SHFE", symbol=symbol, price_increment=r, quantity_increment=r,
                    multiplier=10 * r, increments=[], slippage_ticks=0, bars=bars,
                    costs=[dict(effective_from=days[0], source="test",
                                values=[0, 0, 0, 0, r // 10, 0, 0, 0])],
                    settlements=list(dict((day, (day, close * r))
                                          for day, close in zip(days, closes)).values()),
                    open_interest={stamp: 1000 for stamp in times})

    def engine(self, contracts, series=(), order_lots=10):
        import asterion_engine
        r = backtest.R
        return asterion_engine.Replay(1_000_000 * r, contracts, list(series),
                                     (order_lots * r, 10 * r, 10, 10 * r))

    def test_not_ready_cancels_another_contracts_pending_target(self):
        first, second = "SHFE/rb/2026-05", "SHFE/rb/2026-10"
        day = "2026-01-05"
        minute = 60 * 10 ** 9
        start = backtest.close_ns(day) - 180 * minute
        contracts = [self.contract(first, [day] * 3, [100] * 3,
                                   [start, start + minute, start + 3 * minute]),
                     self.contract(second, [day] * 2, [100] * 2, [start, start + 2 * minute])]

        class Once(Strategy):
            def __init__(self):
                super().__init__()
                self.count = 0

            def on_bars(self, ctx, bars):
                self.count += 1
                if self.count == 1:
                    ctx.target(second, 1)

            def ready(self, ctx):
                return self.count == 1

        for followed in (False, True):
            with self.subTest(rolling=followed):
                engine = self.engine(contracts)
                if followed:
                    rolling.follow(engine, [Once()], {0: first, 1: second}, contracts, [0])
                else:
                    backtest.replay(engine, Once(), {0: first, 1: second}, contracts, None)
                self.assertEqual(engine.orders(), [])

    def test_not_ready_neither_opens_nor_closes_a_position_at_a_roll(self):
        days = [day.isoformat() for day in DAYS[:4]]

        class Once(Strategy):
            def __init__(self):
                super().__init__()
                self.count = 0

            def on_bar(self, ctx, bar):
                self.count += 1
                if self.count == 1:
                    ctx.target(bar.symbol, 3)

            def ready(self, ctx):
                return self.count == 1

        for filled in (False, True):
            for followed in (False, True):
                with self.subTest(filled=filled, rolling=followed):
                    contracts = [self.contract("SHFE/rb/2026-05", days, [100] * 4),
                                 self.contract("SHFE/rb/2026-10", days, [130] * 4)]
                    contracts[0]["bars"][1] = (*contracts[0]["bars"][1][:6],
                                               (10 if filled else 0) * backtest.R)
                    contracts[1]["roll_ratio"] = 1.0
                    engine = self.engine(contracts, [[(days[0], 0), (days[2], 1)]], order_lots=1)
                    if followed:
                        rolling.follow(engine, [Once()], {0: "SHFE/rb"}, contracts, [0] * 4)
                    else:
                        backtest.replay(engine, Once(), {0: "SHFE/rb"}, contracts, None)
                    self.assertEqual(len(engine.orders()), 1)
                    self.assertEqual(len(engine.fills()), int(filled))
                    held = [(row[1], row[2], row[3]) for row in engine.positions if row[0] == days[-1]]
                    self.assertEqual(held, [(0, backtest.R, 0)] if filled else [])

    def test_switching_to_an_unready_candidate_discards_the_previous_target(self):
        symbol = "SHFE/rb/2026-05"
        days = [day.isoformat() for day in DAYS[:4]]
        contracts = [self.contract(symbol, days, [100] * 4)]
        contracts[0]["bars"] = [(*bar[:6], 10 * backtest.R) for bar in contracts[0]["bars"]]

        class Ready(Strategy):
            def on_bar(self, ctx, bar):
                ctx.target(bar.symbol, 3)

            def ready(self, ctx):
                return True

        class Waiting(Strategy):
            def on_bar(self, ctx, bar):
                pass

            def ready(self, ctx):
                return False

        for followed in ([0, 1, 1, 1], [0, 0, 1, 1]):
            with self.subTest(followed=followed):
                engine = self.engine(contracts, order_lots=1)
                rolling.follow(engine, [Ready(), Waiting()], {0: symbol}, contracts, followed)
                self.assertEqual(len(engine.fills()), followed.count(0) - 1)

    def test_ready_again_waits_for_a_new_target_and_keeps_partial_fills(self):
        symbol = "SHFE/rb/2026-05"
        days = [day.isoformat() for day in DAYS[:8]]
        contracts = [self.contract(symbol, days, [100] * 8)]
        contracts[0]["bars"][1] = (*contracts[0]["bars"][1][:6], 10 * backtest.R)

        class Resume(Strategy):
            def __init__(self):
                super().__init__()
                self.count = 0

            def on_bar(self, ctx, bar):
                self.count += 1
                if self.count == 1:
                    ctx.target(bar.symbol, 3)
                if self.count == 5:
                    ctx.target(bar.symbol, 2)

            def ready(self, ctx):
                return self.count != 2

        for followed in (False, True):
            with self.subTest(rolling=followed):
                engine = self.engine(contracts, order_lots=1)
                if followed:
                    rolling.follow(engine, [Resume()], {0: symbol}, contracts, [0] * 8)
                else:
                    backtest.replay(engine, Resume(), {0: symbol}, contracts, None)
                self.assertEqual([fill[5] for fill in engine.fills()],
                                 [backtest.close_ns(days[1]), backtest.close_ns(days[5])])
                self.assertEqual(engine.positions[-1][2], 2 * backtest.R)


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

    def test_warmup_reads_recent_bars_of_explicit_contracts(self):
        writer = Warehouse(self.directory).writer()
        selections = [("CFFEX", "IF3005", "if"), ("CZCE", "TA005", "ta"), ("SHFE", "rb3005", "rb")]
        for exchange, _, product in selections:
            contract = f"{exchange}/{product}/2030-05"
            for month in ("2029-01", "2029-02"):
                days = [datetime.date.fromisoformat(f"{month}-{day:02}") for day in (1, 2)]
                writer.add_days(exchange, month, [daily(contract, day, 3000 + day.day, 10)
                                                   for day in days], [day.isoformat() for day in days])
        version = writer.commit()
        service = Service("lab", self.directory, None)
        cache = service.dataset()
        for key in version.keys("daily"):
            if key.endswith("2029-01"):
                (cache.root / cache.version(version.identity).manifest["partitions"][key]["file"]).unlink()
        request = Request(build_warmup=dict(
            instruments=[dict(instrument=dict(exchange=exchange, symbol=symbol),
                              contract=f"{exchange}/{product}/2030-05")
                         for exchange, symbol, product in selections],
            period=Period.PERIOD_DAY, max_bars=2))
        result = service.build_warmup(request.build_warmup)
        self.assertEqual(result.dataset_version, version.identity)
        self.assertEqual([[bar.instrument.symbol for bar in batch.bars] for batch in result.bars],
                         [["IF3005", "TA005", "rb3005"]] * 2)
        self.assertEqual([batch.bars[0].trading_day for batch in result.bars],
                         ["2029-02-01", "2029-02-02"])
        self.assertEqual([batch.bars[0].close.text for batch in result.bars], ["3001", "3002"])
        service.reads.shutdown()
        service.runs.index.close()

    def test_warmup_keeps_complete_minute_periods_across_recent_partitions(self):
        writer = Warehouse(self.directory).writer()
        for idx, day in enumerate(DAYS[1:4]):
            start = int(datetime.datetime.combine(day, datetime.time(9), chart.BEIJING).timestamp()) * 1000
            for month in ("2026-05", "2026-10"):
                contract = f"SHFE/rb/{month}"
                writer.put_rows(f"minutes/SHFE/{day}/rb/{month}", "minutes", [
                    dict(exchange="SHFE", product="rb", contract=contract, day=day.isoformat(),
                         start_ms=start + minute * 60_000, partial=idx == 2,
                         open=3000 + minute, high=3001 + minute, low=2999 + minute, close=3000 + minute,
                         volume=1, average_price=None, open_interest=100 if idx == 0 else None)
                    for minute in range(150 if idx == 2 else 10) if idx != 1 or minute != 2],
                    days=[day.isoformat()])
        writer.commit()
        service = Service("lab", self.directory, None)
        result = service.build_warmup(Request(build_warmup=dict(
            instruments=[dict(contract="SHFE/rb"), dict(instrument=dict(exchange="SHFE", symbol="rb2610"),
                                                        contract="SHFE/rb/2026-10")],
            period=Period.PERIOD_MINUTES_5, max_bars=2)).build_warmup)
        self.assertEqual([batch.bars[0].trading_day for batch in result.bars],
                         [DAYS[1].isoformat(), DAYS[2].isoformat()])
        for batch in result.bars:
            self.assertEqual([(bar.instrument.symbol, bar.open.text, bar.close.text, bar.volume)
                              for bar in batch.bars], [("rb2605", "3005", "3009", 5),
                                                      ("rb2610", "3005", "3009", 5)])
        self.assertTrue(result.bars[0].bars[0].HasField("open_interest"))
        self.assertFalse(result.bars[1].bars[0].HasField("open_interest"))
        service.reads.shutdown()
        service.runs.index.close()

    def config(self, symbols, **changes):
        values = dict(repository=str(self.repository), strategy="ma_cross.MaCross",
                      params={"fast": 2, "slow": 4}, symbols=symbols,
                      begin_day=DAYS[0].isoformat(), end_day=DAYS[-1].isoformat(),
                      deposit=Decimal(1_000_000), costs=None, slippage_ticks=0,
                      max_order_lots=Decimal(10), max_gross_lots=Decimal(10),
                      max_contract_lots=Decimal(10), max_working_orders=10)
        values.update(changes)
        values.setdefault("dataset_version", Dataset(self.directory).current().identity)
        if "source" not in values:
            # As the service does at admission: runs import this copy, never the repository.
            source = Path(tempfile.mkdtemp(dir=self.directory))
            taken = repository.snapshot(self.repository, source,
                                        f"strategies/{values['strategy'].split('.')[0]}.py")
            values.update(source=str(source), commit=taken.commit, changes=taken.changes)
        return backtest.Config(**values)

    def test_a_run_imports_the_code_copied_at_admission(self):
        config = self.config(("SHFE/rb/2026-05",), params={"fast": 2, "slow": 4})
        (self.repository / "strategies/ma_cross.py").write_text("raise RuntimeError('edited')\n")
        backtest.run(self.directory, "pinned", config)
        (row,) = Runs(self.directory).list()
        self.assertEqual(row["state"], "succeeded", row["error"])
        self.assertEqual((row["commit"], row["changes"]), (config.commit, ""))

    def test_a_days_final_high_never_changes_its_morning_fills(self):
        import asterion_engine

        contract, day = "SHFE/rb/2026-05", DAYS[0].isoformat()
        start = int(datetime.datetime.combine(DAYS[0], datetime.time(9), backtest.BEIJING).timestamp()) * 1000
        writer = Warehouse(self.directory).writer()
        writer.put_rows(f"minutes/SHFE/{day}/rb/2026-05", "minutes", [
            dict(exchange="SHFE", product="rb", contract=contract, day=day,
                 start_ms=start + minute * 60000, partial=False,
                 open=Decimal(close), high=Decimal(close + 5), low=Decimal(close - 5),
                 close=Decimal(close), volume=Decimal(100), average_price=None, open_interest=Decimal(100))
            for minute, close in enumerate((3030, 3020))], days=[day])
        writer.commit()
        config = self.config((contract,), end_day=day, minutes=1, slippage_ticks=1)

        def morning(final_high):
            data = backtest.open_data(self.directory, config)
            data.product("SHFE", "rb")[contract].days[day]["high"] = Decimal(final_high)
            item = backtest.contract_input(data, config, contract, day, day)
            engine = asterion_engine.Replay(backtest.raw(config.deposit), [item], [],
                                            (10 * backtest.R, 10 * backtest.R, 10, 10 * backtest.R))
            while not engine.finished:
                while not engine.day_done:
                    engine.next()
                    engine.want(0, backtest.R)
                    engine.decide()
                engine.settle()
            return item, engine.fills()

        before, before_fills = morning(3045)
        after, after_fills = morning(3046)
        self.assertEqual(after_fills, before_fills)
        self.assertEqual([fill[3] for fill in before_fills], [3021 * backtest.R])
        self.assertEqual(before["price_increment"], backtest.R)
        self.assertEqual(after["price_increment"], backtest.R)

    def test_each_contract_uses_its_pinned_price_increment(self):
        symbols = ("SHFE/rb/2026-05", "SHFE/rb/2026-10")
        pinned = self.config(symbols)
        writer = Warehouse(self.directory).writer()
        writer.put_rows("price_ticks/SHFE", "price_ticks", [
            price_tick(symbol, tick) for symbol, tick in zip(symbols, ("2", "0.00000001"))])
        writer.commit()
        current = self.config(symbols)
        for config, ticks in ((pinned, [backtest.R, backtest.R]),
                              (current, [2 * backtest.R, 1])):
            with self.subTest(version=config.dataset_version):
                contracts, _, _ = backtest.prepare(backtest.open_data(self.directory, config), config)
                self.assertEqual([item["price_increment"] for item in contracts], ticks)
                self.assertEqual([item["increments"] for item in contracts], [[], []])

    def test_product_price_ticks_apply_to_each_delivery_month(self):
        symbols = ("SHFE/rb/2026-05", "SHFE/rb/2026-10")
        writer = Warehouse(self.directory).writer()
        writer.put_rows("price_ticks/SHFE", "price_ticks", [
            {**price_tick(symbols[0], "2"), "contract": None},
            {**price_tick("SHFE/cu/2026-05", "10"), "contract": None}])
        writer.commit()
        config = self.config(symbols)
        contracts, _, _ = backtest.prepare(backtest.open_data(self.directory, config), config)
        self.assertEqual([item["price_increment"] for item in contracts], [2 * backtest.R] * 2)

    def test_shipped_exchange_rules_cover_six_exchanges_from_2016_to_2026_offline(self):
        rules = json.loads((Path(__file__).resolve().parents[2]
                            / "apps/services/warehouse/config/exchange_rules.json").read_text())
        writer = Warehouse(self.directory).writer()
        sample_days = [datetime.date(2016, 1, 4), datetime.date(2018, 5, 16),
                       datetime.date(2020, 6, 1), datetime.date(2023, 11, 7),
                       datetime.date(2024, 12, 18), datetime.date(2026, 5, 11)]
        chosen = ("SHFE/au/2026-12", "INE/ec/2026-12", "DCE/p/2026-09",
                  "CZCE/oi/2026-09", "CFFEX/ts/2026-12", "GFEX/lc/2026-12")
        for exchange, section in rules["price_ticks"].items():
            writer.put_rows(f"price_ticks/{exchange}", "price_ticks", [
                dict(exchange=exchange, product=product, contract=item.get("contract"),
                     effective_from=item["from"], effective_until=item.get("until", "2100-01-01"),
                     price_tick=Decimal(item["tick"]),
                     source_url=item.get("source_url", section["source_url"]))
                for product, items in section["products"].items() for item in items])
        for symbol in chosen:
            exchange, product, month = symbol.split("/")
            for year in range(2016, 2027):
                span = [datetime.date(year, 1, 1) + datetime.timedelta(days=n)
                        for n in range(366) if (datetime.date(year, 1, 1) + datetime.timedelta(days=n)).year == year]
                writer.put_rows(f"calendar/{exchange}/{year}", "calendar", [
                    dict(exchange=exchange, day=d.isoformat(), open=d in sample_days,
                         previous_trading_day=None) for d in span])
            writer.put_rows(f"contracts/{exchange}", "contracts", [dict(
                exchange=exchange, contract=symbol, product=product, delivery_month=month,
                source_instrument=symbol, name=symbol, list_date="2015-01-01",
                delist_date="2026-12-31", last_delivery_date=None, multiplier=Decimal(10),
                per_unit=Decimal(1), trade_unit="吨", quote_unit="元/吨",
                tick_description="", delivery_description="", trading_hours="")])
            bars = [{**daily(symbol, d, 5000 + n * 100, 1000),
                     "high": Decimal(5000 + n * 100 + 100),
                     "low": Decimal(5000 + n * 100 - 100)}
                    for n, d in enumerate(sample_days)]
            for d in sample_days:
                writer.put_rows(f"settlement/{exchange}/{d.isoformat()}", "settlement",
                                [settlement(symbol, d)])
            for ym in sorted({d.isoformat()[:7] for d in sample_days}):
                writer.add_days(exchange, ym, [b for b in bars if b["day"].startswith(ym)],
                                [b["day"] for b in bars if b["day"].startswith(ym)])
        version = writer.commit().identity
        config = self.config(chosen, dataset_version=version, deposit=Decimal("10000000"),
                             begin_day="2016-01-04", end_day="2026-05-11", params={"fast": 2, "slow": 4})
        backtest.run(self.directory, "six-exchanges-decade", config)
        (row,) = [r for r in Runs(self.directory).list() if r["id"] == "six-exchanges-decade"]
        self.assertEqual(row["state"], "succeeded", row["error"])

    def test_a_backtest_requires_price_ticks_for_each_trading_day(self):
        symbol = "SHFE/rb/2026-05"
        first, second, third = [day.isoformat() for day in DAYS[:3]]
        cases = [([], first),
                 ([price_tick(symbol, end=second)], second),
                 ([price_tick(symbol, end=second), price_tick(symbol, begin=third)], second),
                 ([price_tick(symbol), price_tick(symbol, "2", begin=second)], second),
                 ([price_tick(symbol), {**price_tick(symbol, "2"), "contract": None}], first),
                 ([price_tick(symbol, "0")], first),
                 ([price_tick(symbol, begin=first, end=first)], first),
                 ([{**price_tick(symbol), "effective_until": None}], first)]
        for rows, day in cases:
            with self.subTest(rows=rows):
                writer = Warehouse(self.directory).writer()
                writer.put_rows("price_ticks/SHFE", "price_ticks", rows)
                writer.commit()
                config = self.config((symbol,))
                with self.assertRaisesRegex(ValueError, f"price tick.*{symbol}.*{day}"):
                    backtest.prepare(backtest.open_data(self.directory, config), config)

    def test_price_tick_changes_apply_to_night_session_fills_by_trading_day(self):
        import asterion_engine

        symbol = "SHFE/rb/2026-05"
        first, second = [day.isoformat() for day in DAYS[:2]]
        writer = Warehouse(self.directory).writer()
        writer.put_rows("price_ticks/SHFE", "price_ticks", [
            {**price_tick(symbol, "1", end=second), "contract": None},
            {**price_tick(symbol, "2", begin=second), "contract": None},
            price_tick(symbol, "2", begin=second, source="https://example.test/second-source")])
        for day, closes in zip(DAYS[:2], ((3030, 3020), (3030, 3040))):
            evening = day - datetime.timedelta(days=3 if day.weekday() == 0 else 1)
            start = int(datetime.datetime.combine(evening, datetime.time(21), backtest.BEIJING).timestamp()) * 1000
            writer.put_rows(f"minutes/SHFE/{day}/rb/2026-05", "minutes", [
                dict(exchange="SHFE", product="rb", contract=symbol, day=day.isoformat(),
                     start_ms=start + minute * 60000, partial=False,
                     open=Decimal(close), high=Decimal(close + 10), low=Decimal(close - 10),
                     close=Decimal(close), volume=Decimal(100), average_price=None, open_interest=Decimal(100))
                for minute, close in enumerate(closes)], days=[day.isoformat()])
        writer.commit()
        config = self.config((symbol,), end_day=second, minutes=1, slippage_ticks=1)
        contracts, _, _ = backtest.prepare(backtest.open_data(self.directory, config), config)
        self.assertEqual(contracts[0]["price_increment"], backtest.R)
        self.assertEqual(contracts[0]["increments"], [(second, 2 * backtest.R)])
        engine = asterion_engine.Replay(backtest.raw(config.deposit), contracts, [],
                                        (10 * backtest.R, 10 * backtest.R, 10, 10 * backtest.R))
        while not engine.finished:
            while not engine.day_done:
                bar, = engine.next()
                engine.want(0, backtest.R if bar[4] == first else 0)
                engine.decide()
            engine.settle()
        self.assertEqual([fill[3] for fill in engine.fills()], [3021 * backtest.R, 3038 * backtest.R])
        # Monday's first fill came on Friday night: it belongs to Monday, at Friday's time.
        fill = engine.fills()[0]
        self.assertEqual(fill[6], first)
        self.assertEqual(datetime.datetime.fromtimestamp(fill[5] / 10 ** 9, backtest.BEIJING).date(),
                         DAYS[0] - datetime.timedelta(days=3))
        self.assertEqual(engine.orders()[0][9], first)
        later = self.config((symbol,), begin_day=second, end_day=second, minutes=1)
        contracts, _, _ = backtest.prepare(backtest.open_data(self.directory, later), later)
        self.assertEqual(contracts[0]["price_increment"], 2 * backtest.R)
        self.assertEqual(contracts[0]["increments"], [])

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

    def test_admission_pins_one_research_version_for_days_and_minutes(self):
        service = Service("lab", self.directory, type("Log", (), {"write": lambda *a, **k: None})())
        request = self.sweep_request("sources", [2], [4], 1).start_sweep.base
        request.period = Period.PERIOD_MINUTES_1
        with self.assertRaisesRegex(ValueError, "has no minutes partitions"):
            service.pin_backtest(request)
        writer = Warehouse(self.directory).writer()
        bar = dict(exchange="SHFE", product="rb", contract="SHFE/rb/2026-05",
                   day=DAYS[0].isoformat(), start_ms=1767574800000, partial=False,
                   open=Decimal(3000), high=Decimal(3005), low=Decimal(2995), close=Decimal(3000),
                   volume=Decimal(100), average_price=None, open_interest=Decimal(100))
        key = f"minutes/SHFE/{DAYS[0].isoformat()}/rb/2026-05"
        writer.put_rows(key, "minutes", [bar], days=[bar["day"]])
        version = writer.commit().identity
        pinned = service.pin_backtest(request)
        self.assertEqual(pinned.pinned.dataset_version, version)
        writer.put_rows(key, "minutes", [{**bar, "close": Decimal(3010)}], days=[bar["day"]])
        self.assertNotEqual(writer.commit().identity, version)
        data = backtest.open_data(self.directory, backtest.config_of(pinned))
        self.assertEqual(data.version.identity, version)
        rows = data.minute_rows(bar["contract"], bar["day"], bar["day"])
        self.assertEqual(chart.minute_bars(rows, 1, bar["contract"])[0]["close"], Decimal(3000))

    def test_contract_terms_and_dominant_rolls_are_read_from_the_dataset(self):
        writer = Warehouse(self.directory).writer()
        writer.put_rows("price_ticks/SHFE", "price_ticks", [
            price_tick("SHFE/rb/2026-05"), price_tick("SHFE/rb/2026-10", "0.2")])
        writer.commit()
        data = backtest.Data(Warehouse(self.directory), DAYS[0].isoformat(), DAYS[-1].isoformat())
        self.assertEqual(data.multiplier("SHFE", "SHFE/rb/2026-05"), Decimal(10))
        contracts, series, first = backtest.prepare(data, self.config(("SHFE/rb",)))
        self.assertEqual([item["price_increment"] for item in contracts], [backtest.R, backtest.R // 5])
        # 2605 until day 15 rolls into 2610, which joins a day before the roll.
        self.assertEqual([item["symbol"] for item in contracts], ["SHFE/rb/2026-05", "SHFE/rb/2026-10"])
        (rolls,) = series
        self.assertEqual(rolls,
                         [(DAYS[1].isoformat(), 0), (DAYS[16].isoformat(), 1)])
        self.assertEqual(contracts[1]["bars"][0][0], DAYS[15].isoformat())
        self.assertEqual(contracts[0]["bars"][-1][0], DAYS[-1].isoformat())
        # Starting after the first stored day uses the previous trading day as anchor,
        # so the dominant series is active from begin_day itself.
        anchored = self.config(("SHFE/rb",), begin_day=DAYS[5].isoformat())
        _, anchored_series, _ = backtest.prepare(
            backtest.Data(Warehouse(self.directory), anchored.begin_day, anchored.end_day), anchored)
        self.assertEqual(anchored_series, [[(DAYS[5].isoformat(), 0), (DAYS[16].isoformat(), 1)]])

    def test_a_series_rejects_missing_daily_partitions(self):
        store = Warehouse(self.directory)
        original = store.read(store.current(), "daily/SHFE/2026-01").to_dicts()
        for missing in (DAYS[0], DAYS[10]):
            with self.subTest(missing=missing):
                rows = [dict(row, day=row["day"].isoformat()) for row in original
                        if row["day"] != missing]
                writer = store.writer()
                writer.put_rows("daily/SHFE/2026-01", "daily", rows,
                                days=sorted({row["day"] for row in rows}))
                writer.commit()
                config = self.config(("SHFE/rb",))
                with self.assertRaisesRegex(ValueError, f"daily bars of SHFE are not stored on {missing}"):
                    backtest.prepare(backtest.open_data(self.directory, config), config)

    def test_later_days_never_change_what_a_series_showed_before(self):
        import asterion_engine

        class Watch(Strategy):
            def __init__(self):
                super().__init__()
                self.seen = []

            def on_bar(self, ctx, bar):
                self.seen.append(list(ctx.history(bar.symbol, "close", 100)))

        def seen(end_day):
            config = self.config(("SHFE/rb",), end_day=end_day)
            contracts, series, first = backtest.prepare(backtest.open_data(self.directory, config), config)
            engine = asterion_engine.Replay(backtest.raw(config.deposit), contracts, series,
                                            (10 * backtest.R, 10 * backtest.R, 10, 10 * backtest.R))
            strategy = Watch()
            backtest.replay(engine, strategy, {engine.unit(index): symbol for symbol, index in first.items()},
                            contracts, config)
            return strategy.seen

        # The series rolls into 2610 on day 16.
        before, after = seen(DAYS[15].isoformat()), seen(DAYS[-1].isoformat())
        self.assertEqual(after[:len(before)], before)
        self.assertNotEqual(after[-1][:len(before[-1])], before[-1])

    def test_roll_yield_is_unscaled_across_rolls_and_works_in_daily_and_minute_replays(self):
        import asterion_engine
        from asterion_lab import dominant

        def carry_fn(roll_yield):
            return roll_yield

        class WatchRollYield(Strategy):
            def __init__(self):
                super().__init__()
                self.bars = []
                self.history_after_roll = None
                self.factor_values = []

            def on_bar(self, ctx, bar):
                self.bars.append((bar.time.date(), bar.contract, bar.roll_yield))
                self.factor_values.append(ctx.factor(carry_fn, bar.symbol, 1))
                if bar.contract == "SHFE/rb/2026-10" and self.history_after_roll is None:
                    self.history_after_roll = list(ctx.history(bar.symbol, "roll_yield", 100))

        # Daily dominant series: 2026-05 -> 2026-10 roll on DAYS[16].
        config = self.config(("SHFE/rb",))
        data = backtest.open_data(self.directory, config)
        stored = data.product("SHFE", "rb")
        contracts, series, first = backtest.prepare(data, config)
        engine = asterion_engine.Replay(backtest.raw(config.deposit), contracts, series,
                                        (10 * backtest.R, 10 * backtest.R, 10, 10 * backtest.R))
        watch = WatchRollYield()
        backtest.replay(engine, watch, {engine.unit(index): symbol for symbol, index in first.items()},
                        contracts, config)
        # Before the roll, 2026-05 vs 2026-10 gives exact annualized roll yield.
        day1_close_near = Decimal(stored["SHFE/rb/2026-05"].days[DAYS[1].isoformat()]["close"])
        day1_close_far = Decimal(stored["SHFE/rb/2026-10"].days[DAYS[1].isoformat()]["close"])
        expected_day1 = float(dominant.annualized_roll_yield(
            day1_close_near, day1_close_far, "2026-05", "2026-10"))
        self.assertEqual(watch.bars[0][2], expected_day1)
        self.assertEqual(watch.factor_values[0], expected_day1)
        # Across the roll to 2026-10 (the last month), earlier roll_yield history stays unscaled
        # and the new bar has NaN (no later month).
        self.assertIsNotNone(watch.history_after_roll)
        self.assertEqual(watch.history_after_roll[0], expected_day1)
        self.assertTrue(math.isnan(watch.history_after_roll[-1]))

        # Fixed single contract receives NaN roll_yield.
        fixed_cfg = self.config(("SHFE/rb/2026-05",))
        fc, fs, ff = backtest.prepare(backtest.open_data(self.directory, fixed_cfg), fixed_cfg)
        fe = asterion_engine.Replay(backtest.raw(fixed_cfg.deposit), fc, fs,
                                    (10 * backtest.R, 10 * backtest.R, 10, 10 * backtest.R))
        fixed_watch = WatchRollYield()
        backtest.replay(fe, fixed_watch, {fe.unit(i): s for s, i in ff.items()}, fc, fixed_cfg)
        self.assertTrue(all(math.isnan(ry) for _, _, ry in fixed_watch.bars))

        # Minute dominant series: uses sub-dominant minute closes when stored, NaN when absent.
        writer = Warehouse(self.directory).writer()
        for contract, days, price_offset in (
            ("SHFE/rb/2026-05", DAYS[1:18], Decimal(0)),
            ("SHFE/rb/2026-10", DAYS[1:2] + DAYS[15:], Decimal(50)),
        ):
            for day in days:
                base_close = Decimal(stored[contract].days[day.isoformat()]["close"])
                start = int(datetime.datetime.combine(day, datetime.time(9), backtest.BEIJING)
                            .timestamp()) * 1000
                writer.put_rows(f"minutes/SHFE/{day.isoformat()}/rb/{contract[-7:]}", "minutes", [
                    dict(exchange="SHFE", product="rb", contract=contract, day=day.isoformat(),
                         start_ms=start + minute * 60000, partial=False,
                         open=base_close + price_offset, high=base_close + price_offset,
                         low=base_close + price_offset, close=base_close + price_offset,
                         volume=Decimal(10), average_price=None, open_interest=Decimal(900))
                    for minute in range(5)], days=[day.isoformat()])
        writer.commit()
        m_cfg = self.config(("SHFE/rb",), minutes=1)
        mc, ms, mf = backtest.prepare(backtest.open_data(self.directory, m_cfg), m_cfg)
        me = asterion_engine.Replay(backtest.raw(m_cfg.deposit), mc, ms,
                                    (10 * backtest.R, 10 * backtest.R, 10, 10 * backtest.R))
        m_watch = WatchRollYield()
        backtest.replay(me, m_watch, {me.unit(i): s for s, i in mf.items()}, mc, m_cfg)
        expected_min_day1 = float(dominant.annualized_roll_yield(
            day1_close_near, day1_close_far + Decimal(50), "2026-05", "2026-10"))
        # Day 1 has stored sub-dominant minutes -> exact minute roll_yield.
        day1_bars = [ry for d, _, ry in m_watch.bars if d == DAYS[1]]
        self.assertTrue(day1_bars and all(ry == expected_min_day1 for ry in day1_bars))
        # Day 2 has no stored sub-dominant minutes -> NaN without failing.
        day2_bars = [ry for d, _, ry in m_watch.bars if d == DAYS[2]]
        self.assertTrue(day2_bars and all(math.isnan(ry) for ry in day2_bars))

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

    def test_minute_runs_report_absent_periods_and_keep_zero_volume_distinct(self):
        writer = Warehouse(self.directory).writer()
        contract = "SHFE/rb/2026-05"
        monday, tuesday = DAYS[5:7]
        for day in (monday, tuesday):
            midnight = datetime.datetime.combine(day, datetime.time(), backtest.BEIJING)
            sessions = [(midnight, 540, 615), (midnight, 630, 690), (midnight, 810, 900)]
            if day == monday:
                sessions.insert(0, (midnight - datetime.timedelta(days=3), 1260, 1380))
            rows = []
            for base, begin, end in sessions:
                for slot in range(begin, end):
                    if day == monday and (545 <= slot < 550 or slot == 552):
                        continue
                    zero = day == monday and 555 <= slot < 560
                    price = None if zero else Decimal(3000)
                    rows.append(dict(exchange="SHFE", product="rb", contract=contract,
                                     day=day.isoformat(), start_ms=int(base.timestamp()) * 1000 + slot * 60_000,
                                     partial=day == tuesday and slot == 543, open=price, high=price,
                                     low=price, close=price, volume=Decimal(0 if zero else 100),
                                     average_price=None, open_interest=Decimal(900)))
            writer.put_rows(f"minutes/SHFE/{day.isoformat()}/rb/2026-05", "minutes", rows,
                            days=[day.isoformat()])
        writer.commit()
        monday, tuesday = monday.isoformat(), tuesday.isoformat()
        for period, partial, missing, marks in ((1, [tuesday], [6, 120], 558),
                                                 (5, [monday, tuesday], [1, 24], 110)):
            with self.subTest(minutes=period):
                identity = f"coverage-{period}"
                backtest.run(self.directory, identity, self.config(
                    (contract,), begin_day=monday, end_day=tuesday, minutes=period))
                runs = Runs(self.directory)
                data = runs.record(identity)["data"]
                self.assertEqual(data["excluded"], {"bars": len(partial), "days": partial})
                self.assertEqual(data["minute_coverage"], [dict(contract=contract,
                    missing={monday: missing[0], tuesday: missing[1]}, zero_volume_minutes=5,
                    unchecked_days=[])])
                self.assertEqual((runs.table(identity, "equity")["event"] == "mark").sum(), marks)

    def test_minutes_missing_on_a_settled_day_stop_the_run(self):
        writer = Warehouse(self.directory).writer()
        day = DAYS[0].isoformat()
        start = int(datetime.datetime.combine(DAYS[0], datetime.time(9), backtest.BEIJING).timestamp()) * 1000
        writer.put_rows(f"minutes/SHFE/{day}/rb/2026-05", "minutes", [
            dict(exchange="SHFE", product="rb", contract="SHFE/rb/2026-05", day=day,
                 start_ms=start + minute * 60000, partial=False, open=Decimal(3000),
                 high=Decimal(3005), low=Decimal(2995), close=Decimal(3000), volume=Decimal(100),
                 average_price=None, open_interest=Decimal(900)) for minute in range(10)], days=[day])
        writer.commit()
        with self.assertRaises(ValueError):
            backtest.run(self.directory, "gap", self.config(("SHFE/rb/2026-05",), minutes=1))
        (row,) = Runs(self.directory).list()
        self.assertEqual(row["state"], "failed")
        self.assertEqual(row["error"], f"minutes of SHFE/rb/2026-05 are not stored on "
                                       f"{DAYS[1].isoformat()} to {DAYS[-1].isoformat()} (29 days)")

    def test_a_run_requires_calendar_status_for_every_requested_date(self):
        contract = "SHFE/rb/2026-05"
        warehouse = Warehouse(self.directory)
        writer = warehouse.writer()
        calendar = warehouse.read(writer.base, "calendar/SHFE/2026").to_dicts()
        for row in calendar:
            row["day"] = row["day"].isoformat()
        for day in DAYS:
            start = int(datetime.datetime.combine(day, datetime.time(9), backtest.BEIJING).timestamp()) * 1000
            writer.put_rows(f"minutes/SHFE/{day.isoformat()}/rb/2026-05", "minutes", [
                dict(exchange="SHFE", product="rb", contract=contract, day=day.isoformat(),
                     start_ms=start, partial=False, open=Decimal(3000), high=Decimal(3005),
                     low=Decimal(2995), close=Decimal(3000), volume=Decimal(100),
                     average_price=None, open_interest=Decimal(900))], days=[day.isoformat()])
        writer.commit()
        closed = (DAYS[4] + datetime.timedelta(days=1)).isoformat()
        extended = (DAYS[-1] + datetime.timedelta(days=3)).isoformat()
        first_missing = (DAYS[-1] + datetime.timedelta(days=1)).isoformat()
        for damage, end, missing in (("tail", extended, f"{first_missing} to {extended} (3 days)"),
                                     ("closed", DAYS[-1].isoformat(), closed),
                                     ("status", DAYS[-1].isoformat(), closed)):
            with self.subTest(damage=damage):
                writer = warehouse.writer()
                rows = [dict(row, open=None) if damage == "status" and row["day"] == closed else row
                        for row in calendar if damage != "closed" or row["day"] != closed]
                writer.put_rows("calendar/SHFE/2026", "calendar", rows)
                writer.commit()
                for minutes in (0, 1):
                    identity = f"{damage}-{minutes}"
                    with self.assertRaises(ValueError) as failure:
                        backtest.run(self.directory, identity, self.config(
                            (contract,), end_day=end, minutes=minutes))
                    self.assertEqual(str(failure.exception), f"the calendar of SHFE is incomplete on {missing}")
                    self.assertEqual(Runs(self.directory).get(identity)["state"], "failed")

    def test_a_listed_open_day_cannot_disappear_from_a_run(self):
        from warehouse_fixture import daily

        contract, missing = "SHFE/rb/2026-05", DAYS[1].isoformat()
        for damage, message in (("row", "daily bars of {contract} are not stored on {missing}"),
                                ("settlement", "settlement prices of {contract} are missing on {missing}"),
                                ("prices", "minutes of {contract} are not stored on {missing}")):
            with self.subTest(damage=damage):
                writer = Warehouse(self.directory).writer()
                rows = [daily("SHFE/rb/2026-10", day, 3000, 1000) for day in DAYS[:3]]
                for day in DAYS[:3]:
                    if day == DAYS[1] and damage == "row":
                        continue
                    row = daily(contract, day, 3000, 1000, auction=day != DAYS[1] or damage != "prices")
                    if day == DAYS[1] and damage == "settlement":
                        row["settlement"] = None
                    rows.append(row)
                writer.put_rows("daily/SHFE/2026-01", "daily", rows,
                                days=[day.isoformat() for day in DAYS[:3]])
                for day in (DAYS[0], DAYS[2]):
                    start = int(datetime.datetime.combine(day, datetime.time(9), backtest.BEIJING).timestamp()) * 1000
                    writer.put_rows(f"minutes/SHFE/{day.isoformat()}/rb/2026-05", "minutes", [
                        dict(exchange="SHFE", product="rb", contract=contract, day=day.isoformat(),
                             start_ms=start, partial=False, open=Decimal(3000), high=Decimal(3005),
                             low=Decimal(2995), close=Decimal(3000), volume=Decimal(100),
                             average_price=None, open_interest=Decimal(900))], days=[day.isoformat()])
                writer.commit()
                for minutes in (0, 1):
                    identity = f"{damage}-{minutes}"
                    expected = "daily prices of {contract} are missing on {missing}" \
                        if damage == "prices" and not minutes else message
                    with self.assertRaisesRegex(ValueError, expected.format(contract=contract, missing=missing)):
                        backtest.run(self.directory, identity, self.config(
                            (contract,), end_day=DAYS[2].isoformat(), minutes=minutes))
                    self.assertEqual(Runs(self.directory).get(identity)["state"], "failed")

    def test_coverage_respects_listed_life_and_zero_volume_days(self):
        from warehouse_fixture import contract_row, daily

        contract = "SHFE/rb/2026-05"
        writer = Warehouse(self.directory).writer()
        listed = contract_row(contract)
        listed.update(list_date=DAYS[1].isoformat(), delist_date=DAYS[3].isoformat())
        writer.put_rows("contracts/SHFE", "contracts", [listed, contract_row("SHFE/rb/2026-10")])
        rows = []
        for day in DAYS[1:4]:
            row = daily(contract, day, 3000, 1000, auction=day != DAYS[2])
            if day == DAYS[2]:
                row.update(volume=Decimal(0), amount=Decimal(0))
            rows.append(row)
        writer.put_rows("daily/SHFE/2026-01", "daily", rows,
                        days=[day.isoformat() for day in DAYS[1:4]])
        for day in (DAYS[1], DAYS[3]):
            start = int(datetime.datetime.combine(day, datetime.time(9), backtest.BEIJING).timestamp()) * 1000
            writer.put_rows(f"minutes/SHFE/{day.isoformat()}/rb/2026-05", "minutes", [
                dict(exchange="SHFE", product="rb", contract=contract, day=day.isoformat(),
                     start_ms=start, partial=False, open=Decimal(3000), high=Decimal(3005),
                     low=Decimal(2995), close=Decimal(3000), volume=Decimal(100),
                     average_price=None, open_interest=Decimal(900))], days=[day.isoformat()])
        writer.commit()
        for minutes in (0, 1):
            identity = f"listed-{minutes}"
            backtest.run(self.directory, identity, self.config(
                (contract,), end_day=DAYS[4].isoformat(), minutes=minutes))
            runs = Runs(self.directory)
            self.assertEqual(runs.get(identity)["state"], "succeeded")
            self.assertEqual(runs.table(identity, "settlements")["trading_day"].to_list(), DAYS[1:4])
            if minutes:
                self.assertEqual(runs.record(identity)["data"]["excluded"], {"bars": 0, "days": []})

    def test_a_strategy_that_is_not_ready_places_no_more_orders(self):
        import asterion_engine

        class Once(Strategy):
            """Wants three lots at the first bar, is ready only then and at the tenth."""
            def __init__(self):
                super().__init__()
                self.bars = 0

            def on_bar(self, ctx, bar):
                self.bars += 1
                if self.bars == 1:
                    ctx.target(bar.symbol, 3)

            def ready(self, ctx):
                return self.bars in (1, 10)

        config = self.config(("SHFE/rb/2026-05",), max_order_lots=Decimal(1))
        contracts, series, first = backtest.prepare(backtest.open_data(self.directory, config), config)
        contracts[0]["bars"][1] = (*contracts[0]["bars"][1][:6], 10 * backtest.R)
        engine = asterion_engine.Replay(backtest.raw(config.deposit), contracts, series,
                                        (backtest.R, 10 * backtest.R, 10, 10 * backtest.R))
        backtest.replay(engine, Once(), {engine.unit(index): symbol for symbol, index in first.items()},
                        contracts, config)
        # One lot of the first leg; the rest of the target is never placed, and stays void once ready again.
        self.assertEqual([order[4] for order in engine.orders()], [backtest.R])
        self.assertEqual(engine.positions[-1][2], backtest.R)

    def test_a_paused_series_keeps_its_old_month_until_a_new_target_arrives(self):
        import asterion_engine

        class Pause(Strategy):
            def on_bar(self, ctx, bar):
                if bar.time.date() == DAYS[2]:
                    ctx.target(bar.symbol, 3)
                if bar.time.date() == DAYS[22]:
                    ctx.target(bar.symbol, 2)

            def ready(self, ctx):
                return ctx.time.date() <= DAYS[2] or ctx.time.date() >= DAYS[22]

        config = self.config(("SHFE/rb",), max_order_lots=Decimal(1))
        contracts, series, first = backtest.prepare(backtest.open_data(self.directory, config), config)
        contracts[0]["bars"][2] = (*contracts[0]["bars"][2][:6], 10 * backtest.R)
        engine = asterion_engine.Replay(backtest.raw(config.deposit), contracts, series,
                                        (backtest.R, 10 * backtest.R, 10, 10 * backtest.R))
        backtest.replay(engine, Pause(), {engine.unit(index): symbol for symbol, index in first.items()},
                        contracts, config)
        held = [row for row in engine.positions
                if DAYS[3].isoformat() <= row[0] <= DAYS[22].isoformat()]
        self.assertEqual([row[0] for row in held], [day.isoformat() for day in DAYS[3:23]])
        self.assertTrue(all(row[1:4] == (0, backtest.R, 0) for row in held))
        self.assertEqual([fill[5] for fill in engine.fills()
                          if fill[5] <= backtest.close_ns(DAYS[22].isoformat())],
                         [backtest.close_ns(DAYS[3].isoformat())])
        self.assertEqual(engine.positions[-1][1:4], (1, 2 * backtest.R, 0))

    def test_a_series_month_needs_minutes_only_while_it_trades(self):
        import asterion_engine

        # 2026-05 is dominant until its handover on DAYS[16]; 2026-10 joins on DAYS[15].
        writer = Warehouse(self.directory).writer()
        stored = backtest.open_data(self.directory, self.config(("SHFE/rb",))).product("SHFE", "rb")
        for contract, days in (("SHFE/rb/2026-05", DAYS[1:18]), ("SHFE/rb/2026-10", DAYS[15:])):
            for day in days:
                close = Decimal(stored[contract].days[day.isoformat()]["close"])
                start = int(datetime.datetime.combine(day, datetime.time(9), backtest.BEIJING)
                            .timestamp()) * 1000
                writer.put_rows(f"minutes/SHFE/{day.isoformat()}/rb/{contract[-7:]}", "minutes", [
                    dict(exchange="SHFE", product="rb", contract=contract, day=day.isoformat(),
                         start_ms=start + minute * 60000, partial=False, open=close, high=close,
                         low=close, close=close, volume=Decimal(10), average_price=None,
                         open_interest=Decimal(900)) for minute in range(10)], days=[day.isoformat()])
        writer.commit()
        backtest.run(self.directory, "dominant", self.config(("SHFE/rb",), minutes=1))
        (row,) = Runs(self.directory).list()
        self.assertEqual(row["state"], "succeeded", row["error"])

        class Pause(Strategy):
            def on_bar(self, ctx, bar):
                if bar.time.date() == DAYS[2]:
                    ctx.target(bar.symbol, 3)

            def ready(self, ctx):
                return ctx.time.date() <= DAYS[2]

        config = self.config(("SHFE/rb",), minutes=1)
        contracts, series, first = backtest.prepare(backtest.open_data(self.directory, config), config)
        engine = asterion_engine.Replay(backtest.raw(config.deposit), contracts, series,
                                        (backtest.R, 10 * backtest.R, 10, 10 * backtest.R))
        backtest.replay(engine, Pause(), {engine.unit(index): symbol for symbol, index in first.items()},
                        contracts, config)
        with self.assertRaisesRegex(ValueError, f"minutes of SHFE/rb/2026-05 are not stored on "
                                                f"{DAYS[18].isoformat()} to .*, while it is held"):
            backtest.held_without_minutes(engine, contracts)

    def test_minute_bars_are_read_gathered_and_traded(self):
        # Ten minutes 09:00-09:10 Beijing on each of the first ten days, up
        # then down, on top of the days' settlements.
        writer = Warehouse(self.directory).writer()
        bj = datetime.timezone(datetime.timedelta(hours=8))
        for n, day in enumerate(DAYS[:10]):
            start = int(datetime.datetime.combine(day, datetime.time(9), bj).timestamp()) * 1000
            rows = []
            for minute in range(10):
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
                    ("SHFE/rb/2026-05",), end_day=DAYS[9].isoformat(), minutes=minutes))
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
                                     minutes=minutes)
                contracts, series, first = backtest.prepare(backtest.open_data(self.directory, config), config)
                engine = asterion_engine.Replay(backtest.raw(config.deposit), contracts, series,
                                               (10 * backtest.R, 10 * backtest.R, 10, 10 * backtest.R))
                context = backtest.warmup(engine, repository.load(
                    self.repository, config.strategy, config.params),
                    {engine.unit(index): symbol for symbol, index in first.items()}, contracts)
                self.assertEqual(len(context.history("SHFE/rb/2026-05", "close", 1000)), 10 * marks)
                self.assertEqual(engine.orders(), [])
                self.assertEqual(engine.fills(), [])
        with self.assertRaisesRegex(ValueError, "backtest bars are daily or"):
            backtest.minutes_of(Period.PERIOD_WEEK)

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
                                                 (10 * backtest.R, 10 * backtest.R, 10, 10 * backtest.R))

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
                        self.assertEqual(
                            [None if math.isnan(x) else x
                             for x in warmed.history(symbol, field, 1000).tolist()],
                            [None if math.isnan(x) else x
                             for x in traded.history(symbol, field, 1000).tolist()],
                        )
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

        # Margin cap 0.015 scales two 0.50 weights (implied margin 0.10 at 10% rate) down to 0.075 each (2 lots).
        capped = self.config(
            symbols,
            strategy="momentum_rank.MomentumRank",
            params={"lookback": 2, "rebalance": 1, "weight": 0.5},
            max_gross_weight=Decimal(2),
            max_net_weight=Decimal(2),
            max_margin_ratio=Decimal("0.015"),
            min_rebalance_weight=Decimal("0.02"),
        )
        backtest.run(self.directory, "margin-capped", capped)
        capped_fills = Runs(self.directory).table("margin-capped", "fills")
        self.assertTrue(all(q <= Decimal(2) for q in capped_fills["quantity"].to_list()))
        rec = Runs(self.directory).record("margin-capped")
        self.assertEqual(rec["risk"]["max_margin_ratio"], "0.015")
        self.assertEqual(rec["risk"]["min_rebalance_weight"], "0.02")

    def test_volatility_weights_size_after_fees_leave_cents(self):
        # Fees of 3.25 a lot put cents into equity; inverse-volatility weights
        # have eight places. Their product needs rounding, not a failed run.
        symbols = ("SHFE/rb/2026-05", "SHFE/rb/2026-10")
        self.fees(DAYS, "3.25")
        config = self.config(symbols, strategy="momentum_rank.MomentumRank",
                             params={"lookback": 2, "rebalance": 1, "weight": 0.3, "volatility": 3},
                             max_gross_weight=Decimal(1), max_net_weight=Decimal(1))
        backtest.run(self.directory, "volatility", config)
        (row,) = Runs(self.directory).list()
        self.assertEqual(row["state"], "succeeded", row["error"])
        fees = Runs(self.directory).table("volatility", "fills")["fee"].to_list()
        self.assertTrue(any(fee % 1 for fee in fees))

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
        self.assertEqual((row["state"], row["error"]),
                         ("failed", "the contract table has no unique listed life of SHFE/rb/2027-01"))

    def fees(self, days, fee, today_fee=None, margin="0.1", short_margin=None):
        """The exchange's fees of SHFE/rb on `days` in a new dataset version."""
        writer = Warehouse(self.directory).writer()
        for day in days:
            writer.put_rows(f"settlement/SHFE/{day.isoformat()}", "settlement",
                            [settlement(contract, day, fee,
                                        fee if today_fee is None else today_fee,
                                        margin, short_margin)
                             for contract in ("SHFE/rb/2026-05", "SHFE/rb/2026-10")])
        writer.commit()

    def test_fees_follow_each_day_and_a_missing_one_stops_the_run(self):
        self.fees(DAYS[15:], "5", margin="0.1", short_margin="0.15")
        backtest.run(self.directory, "dated", self.config(("SHFE/rb/2026-05",)))
        fills = Runs(self.directory).table("dated", "fills")
        self.assertEqual({fee / quantity for fee, quantity in zip(fills["fee"], fills["quantity"])},
                         {Decimal(3), Decimal(5)})
        positions = Runs(self.directory).table("dated", "positions")
        short_rows = positions.filter(positions["short"] > 0)
        self.assertTrue(len(short_rows) > 0)
        for row in short_rows.iter_rows(named=True):
            self.assertEqual(row["margin"], row["short"] * row["settlement_price"] * 10 * Decimal("0.15"))
        # Daily strategies never close today's position on the same day, so a missing
        # close-today fee does not block overnight runs, while a missing open fee does.
        self.fees(DAYS[3:4], "3", today_fee="")
        backtest.run(self.directory, "overnight_ok", self.config(("SHFE/rb/2026-05",)))
        self.fees(DAYS[3:4], "")
        with self.assertRaisesRegex(ValueError, f"missing fees of SHFE/rb/2026-05 on {DAYS[3].isoformat()}"):
            backtest.run(self.directory, "missing", self.config(("SHFE/rb/2026-05",)))

    def test_targets_the_limits_refuse_are_recorded_with_their_reason(self):
        backtest.run(self.directory, "refused", self.config(
            ("SHFE/rb/2026-05",), params={"fast": 2, "slow": 4, "lots": 5}, max_gross_lots=Decimal(2),
            max_contract_lots=Decimal(2)))
        rows = Runs(self.directory).record("refused")["data"]["rejections"]
        # Long, then short: each target is refused at every bar it stands.
        self.assertEqual([(row["contract"], row["lots"], row["reason"]) for row in rows],
                         [("SHFE/rb/2026-05", "5", "gross_quantity"),
                          ("SHFE/rb/2026-05", "-5", "gross_quantity")])
        self.assertTrue(all(row["count"] > 1 and row["first_ms"] < row["last_ms"] for row in rows))

    def test_a_missing_limit_is_named(self):
        run = self.sweep_request("named", [2], [4], 1).start_sweep.base
        run.limits.max_order_lots.text = ""
        with self.assertRaisesRegex(ValueError, "max_order_lots must be a number, not ''"):
            backtest.config_of(AdmittedRun(backtest=run))

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
            listed = (await service.reply(Request(id=1,
                                                  list_stored_contracts={}))).list_stored_contracts
            stored = {item.contract: item for item in listed.contracts}
            self.assertEqual(sorted(stored), ["SHFE/rb/2026-05", "SHFE/rb/2026-10"])
            may = stored["SHFE/rb/2026-05"]
            self.assertEqual((may.name, may.daily.first_day, may.daily.days),
                             ("SHFE/rb/2026-05", DAYS[0].isoformat(), 30))
            self.assertEqual((may.minutes.first_day, may.minutes.days), (DAYS[1].isoformat(), 1))
            self.assertEqual(stored["SHFE/rb/2026-10"].minutes.days, 0)
            query = Request(id=1, get_chart_bars={
                "contract": "SHFE/rb/2026-05", "period": Period.PERIOD_MINUTES_5})
            five = (await service.reply(query)).get_chart_bars.bars
            # Both minutes began within 21:00-21:05; the bar ends then.
            self.assertEqual([(item.end_ms, item.open.text, item.close.text, item.volume.text)
                              for item in five], [(1767618300000, "3000", "3001", "8")])
            self.assertTrue(five[0].partial)
            query.get_chart_bars.period = Period.PERIOD_WEEK
            weeks = (await service.reply(query)).get_chart_bars.bars
            self.assertEqual(len(weeks), 6)
            self.assertEqual(weeks[0].trading_day, DAYS[4].isoformat())
            self.assertFalse(weeks[0].HasField("macd_diff"))
            query.get_chart_bars.period = Period.PERIOD_DAY
            original = (await service.reply(query)).get_chart_bars
            days = original.bars
            # Thirty days: all within MACD's warm-up.
            self.assertEqual(len(days), 30)
            self.assertFalse(any(item.HasField("macd_diff") for item in days))
            self.publish_incomplete()
            pinned = Request(id=1, list_stored_contracts={"dataset_version": listed.dataset_version})
            self.assertEqual((await service.reply(pinned)).list_stored_contracts, listed)
            latest = (await service.reply(Request(id=1, list_stored_contracts={}))).list_stored_contracts
            self.assertNotEqual(latest.dataset_version, listed.dataset_version)
            query.get_chart_bars.dataset_version = original.dataset_version
            self.assertEqual((await service.reply(query)).get_chart_bars, original)
            query.get_chart_bars.dataset_version = "a" * 64
            self.assertEqual((await service.reply(query)).error.code, INVALID)
            query.get_chart_bars.dataset_version = ""
            current = (await service.reply(query)).get_chart_bars
            self.assertNotEqual(current.dataset_version, original.dataset_version)
            self.assertLess(len(current.bars), len(original.bars))
        asyncio.run(scenario())

    def test_a_deleted_run_takes_its_results_and_code_copy_and_start_prunes_strays(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        # Code copies; the fixture's dataset also lives under sources/.
        def copies():
            return sorted(path.name.rsplit("-", 1)[0]
                          for path in (self.directory / "sources").glob("run-*"))

        async def scenario():
            service = Service("lab", self.directory, Log())
            for identity in ("keep", "drop"):
                request = Request(id=1)
                run = request.start_backtest
                run.id, run.repository, run.strategy = identity, str(self.repository), "ma_cross.MaCross"
                run.symbols.append("SHFE/rb/2026-05")
                self.fill(run)
                reply = await service.reply(request)
                self.assertEqual(reply.WhichOneof("result"), "start_backtest", reply.error)
            deadline = time.monotonic() + 30
            while service.backtests and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            self.assertEqual(copies(), ["run-drop", "run-keep"])
            reply = await service.reply(Request(id=1, delete_run={"id": "drop"}))
            self.assertEqual(reply.WhichOneof("result"), "delete_run", reply.error)
            listed = await service.reply(Request(id=1, list_runs={}))
            self.assertEqual([row.id for row in listed.list_runs.runs], ["keep"])
            self.assertFalse((self.directory / "runs" / "drop").exists())
            self.assertEqual(copies(), ["run-keep"])
            missing = await service.reply(Request(id=1, delete_run={"id": "drop"}))
            self.assertEqual(missing.error.code, INVALID)

        asyncio.run(scenario())
        # A copy no record refers to goes when Lab starts; the kept run's stays.
        (self.directory / "sources" / "run-gone-abcd1234").mkdir()
        (self.directory / "factors").mkdir(exist_ok=True)
        (self.directory / "factors" / "ic.binpb").write_bytes(b"")
        (self.directory / "sources" / "factor-ic-abcd1234").mkdir()
        service = Service("lab", self.directory, Log())
        self.assertEqual(copies(), ["run-keep"])
        self.assertTrue((self.directory / "sources" / "factor-ic-abcd1234").exists())
        # A factor evaluation goes with its code copy.
        reply = asyncio.run(service.reply(Request(id=1, delete_factor_result={"id": "ic"})))
        self.assertEqual(reply.WhichOneof("result"), "delete_factor_result", reply.error)
        self.assertFalse((self.directory / "factors" / "ic.binpb").exists())
        self.assertFalse((self.directory / "sources" / "factor-ic-abcd1234").exists())

    def test_the_service_runs_a_backtest_in_a_child_process(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            request = Request(id=1)
            run = request.start_backtest
            run.id, run.repository, run.strategy = "child", str(self.repository), "ma_cross.MaCross"
            run.params["fast"].integer = 2
            run.params["slow"].integer = 4
            run.symbols.append("SHFE/rb/2026-05")
            self.fill(run)
            replies = await asyncio.gather(service.reply(request), service.reply(request))
            self.assertEqual(sorted(reply.WhichOneof("result") for reply in replies),
                             ["error", "start_backtest"])
            self.assertEqual(next(reply for reply in replies if reply.HasField("error")).error.code,
                             INVALID)
            # Accepted means recorded: the run can be read at once.
            query = Request(id=1, get_run={"id": "child"})
            self.assertEqual((await service.reply(query)).WhichOneof("result"), "get_run")
            self.assertEqual((await service.reply(request)).error.code, INVALID)
            deadline = time.monotonic() + 30
            while "child" in service.backtests and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            listed = await service.reply(Request(id=1, list_runs={}))
            (row,) = listed.list_runs.runs
            self.assertEqual(row.state, SUCCEEDED, row.error)
            self.assertIn("total_return", row.metrics)
            # The list says what each run ran, so records compare without opening them.
            self.assertEqual(list(row.symbols), ["SHFE/rb/2026-05"])
            self.assertEqual((row.first_day, row.last_day), (run.first_day, run.last_day))
            self.assertEqual(row.period, run.period)
            self.assertEqual(row.params["slow"].integer, 4)
            detail = (await service.reply(Request(id=1, get_run={"id": "child"}))).get_run
            self.assertEqual(row.metrics["fills"], len(detail.fills))
            # A name the user gives a run lists with it; an empty one removes it.
            renamed = await service.reply(Request(id=1, rename_run={"id": "child", "label": " 快慢均线 "}))
            self.assertEqual(renamed.WhichOneof("result"), "rename_run", renamed.error)
            (row,) = (await service.reply(Request(id=1, list_runs={}))).list_runs.runs
            self.assertEqual(row.label, "快慢均线")
            await service.reply(Request(id=1, rename_run={"id": "child", "label": ""}))
            (row,) = (await service.reply(Request(id=1, list_runs={}))).list_runs.runs
            self.assertEqual(row.label, "")
            missing = await service.reply(Request(id=1, rename_run={"id": "nothing", "label": "x"}))
            self.assertEqual(missing.error.code, INVALID)
            self.assertEqual(detail.summary.state, SUCCEEDED)
            report_reply = await service.reply(Request(id=1,
                                                       get_run_report={"id": "child"}))
            self.assertEqual(report_reply.WhichOneof("result"), "get_run_report", report_reply.error)
            report = report_reply.get_run_report
            self.assertTrue(report.quantstats_version)
            self.assertLessEqual({"tail_ratio", "ulcer_index", "value_at_risk",
                                  "conditional_value_at_risk"}, set(report.figures))
            missing = await service.reply(Request(id=1,
                                                  get_run_report={"id": "nothing"}))
            self.assertEqual(missing.WhichOneof("result"), "error")
            self.assertIn('"commit"', detail.record)
            self.assertEqual(len(detail.days), len(DAYS))
            self.assertTrue(detail.fills and detail.equity[-1].settlement)
            self.assertEqual(Decimal(detail.days[-1].equity.text), Decimal(detail.equity[-1].equity.text))
            run.id, run.repository = "elsewhere", str(Path(self.temporary.name) / "missing")
            self.assertEqual((await service.reply(request)).error.code, INVALID)

        asyncio.run(scenario())

    def test_completed_runs_can_be_read_concurrently_for_comparison(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        for identity, fast in (("first", 2), ("second", 3)):
            backtest.run(self.directory, identity, self.config(
                ("SHFE/rb/2026-05",), params={"fast": fast, "slow": 4}))

        async def scenario():
            service = Service("lab", self.directory, Log())
            identities = ("first", "second")
            try:
                for _ in range(1000):
                    replies = await asyncio.gather(*(service.reply(
                        Request(id=n + 1, get_run={"id": identity}))
                        for n, identity in enumerate(identities)))
                    for identity, reply in zip(identities, replies):
                        self.assertEqual(reply.WhichOneof("result"), "get_run", reply.error)
                        self.assertEqual(reply.get_run.summary.id, identity)
                        self.assertEqual(json.loads(reply.get_run.record)["id"], identity)
            finally:
                service.reads.shutdown()
                service.runs.index.close()

        asyncio.run(scenario())

    def fill(self, run):
        """Daily bars of every day, with the test's deposit and limits."""
        run.first_day, run.last_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
        run.period = Period.PERIOD_DAY
        run.deposit.text = "1000000"
        run.limits.max_order_lots.text = run.limits.max_gross_lots.text = "10"
        run.limits.max_contract_lots.text = "10"
        run.limits.max_working_orders = 10

    def sweep_request(self, identity, fast, slow, processes):
        request = Request(id=1)
        sweep = request.start_sweep
        sweep.id, sweep.processes = identity, processes
        run = sweep.base
        run.repository, run.strategy = str(self.repository), "ma_cross.MaCross"
        run.symbols.append("SHFE/rb/2026-05")
        self.fill(run)
        run.periods_per_year, run.zero_risk_free_rate, run.holdout_day = 252, True, DAYS[20].isoformat()
        for name, values in (("fast", fast), ("slow", slow)):
            item = sweep.values.add(name=name)
            for value in values:
                item.values.add(integer=value)
        return request

    @asynccontextmanager
    async def service(self):
        service = Service("lab", self.directory, type("Log", (), {"write": lambda *a, **k: None})())
        try:
            yield service
        finally:
            service.waiting.clear()
            service.rolling.clear()
            processes = list(service.backtests.values())
            for process in processes:
                if process.returncode is None:
                    process.terminate()
            await asyncio.gather(*(process.wait() for process in processes))
            watchers = [task for task in asyncio.all_tasks()
                        if task.get_coro().__qualname__ == "Service.watch"]
            await asyncio.gather(*watchers, return_exceptions=True)
            service.reads.shutdown()
            service.runs.index.close()

    async def idle(self, service):
        async with asyncio.timeout(30):
            while service.backtests or service.waiting or service.rolling:
                await asyncio.sleep(0.01)

    def child_before(self, code):
        child = worker.command

        def command(module):
            result = child(module)
            start, call = result[-1].rsplit("; main(", 1)
            result[-1] = start + "\n" + code + "\nmain(" + call
            return result
        return patch.object(worker, "command", command)

    def test_a_candidate_that_cannot_spawn_is_failed_and_the_sweep_continues(self):
        async def scenario():
            spawn, calls = asyncio.create_subprocess_exec, 0

            async def fail_first(*args, **kwargs):
                nonlocal calls
                calls += 1
                if calls == 1:
                    raise OSError("fixture process could not start")
                return await spawn(*args, **kwargs)

            async with self.service() as service:
                version = Dataset(self.directory).current().identity
                with patch("asyncio.create_subprocess_exec", fail_first):
                    reply = await service.reply(self.sweep_request("spawn", [2, 3], [4], 1))
                    self.assertEqual(reply.WhichOneof("result"), "start_sweep", reply.error)
                    await self.idle(service)
                sweep = (await service.reply(Request(id=1, get_sweep={"id": "spawn"}))).get_sweep
                self.assertEqual([run.state for run in sweep.runs],
                                 [RunState.RUN_STATE_FAILED, SUCCEEDED])
                self.assertFalse(sweep.active)
                detail = (await service.reply(Request(id=1, get_run={"id": "spawn-0"}))).get_run
                self.assertIn("fixture process could not start", detail.summary.error)
                self.assertEqual(detail.summary.pinned.dataset_version, version)
                self.assertEqual(json.loads(detail.record)["params"], {"fast": 2, "slow": 4})
        asyncio.run(scenario())

    def test_a_sweep_stays_active_while_its_last_candidate_starts(self):
        async def scenario():
            spawn, calls = asyncio.create_subprocess_exec, 0
            starting, release = asyncio.Event(), asyncio.Event()

            async def delayed_spawn(*args, **kwargs):
                nonlocal calls
                calls += 1
                if calls == 2:
                    starting.set()
                    await release.wait()
                return await spawn(*args, **kwargs)

            async with self.service() as service:
                with patch("asyncio.create_subprocess_exec", delayed_spawn):
                    response = await service.reply(self.sweep_request("handoff", [2, 3], [4], 1))
                    self.assertEqual(response.WhichOneof("result"), "start_sweep", response.error)
                    try:
                        await asyncio.wait_for(starting.wait(), 30)
                        reply = (await service.reply(Request(id=1, get_sweep={"id": "handoff"}))).get_sweep
                    finally:
                        release.set()
                        await service.pump()
                    await self.idle(service)
                self.assertTrue(reply.active)
                self.assertEqual([run.state for run in reply.runs],
                                 [SUCCEEDED, RunState.RUN_STATE_UNSPECIFIED])
                finished = (await service.reply(Request(id=1, get_sweep={"id": "handoff"}))).get_sweep
                self.assertEqual([run.state for run in finished.runs], [SUCCEEDED, SUCCEEDED])
                self.assertFalse(finished.active)
        asyncio.run(scenario())

    def test_a_child_exit_before_acknowledgement_keeps_its_record_after_restart(self):
        async def scenario(registered):
            identity = "registered" if registered else "json-only"
            code = f"""
import sqlite3
from asterion_lab.runs import Runs
original_start = Runs.start
def interrupted_start(self, identity, plan):
    if not {registered!r}:
        self.index.set_authorizer(lambda operation, table, *args:
            sqlite3.SQLITE_DENY if operation == sqlite3.SQLITE_INSERT and table == 'runs'
            else sqlite3.SQLITE_OK)
    try:
        original_start(self, identity, plan)
    finally:
        (self.root.parent / 'before-exit.json').write_text(
            (self.root / identity / 'run.json').read_text())
    raise RuntimeError('fixture exited before acknowledgement')
Runs.start = interrupted_start
"""
            async with self.service() as service:
                request = Request(id=1)
                request.start_backtest.CopyFrom(self.sweep_request("base", [2], [4], 1).start_sweep.base)
                request.start_backtest.id = identity
                version = Dataset(self.directory).current().identity
                with self.child_before(code):
                    response = await asyncio.wait_for(service.reply(request), 30)
                self.assertEqual(response.WhichOneof("result"), "error")
                await self.idle(service)
                reply = await service.reply(Request(id=1, get_run={"id": identity}))
                self.assertEqual(reply.WhichOneof("result"), "get_run", reply.error)
                detail = reply.get_run
                self.assertEqual(detail.summary.state, RunState.RUN_STATE_FAILED)
                self.assertIn("acknowledgement" if registered else "not authorized", detail.summary.error)
                self.assertEqual(detail.summary.pinned.dataset_version, version)
                original = json.loads((self.directory / "before-exit.json").read_text())
                self.assertEqual(json.loads(detail.record), original)
            async with self.service() as restarted:
                again = (await restarted.reply(Request(id=1, get_run={"id": identity}))).get_run
                self.assertEqual(again, detail)

        for registered in (False, True):
            with self.subTest(registered=registered):
                asyncio.run(scenario(registered))

    def test_rolling_failure_before_selection_keeps_candidates_and_fixed_inputs(self):
        async def scenario(spawn_fails):
            identity = "spawn-roll" if spawn_fails else "short"
            spawn = asyncio.create_subprocess_exec

            async def start(*args, **kwargs):
                if spawn_fails and "asterion_lab.rolling" in args[args.index("-c") + 1]:
                    raise OSError("fixture rolling process could not start")
                return await spawn(*args, **kwargs)

            async with self.service() as service:
                request = self.sweep_request(identity, [2, 3], [4], 2)
                request.start_sweep.base.holdout_day = ""
                request.start_sweep.walk_forward.training_days = len(DAYS)
                request.start_sweep.walk_forward.validation_days = 3
                version = Dataset(self.directory).current().identity
                with patch("asyncio.create_subprocess_exec", start):
                    self.assertEqual((await service.reply(request)).WhichOneof("result"), "start_sweep")
                    await self.idle(service)
                sweep = (await service.reply(Request(id=1, get_sweep={"id": identity}))).get_sweep
                self.assertEqual([run.state for run in sweep.runs], [SUCCEEDED, SUCCEEDED])
                self.assertFalse(sweep.active)
                self.assertEqual(sweep.rolling.state, RunState.RUN_STATE_FAILED)
                self.assertIn("fixture rolling process" if spawn_fails else "validation days after them",
                              sweep.rolling.error)
                reply = await service.reply(Request(id=1, get_run={"id": f"{identity}-rolling"}))
                self.assertEqual(reply.WhichOneof("result"), "get_run", reply.error)
                record = json.loads(reply.get_run.record)
                self.assertEqual(record["params"]["candidates"],
                                 [{"fast": 2, "slow": 4}, {"fast": 3, "slow": 4}])
                self.assertEqual(record["data"]["dataset_version"], version)
                self.assertEqual((record["data"]["begin_day"], record["data"]["end_day"]),
                                 (DAYS[0].isoformat(), DAYS[-1].isoformat()))
                self.assertEqual(record["statistics"]["rolling"], {
                    "sweep": identity, "candidates": [f"{identity}-0", f"{identity}-1"],
                    "training_days": len(DAYS), "validation_days": 3, "rounds": []})
        for spawn_fails in (False, True):
            with self.subTest(spawn_fails=spawn_fails):
                asyncio.run(scenario(spawn_fails))

    def test_cancelling_the_start_request_keeps_the_child_and_unblocks_the_sweep(self):
        async def scenario():
            entered, release = self.directory / "entered", self.directory / "release"
            code = f"""
import builtins
import time
from pathlib import Path
original_print = builtins.print
def delayed_print(*args, **kwargs):
    if args == ('started',):
        Path({str(entered)!r}).touch()
        while not Path({str(release)!r}).exists():
            time.sleep(0.01)
    return original_print(*args, **kwargs)
builtins.print = delayed_print
"""
            async with self.service() as service:
                service.processes = 1
                request = Request(id=1)
                request.start_backtest.CopyFrom(self.sweep_request("base", [2], [4], 1).start_sweep.base)
                request.start_backtest.id = "detached"
                with self.child_before(code):
                    pending = asyncio.create_task(service.reply(request))
                    async with asyncio.timeout(30):
                        while not entered.exists():
                            await asyncio.sleep(0.01)
                pending.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await pending
                reply = await service.reply(self.sweep_request("next", [2, 3], [4], 1))
                self.assertEqual(reply.WhichOneof("result"), "start_sweep", reply.error)
                self.assertEqual(len(service.waiting), 2)
                watcher = next(task for task in asyncio.all_tasks()
                               if task.get_coro().__qualname__ == "Service.watch")
                release.touch()
                await asyncio.wait_for(asyncio.shield(watcher), 30)
                await self.idle(service)
                detached = (await service.reply(Request(id=1, get_run={"id": "detached"}))).get_run
                self.assertEqual(detached.summary.state, SUCCEEDED, detached.summary.error)
                sweep = (await service.reply(Request(id=1, get_sweep={"id": "next"}))).get_sweep
                self.assertEqual([run.state for run in sweep.runs], [SUCCEEDED, SUCCEEDED])
                self.assertFalse(sweep.active)
        asyncio.run(scenario())

    def test_a_sweep_runs_every_combination_in_parallel_and_selects_by_development_sharpe(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            request = self.sweep_request("grid", [2, 3], [4, 5], 2)
            fixed_version = Dataset(self.directory).current().identity
            self.assertEqual((await service.reply(request)).WhichOneof("result"), "start_sweep")
            # The queued candidates must keep the inputs accepted for the whole sweep.
            self.assertNotEqual(self.publish_incomplete(), fixed_version)
            self.assertLessEqual(len(service.backtests), 2)
            self.assertEqual((await service.reply(request)).error.code, INVALID)
            query = Request(id=1, get_sweep={"id": "grid"})
            self.assertTrue((await service.reply(query)).get_sweep.active)
            deadline = time.monotonic() + 60
            while (service.backtests or service.waiting) and time.monotonic() < deadline:
                self.assertLessEqual(len(service.backtests), 2)
                await asyncio.sleep(0.1)
            reply = (await service.reply(query)).get_sweep
            self.assertFalse(reply.active)
            self.assertEqual([run.id for run in reply.runs], ["grid-0", "grid-1", "grid-2", "grid-3"])
            self.assertEqual([(run.params["fast"].integer, run.params["slow"].integer) for run in reply.runs],
                             [(2, 4), (2, 5), (3, 4), (3, 5)])
            self.assertEqual({run.state for run in reply.runs}, {SUCCEEDED})
            self.assertEqual({run.pinned.dataset_version for run in reply.runs}, {fixed_version})
            sharpe = {run.id: run.metrics["development_sharpe"] for run in reply.runs
                      if "development_sharpe" in run.metrics}
            self.assertEqual(reply.selected_id, max(sharpe, key=sharpe.get) if sharpe else "")

        asyncio.run(scenario())

    def test_cancelling_a_sweep_stops_its_running_rolling_validation(self):
        (self.repository / "strategies/slow.py").write_text(
            "import time\nfrom strategies.ma_cross import MaCross\n\n\n"
            "class Slow(MaCross):\n    def on_bar(self, ctx, bar):\n"
            "        time.sleep(0.05)\n        super().on_bar(ctx, bar)\n")

        async def scenario():
            service = Service("lab", self.directory, type("Log", (), {"write": lambda *a, **k: None})())
            request = self.sweep_request("slow", [2, 3], [4], 2)
            request.start_sweep.base.strategy, request.start_sweep.base.holdout_day = "slow.Slow", ""
            request.start_sweep.walk_forward.training_days, request.start_sweep.walk_forward.validation_days = 20, 3
            self.assertEqual((await service.reply(request)).WhichOneof("result"), "start_sweep")
            deadline = time.monotonic() + 60
            while "slow-rolling" not in service.backtests and time.monotonic() < deadline:
                await asyncio.sleep(0.05)
            self.assertIn("slow-rolling", service.backtests)
            cancel = Request(id=1, cancel_sweep={"id": "slow"})
            self.assertEqual((await service.reply(cancel)).WhichOneof("result"), "cancel_sweep")
            while service.backtests and time.monotonic() < deadline:
                await asyncio.sleep(0.05)
            # Stopped, possibly before it recorded anything; never run to the end.
            runs = {row["id"]: row["state"] for row in Runs(self.directory).list()}
            self.assertNotEqual(runs.get("slow-rolling"), "succeeded")

        asyncio.run(scenario())

    def test_a_sweep_too_large_is_refused_before_its_combinations_exist(self):
        async def scenario():
            service = Service("lab", self.directory, type("Log", (), {"write": lambda *a, **k: None})())
            request = self.sweep_request("huge", list(range(1000)), list(range(1000)), 1)
            for name in ("lots", "extra"):
                item = request.start_sweep.values.add(name=name)
                for value in range(1000):
                    item.values.add(integer=value)
            reply = await asyncio.wait_for(service.reply(request), 5)
            self.assertIn("at most 10000 combinations", reply.error.message)

        asyncio.run(scenario())

    def test_rolling_validation_follows_each_rounds_choice(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            request = self.sweep_request("walk", [2, 3], [4], 2)
            request.start_sweep.base.holdout_day = ""
            request.start_sweep.walk_forward.training_days, request.start_sweep.walk_forward.validation_days = 20, 3
            fixed_version = Dataset(self.directory).current().identity
            self.assertEqual((await service.reply(request)).WhichOneof("result"), "start_sweep")
            self.assertNotEqual(self.publish_incomplete(), fixed_version)
            deadline = time.monotonic() + 60
            while (service.backtests or service.waiting or service.rolling) and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            reply = (await service.reply(Request(id=1, get_sweep={"id": "walk"}))).get_sweep
            self.assertEqual(reply.rolling.state, SUCCEEDED, reply.rolling.error)
            self.assertEqual(reply.rolling.pinned.dataset_version, fixed_version)
            self.assertEqual({run.pinned.dataset_version for run in reply.runs}, {fixed_version})
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
        request.start_sweep.walk_forward.training_days, request.start_sweep.walk_forward.validation_days = 20, 3
        no_sharpe = self.sweep_request("no-sharpe", [2, 3], [4], 2)
        no_sharpe.start_sweep.base.holdout_day = ""
        no_sharpe.start_sweep.base.zero_risk_free_rate = False
        no_sharpe.start_sweep.walk_forward.training_days, no_sharpe.start_sweep.walk_forward.validation_days = 20, 3

        async def refused():
            service = Service("lab", self.directory, type("Log", (), {"write": lambda *a, **k: None})())
            self.assertEqual((await service.reply(request)).error.code, INVALID)
            self.assertEqual((await service.reply(no_sharpe)).error.code, INVALID)

        asyncio.run(refused())
        with self.assertRaises(ValueError):
            Runs(self.directory).sweep("refused")

    def test_cross_sectional_45_product_composite_weight_backtest_rolling_and_package(self):
        from warehouse_fixture import closes, contract_row

        # 45 two-letter product codes (aa..bs): 45 dominant series = 90 underlying contracts > 20.
        products = [chr(ord("a") + i // 26) + chr(ord("a") + i % 26) for i in range(45)]
        writer = Warehouse(self.directory).writer()
        contracts, jan, feb = [], [], []
        settlements_by_day = {day: [] for day in DAYS}
        ticks = []
        for idx, product in enumerate(products):
            m1, m2 = f"SHFE/{product}/2026-05", f"SHFE/{product}/2026-10"
            contracts.extend([contract_row(m1), contract_row(m2)])
            ticks.extend([price_tick(m1), price_tick(m2)])
            for day_i, (day, base) in enumerate(zip(DAYS, closes(len(DAYS)))):
                drift = (idx - 22) * day_i * 2
                price = max(500, base + drift)
                target = jan if day.isoformat() < "2026-02" else feb
                target.append(daily(m1, day, price, 1500 + idx * 10 if day_i < 15 else 100))
                target.append(daily(m2, day, price + 20, 500 if day_i < 15 else 2500 + idx * 10))
                settlements_by_day[day].extend([settlement(m1, day), settlement(m2, day)])
        writer.put_rows("contracts/SHFE", "contracts", contracts)
        writer.put_rows("price_ticks/SHFE", "price_ticks", ticks)
        for day, s_rows in settlements_by_day.items():
            writer.put_rows(f"settlement/SHFE/{day.isoformat()}", "settlement", s_rows)
        writer.add_days("SHFE", "2026-01", jan,
                        [day.isoformat() for day in DAYS if day.isoformat() < "2026-02"])
        writer.add_days("SHFE", "2026-02", feb,
                        [day.isoformat() for day in DAYS if day.isoformat() >= "2026-02"])
        writer.commit()

        symbols = [f"SHFE/{p}" for p in products]

        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            # 1) 45-product multi-sub-strategy CompositePortfolio backtest + package build
            bt_req = Request(id=1)
            bt = bt_req.start_backtest
            bt.id = "comp-45"
            bt.repository = str(EXAMPLES)
            bt.strategy = "composite.CompositePortfolio"
            bt.params["trend_lookback"].integer = 6
            bt.params["carry_lookback"].integer = 4
            bt.params["count"].integer = 4
            bt.symbols.extend(symbols)
            bt.first_day, bt.last_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
            bt.period = Period.PERIOD_DAY
            bt.deposit.text = "10000000"
            bt.slippage_ticks = 1
            bt.limits.max_order_lots.text = "50"
            bt.limits.max_contract_lots.text = "100"
            bt.limits.max_gross_lots.text = "1000"
            bt.limits.max_working_orders = 64
            bt.max_gross_weight.text = "0.8"
            bt.max_net_weight.text = "0.8"
            bt.periods_per_year = 252
            bt.zero_risk_free_rate = True
            self.assertEqual((await service.reply(bt_req)).WhichOneof("result"), "start_backtest")
            deadline = time.monotonic() + 60
            while service.backtests and time.monotonic() < deadline:
                await asyncio.sleep(0.05)
            comp_summary = (await service.reply(Request(id=2, get_run_summary={"id": "comp-45"}))).get_run_summary
            self.assertEqual(comp_summary.state, SUCCEEDED, comp_summary.error)
            self.assertEqual(len(comp_summary.symbols), 45)
            pkg = (await service.reply(Request(id=3, build_package={"run_id": "comp-45"}))).build_package
            self.assertEqual(pkg.manifest.entry, "strategies.composite:CompositePortfolio")
            self.assertEqual(len(pkg.manifest.research.symbols), 45)

            # 2) 45-product Walk-Forward rolling validation of ctx.weights strategy
            sw_req = Request(id=4)
            sw = sw_req.start_sweep
            sw.id = "walk-weights-45"
            sw.base.CopyFrom(bt)
            sw.base.id = ""
            sw.base.strategy = "composite.MultiFactorRank"
            sw.base.params.clear()
            sw.base.params["fast"].integer = 2
            sw.base.params["rebalance"].integer = 2
            sw.base.params["count"].integer = 4
            sw.base.params["gross_weight"].number = 0.6
            param_vals = sw.values.add(name="slow")
            param_vals.values.add(integer=5)
            param_vals.values.add(integer=8)
            sw.walk_forward.training_days = 20
            sw.walk_forward.validation_days = 5
            sw.processes = 2
            self.assertEqual((await service.reply(sw_req)).WhichOneof("result"), "start_sweep")
            deadline = time.monotonic() + 90
            while (service.backtests or service.waiting or service.rolling) and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            sweep_reply = (await service.reply(Request(id=5, get_sweep={"id": "walk-weights-45"}))).get_sweep
            self.assertEqual(sweep_reply.rolling.state, SUCCEEDED, sweep_reply.rolling.error)
            self.assertIn("out_of_sample_sharpe", sweep_reply.rolling.metrics)

        asyncio.run(scenario())

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
            cancel = Request(id=1, cancel_sweep={"id": "stopped"})
            self.assertEqual((await service.reply(cancel)).WhichOneof("result"), "cancel_sweep")
            self.assertFalse(service.waiting)
            deadline = time.monotonic() + 30
            while service.backtests and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            reply = (await service.reply(Request(id=1, get_sweep={"id": "stopped"}))).get_sweep
            self.assertEqual(len(reply.runs), 9)
            self.assertIn(reply.runs[0].state, (RunState.RUN_STATE_INTERRUPTED, SUCCEEDED))
            self.assertEqual({run.state for run in reply.runs[1:]}, {RunState.RUN_STATE_UNSPECIFIED})
            # Reusing a never-started candidate would attach unrelated results
            # to the cancelled sweep, including after a service restart.
            reused = Request(id=1)
            reused.start_backtest.CopyFrom(self.sweep_request("other", [2], [4], 1).start_sweep.base)
            reused.start_backtest.id = "stopped-1"
            for owner in (service, Service("lab", self.directory, Log())):
                response = await owner.reply(reused)
                self.assertEqual(response.error.code, INVALID)
                self.assertFalse(owner.backtests)
            self.assertFalse((self.directory / "runs/stopped-1").exists())

        asyncio.run(scenario())
        runs = Runs(self.directory)
        runs.index.execute("update runs set state = 'running' where id = 'stopped-0'")
        Service("lab", self.directory, type("Log", (), {"write": lambda *a, **k: None})())
        self.assertEqual(next(row for row in Runs(self.directory).list() if row["id"] == "stopped-0")["state"],
                         "interrupted")

    def test_minute_acceptance_script_audits_cached_versions(self):
        import asterion_engine
        import importlib.util

        contract = "SHFE/rb/2026-05"
        tuesday, wednesday = DAYS[1], DAYS[2]
        midnight = datetime.datetime.combine(tuesday, datetime.time(), backtest.BEIJING)
        sessions = [(midnight - datetime.timedelta(days=1), 1260, 1380),
                    (midnight, 540, 615), (midnight, 630, 690), (midnight, 810, 900)]
        minutes = [dict(exchange="SHFE", product="rb", contract=contract, day=tuesday.isoformat(),
                        start_ms=int(base.timestamp()) * 1000 + slot * 60_000, partial=False,
                        open=Decimal(3000), high=Decimal(3005), low=Decimal(2995), close=Decimal(3000),
                        volume=Decimal(1), average_price=None, open_interest=Decimal(900))
                   for base, begin, end in sessions for slot in range(begin, end)]
        writer = Warehouse(self.directory).writer()
        writer.put_rows(f"minutes/SHFE/{tuesday.isoformat()}/rb/2026-05", "minutes", minutes,
                        days=[tuesday.isoformat()])
        pre_listed = datetime.date(2024, 12, 31)
        writer.add_days("SHFE", "2024-12", [daily(contract, pre_listed, 3000, 1)], [pre_listed.isoformat()])
        writer.add_days("SHFE", "2026-01", [
            {**daily(contract, tuesday, 3000, 900), "volume": Decimal(len(minutes))},
            {**daily(contract, wednesday, 3010, 900, auction=False), "volume": Decimal(0)}],
            [tuesday.isoformat(), wednesday.isoformat()])
        version = writer.commit().identity
        script = EXAMPLES.parent / "tests/acceptance/research_minutes.py"
        build = Path(asterion_engine.__file__).resolve().parents[1]
        spec = importlib.util.spec_from_file_location("research_minutes", script)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        def audit(first, last):
            report = self.directory / f"{first}-{last}.json"
            code = module.main(["--build", str(build), "--lab-directory", str(self.directory),
                                "--version", version, "--contract", contract,
                                "--first-day", first, "--last-day", last, "--report", str(report)])
            return code, json.loads(report.read_text())

        code, passed = audit(tuesday.isoformat(), wednesday.isoformat())
        self.assertEqual((code, passed["passed"], passed["days"][1]["zero_volume_day"]), (0, True, True))
        code, missing = audit(DAYS[0].isoformat(), wednesday.isoformat())
        self.assertEqual((code, missing["passed"], missing["minute_coverage_complete"]), (1, False, False))
        code, out_of_life = audit(pre_listed.isoformat(), wednesday.isoformat())
        self.assertEqual((code, out_of_life["passed"], out_of_life["minute_coverage_complete"]), (1, False, False))

    def test_multi_symbol_sweep_runs_each_symbol_independently_on_its_own_stored_span(self):
        from warehouse_fixture import contract_row

        later = "DCE/i/2026-05"
        dce_row = contract_row(later)
        dce_row.update(list_date=DAYS[5].isoformat(), delist_date=DAYS[-1].isoformat())
        writer = Warehouse(self.directory).writer()
        calendar_days = [DAYS[5] + datetime.timedelta(days=n)
                         for n in range((DAYS[-1] - DAYS[5]).days + 1)]
        writer.put_rows("calendar/DCE/2026", "calendar", [
            dict(exchange="DCE", day=day.isoformat(), open=day in DAYS[5:], previous_trading_day=None)
            for day in calendar_days])
        writer.put_rows("contracts/DCE", "contracts", [dce_row])
        writer.put_rows("price_ticks/DCE", "price_ticks", [price_tick(later)])
        for day in DAYS[5:]:
            writer.put_rows(f"settlement/DCE/{day.isoformat()}", "settlement", [settlement(later, day)])
        jan = [day for day in DAYS[5:] if day.isoformat() < "2026-02"]
        feb = [day for day in DAYS[5:] if day.isoformat() >= "2026-02"]
        writer.add_days("DCE", "2026-01",
                        [daily(later, day, 800 + n * 5, 500) for n, day in enumerate(jan)],
                        [day.isoformat() for day in jan])
        writer.add_days("DCE", "2026-02",
                        [daily(later, day, 860 + n * 5, 500) for n, day in enumerate(feb)],
                        [day.isoformat() for day in feb])
        writer.commit()

        async def scenario():
            async with self.service() as service:
                listed = (await service.reply(Request(id=1, list_strategies={"path": str(self.repository)}))).list_strategies
                by_id = {item.id: item.portfolio for item in listed.strategies}
                self.assertFalse(by_id["ma_cross.MaCross"])
                self.assertTrue(by_id["composite.CompositePortfolio"])

                req = Request(id=2)
                sw = req.start_sweep
                sw.id, sw.processes = "multi-sym", 2
                sw.symbols.extend(["SHFE/rb/2026-05", later])
                run = sw.base
                run.repository, run.strategy = str(self.repository), "ma_cross.MaCross"
                self.fill(run)
                run.first_day = run.last_day = ""
                run.params["fast"].integer = 2
                run.params["slow"].integer = 4
                run.params["lots"].integer = 1
                self.assertEqual((await service.reply(req)).WhichOneof("result"), "start_sweep")
                await self.idle(service)
                reply = (await service.reply(Request(id=3, get_sweep={"id": "multi-sym"}))).get_sweep
                self.assertEqual([list(r.symbols) for r in reply.runs], [["SHFE/rb/2026-05"], [later]])
                self.assertEqual([r.state for r in reply.runs], [SUCCEEDED, SUCCEEDED])
                self.assertEqual((reply.runs[0].first_day, reply.runs[0].last_day),
                                 (DAYS[0].isoformat(), DAYS[-1].isoformat()))
                self.assertEqual((reply.runs[1].first_day, reply.runs[1].last_day),
                                 (DAYS[5].isoformat(), DAYS[-1].isoformat()))
                page = (await service.reply(Request(id=4, list_runs={"limit": 10}))).list_runs
                self.assertEqual(list(page.runs[0].symbols), ["SHFE/rb/2026-05", later])

        asyncio.run(scenario())

    def test_stored_daily_price_limits_block_limit_locked_fills_and_clamp_slippage(self):
        import asterion_engine

        contract = "SHFE/rb/2026-05"
        writer = Warehouse(self.directory).writer()
        rows = [
            daily(contract, DAYS[0], 3000, 1000, lower_limit=2850, upper_limit=3150),
            # Day 1: locked at limit-up (low == upper_limit == 3050) -> buy must NOT fill.
            {**daily(contract, DAYS[1], 3050, 1000, lower_limit=2900, upper_limit=3050),
             "open": Decimal(3050), "high": Decimal(3050), "low": Decimal(3050), "close": Decimal(3050)},
            # Day 2: opens at 3048, trades below 3050 -> buy fills, clamped to upper_limit=3050 despite slippage=5.
            {**daily(contract, DAYS[2], 3048, 1000, lower_limit=2900, upper_limit=3050),
             "open": Decimal(3048), "high": Decimal(3050), "low": Decimal(3000), "close": Decimal(3048)},
            # Day 3: locked at limit-down (high == lower_limit == 2900) -> sell-to-close must NOT fill.
            {**daily(contract, DAYS[3], 2900, 1000, lower_limit=2900, upper_limit=3150),
             "open": Decimal(2900), "high": Decimal(2900), "low": Decimal(2900), "close": Decimal(2900)},
        ]
        writer.add_days("SHFE", "2026-01", rows, [day.isoformat() for day in DAYS[:4]])
        version = writer.commit().identity

        class BuyThenExit(Strategy):
            def __init__(self):
                super().__init__()
                self.step = 0

            def on_bar(self, ctx, bar):
                self.step += 1
                if self.step == 1:
                    ctx.target(bar.symbol, 1)
                elif self.step == 3:
                    ctx.target(bar.symbol, 0)

            def ready(self, ctx):
                return True

        config = backtest.Config(
            repository=str(self.repository), strategy="buy_then_exit", params={},
            symbols=(contract,), begin_day=DAYS[0].isoformat(), end_day=DAYS[3].isoformat(),
            deposit=Decimal(100000), costs=None, slippage_ticks=5,
            max_order_lots=Decimal(10), max_gross_lots=Decimal(10),
            max_contract_lots=Decimal(10), max_working_orders=10, dataset_version=version,
        )
        data = backtest.Data(Dataset(self.directory), config.begin_day, config.end_day, version=version)
        contracts, series, _ = backtest.prepare(data, config)
        self.assertEqual(len(contracts[0]["price_limits"]), 4)
        r = backtest.R
        engine = asterion_engine.Replay(
            backtest.raw(config.deposit), contracts, series,
            (10 * r, 10 * r, 10, 10 * r),
        )
        backtest.replay(engine, BuyThenExit(), {0: contract}, contracts, config)
        fills = engine.fills()
        # Only the day-2 buy fills; day-1 limit-up and day-3 limit-down both block execution.
        self.assertEqual(len(fills), 1)
        self.assertEqual(fills[0][6], DAYS[2].isoformat())
        self.assertEqual(fills[0][3], 3050 * r)
        # Position remains open on day 3 because the limit-down close could not fill.
        held = [row for row in engine.positions if row[0] == DAYS[3].isoformat()]
        self.assertEqual([(row[2], row[3]) for row in held], [(r, 0)])

        # Incomplete limits (only one side present) are refused rather than silently skipped.
        bad_writer = Warehouse(self.directory).writer()
        bad_writer.add_days("SHFE", "2026-01", [
            daily(contract, DAYS[0], 3000, 1000, lower_limit=2850, upper_limit=None),
        ], [DAYS[0].isoformat()])
        bad_version = bad_writer.commit().identity
        bad_data = backtest.Data(Dataset(self.directory), DAYS[0].isoformat(), DAYS[0].isoformat(), version=bad_version)
        with self.assertRaisesRegex(ValueError, "incomplete price limits"):
            backtest.contract_input(bad_data, config, contract, DAYS[0].isoformat(), DAYS[0].isoformat())


if __name__ == "__main__":
    unittest.main()
