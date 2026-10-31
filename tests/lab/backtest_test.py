"""Backtests from the warehouse through asterion_engine, and the service's runs.

Run with the bundled interpreter:
python3 -I -B backtest_test.py <source> <generated-python> <engine-directory>
"""
import asyncio
from decimal import Decimal
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1), sys.argv.pop(1), str(Path(__file__).parent)]
from asterion.lab_pb2 import Request  # noqa: E402
from asterion_lab import backtest  # noqa: E402
from asterion_lab.runs import Runs  # noqa: E402
from asterion_lab.service import Service  # noqa: E402
from asterion_lab.warehouse import Warehouse  # noqa: E402
from warehouse_fixture import DAYS, warehouse  # noqa: E402

EXAMPLES = Path(__file__).parents[2] / "apps/services/lab/examples"


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
            response = await service.reply(request)
            self.assertEqual(response.WhichOneof("body"), "accepted", response)
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
            self.assertEqual((await service.reply(request)).WhichOneof("body"), "accepted")
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
            self.assertEqual((await service.reply(request)).WhichOneof("body"), "accepted")
            deadline = time.monotonic() + 60
            while (service.backtests or service.waiting or service.rolling) and time.monotonic() < deadline:
                await asyncio.sleep(0.1)
            reply = (await service.reply(Request(version=1, service_id="lab", sweep_runs={"id": "walk"}))).sweep_runs
            self.assertEqual(reply.rolling.state, "succeeded", reply.rolling.error)
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

        asyncio.run(scenario())
        runs = Runs(self.directory)
        runs.index.execute("update runs set state = 'running' where id = 'stopped-0'")
        Service("lab", self.directory, type("Log", (), {"write": lambda *a, **k: None})())
        self.assertEqual(next(row for row in Runs(self.directory).list() if row["id"] == "stopped-0")["state"],
                         "interrupted")


if __name__ == "__main__":
    unittest.main()
