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
import tempfile
import time
import unittest

sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1), sys.argv.pop(1)]
from asterion.lab_pb2 import Request  # noqa: E402
from asterion_lab import backtest  # noqa: E402
from asterion_lab.runs import Runs  # noqa: E402
from asterion_lab.service import Service  # noqa: E402
from asterion_lab.warehouse import Warehouse  # noqa: E402

EXAMPLES = Path(__file__).parents[2] / "apps/services/lab/examples"
FIRST = datetime.date(2026, 1, 5)


def weekdays(count):
    days, day = [], FIRST
    while len(days) < count:
        if day.weekday() < 5:
            days.append(day)
        day += datetime.timedelta(days=1)
    return days


DAYS = weekdays(30)


def daily(contract, day, close, interest, auction=True):
    exchange, product, _ = contract.split("/")
    close = Decimal(close)
    return dict(exchange=exchange, product=product, contract=contract, day=day.isoformat(),
                open=close if auction else None, high=close + 15 if auction else None,
                low=close - 15 if auction else None, close=close if auction else None,
                settlement=close, previous_close=None, previous_settlement=None,
                volume=Decimal(100), amount=Decimal(100) * close * 10, open_interest=Decimal(interest),
                open_interest_change=None, delivery_settlement=None)


def contract_row(contract):
    exchange, product, month = contract.split("/")
    return dict(exchange=exchange, contract=contract, product=product, delivery_month=month,
                source_instrument=contract, name=contract, list_date="2025-01-02",
                delist_date="2026-12-15", last_delivery_date=None, multiplier=Decimal(10),
                per_unit=Decimal(1), trade_unit="吨", quote_unit="元/吨",
                tick_description="1元/吨", delivery_description="", trading_hours="")


def closes(count):
    # Up for a while, then down: a moving average rule goes long, then short.
    return [3000 + 10 * i if i < count // 2 else 3000 + 10 * (count - i) for i in range(count)]


def warehouse(directory: Path):
    """SHFE/rb with two months: 2605 most held until day 15, then 2610."""
    rows = []
    for i, (day, close) in enumerate(zip(DAYS, closes(len(DAYS)))):
        rows.append(daily("SHFE/rb/2026-05", day, close, 1000 if i < 15 else 100))
        rows.append(daily("SHFE/rb/2026-10", day, close + 20, 500 if i < 15 else 2000))
    writer = Warehouse(directory).writer()
    writer.put_rows("calendar/SHFE/2026", "calendar", [
        dict(exchange="SHFE", day=day.isoformat(), open=True, previous_trading_day=None)
        for day in DAYS])
    writer.put_rows("contracts/SHFE", "contracts",
                    [contract_row("SHFE/rb/2026-05"), contract_row("SHFE/rb/2026-10")])
    writer.add_days("SHFE", "2026-01", [row for row in rows if row["day"] < "2026-02"],
                    [day.isoformat() for day in DAYS if day.isoformat() < "2026-02"])
    writer.add_days("SHFE", "2026-02", [row for row in rows if row["day"] >= "2026-02"],
                    [day.isoformat() for day in DAYS if day.isoformat() >= "2026-02"])
    writer.commit()


def git(root, *arguments):
    subprocess.run(["git", "-C", str(root), "-c", "user.name=test", "-c", "user.email=test@invalid",
                    *arguments], check=True, capture_output=True)


COSTS = {"SHFE/rb": [Decimal(0), Decimal(3), Decimal(3), Decimal(3), Decimal("0.1"),
                     Decimal(0), Decimal(0), Decimal(0)]}


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
        return backtest.Config(**values)

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

    def test_a_failed_run_is_recorded(self):
        with self.assertRaises(ValueError):
            backtest.run(self.directory, "missing", self.config(("SHFE/rb/2027-01",)))
        (row,) = Runs(self.directory).list()
        self.assertEqual((row["state"], row["error"]), ("failed", "no daily bars of SHFE/rb/2027-01"))
        with self.assertRaisesRegex(ValueError, "no costs"):
            backtest.run(self.directory, "costs", self.config(("SHFE/rb/2026-05",), costs={}))

    def test_the_service_runs_a_backtest_in_a_child_process(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            request = Request(version=1, service_id="lab")
            run = request.backtest
            run.id, run.repository, run.strategy = "child", str(self.repository), "ma_cross.MaCross"
            run.params["slow"].integer = 4
            run.symbols.append("SHFE/rb/2026-05")
            run.begin_day, run.end_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
            run.deposit, run.max_order_lots, run.max_gross_lots = "1000000", "10", "10"
            run.max_working_orders = 10
            run.costs.add(product="SHFE/rb", values=[str(value) for value in COSTS["SHFE/rb"]])
            response = await service.reply(request)
            self.assertEqual(response.WhichOneof("body"), "accepted", response)
            self.assertEqual((await service.reply(request)).error.code, "invalid_request")
            deadline = time.monotonic() + 30
            while "child" in service.backtests and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            listed = await service.reply(Request(version=1, service_id="lab", runs={}))
            (row,) = listed.runs.runs
            self.assertEqual(row.state, "succeeded", row.error)
            self.assertIn("total_return", row.metrics)
            run.id, run.repository = "elsewhere", str(Path(self.temporary.name) / "missing")
            self.assertEqual((await service.reply(request)).error.code, "invalid_request")

        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
