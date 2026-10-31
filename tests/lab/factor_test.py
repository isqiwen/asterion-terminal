"""Factors: found in the strategy repository, evaluated across symbols.

Run with the bundled interpreter:
python3 -I -B factor_test.py <source> <generated-python> <engine-directory>
"""
import asyncio
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1), sys.argv.pop(1), str(Path(__file__).parent)]
import numpy as np  # noqa: E402
import polars as pl  # noqa: E402
from asterion.lab_pb2 import Request  # noqa: E402
from asterion_lab import factor, repository  # noqa: E402
from asterion_lab.service import Service  # noqa: E402
from warehouse_fixture import DAYS, warehouse  # noqa: E402

EXAMPLES = Path(__file__).parents[2] / "apps/services/lab/examples"


def git(root, *arguments):
    subprocess.run(["git", "-C", str(root), "-c", "user.name=test", "-c", "user.email=test@invalid",
                    *arguments], check=True, capture_output=True)


def frame(values, rates, bars=5):
    """Symbols s1.. whose closes grow by `rates` each bar and whose factor is `values`."""
    rows = []
    for symbol, (value, rate) in enumerate(zip(values, rates), 1):
        for bar in range(bars):
            rows.append(dict(symbol=f"s{symbol}", day=f"2026-01-0{bar + 1}", bar=bar,
                             close=100 * (1 + rate) ** bar, value=float(value)))
    return pl.DataFrame(rows)


class Evaluation(unittest.TestCase):
    def test_values_that_order_the_returns_have_an_ic_of_one(self):
        result = factor.evaluate(frame([1, 2, 3, 4], [0.01, 0.02, 0.03, 0.04]), [1, 2], 2)
        for horizon, days in ((1, 4), (2, 3)):
            item = next(item for item in result["horizons"] if item["horizon"] == horizon)
            self.assertEqual(item["days"], days)
            self.assertAlmostEqual(item["mean"], 1.0)
            self.assertEqual(item["positive"], 1.0)
            # Every day alike: no deviation, so no ratio.
            self.assertAlmostEqual(item["deviation"], 0.0)
        low, high = result["groups"]
        self.assertAlmostEqual(low["mean_return"], 0.015)
        self.assertAlmostEqual(high["mean_return"], 0.035)
        self.assertEqual((low["turnover"], high["turnover"]), (0.0, 0.0))
        last = result["days"][-1]
        # The last bar has no next one: it keeps the cumulative of the day before.
        self.assertIsNone(last["ic"])
        self.assertEqual(last["groups"], result["days"][-2]["groups"])
        self.assertAlmostEqual(last["groups"][0], 1.015 ** 4)
        self.assertAlmostEqual(last["long_short"], 1.02 ** 4)

    def test_reversed_values_have_an_ic_of_minus_one_and_too_few_symbols_none(self):
        result = factor.evaluate(frame([4, 3, 2, 1], [0.01, 0.02, 0.03, 0.04]), [1], 2)
        self.assertAlmostEqual(result["horizons"][0]["mean"], -1.0)
        few = factor.evaluate(frame([1, 2], [0.01, 0.02]), [1], 2)
        self.assertEqual(few["horizons"][0]["days"], 0)
        self.assertIsNone(few["horizons"][0]["mean"])

    def test_a_member_that_changes_group_is_turnover(self):
        rows = frame([1, 2, 3, 4], [0.01, 0.02, 0.03, 0.04], bars=3)
        # s1 and s4 swap places on the second bar.
        rows = rows.with_columns(pl.when((pl.col("bar") == 1) & (pl.col("symbol") == "s1")).then(5.0)
                                 .when((pl.col("bar") == 1) & (pl.col("symbol") == "s4")).then(0.0)
                                 .otherwise(pl.col("value")).alias("value"))
        result = factor.evaluate(rows, [1], 2)
        # Two group days (the last bar has no return): half of each group is new.
        self.assertEqual([group["turnover"] for group in result["groups"]], [0.5, 0.5])

    def test_a_factor_returns_one_value_per_bar(self):
        symbols = {"a": {"day": ["2026-01-01", "2026-01-02"], "close": np.array([1.0, 2.0])}}
        with self.assertRaisesRegex(ValueError, r"gave \(1,\) values for the 2 bars of a"):
            factor.values(lambda close: close[:1], ["close"], {}, symbols)
        with self.assertRaisesRegex(ValueError, "failed on a: ZeroDivisionError"):
            factor.values(lambda close: 1 / 0, ["close"], {}, symbols)


class Repository(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name) / "lab"
        self.directory.mkdir()
        warehouse(self.directory)
        self.repository = Path(self.temporary.name) / "strategies"
        shutil.copytree(EXAMPLES, self.repository)
        (self.repository / "factors/broken.py").write_text("def f(close, window):\n    return close\n")
        git(self.repository, "init", "-q")
        git(self.repository, "add", ".")
        git(self.repository, "commit", "-qm", "examples")

    def tearDown(self):
        self.temporary.cleanup()

    def test_factors_are_listed_with_inputs_and_settings(self):
        listed = repository.list_strategies(str(self.repository))
        entries = {entry.id: entry for entry in listed.factors}
        self.assertEqual(sorted(entries), ["positions.interest_change", "price.momentum",
                                           "price.reversal", "price.volatility"])
        momentum = entries["price.momentum"]
        self.assertEqual(list(momentum.inputs), ["close"])
        self.assertEqual([(item.name, item.kind, item.default.integer)
                          for item in momentum.parameters], [("lookback", "int", 20)])
        (error,) = listed.errors
        self.assertEqual(error.module, "factors/broken")
        self.assertIn("parameter window is no bar field", error.message)

    def test_the_service_evaluates_a_factor_on_what_a_strategy_sees(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            request = Request(version=1, service_id="lab")
            run = request.factor
            run.repository, run.factor = str(self.repository), "price.momentum"
            run.params["lookback"].integer = 2
            run.symbols.extend(["SHFE/rb/2026-05", "SHFE/rb/2026-10", "SHFE/rb"])
            run.begin_day, run.end_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
            run.horizons.extend([1, 3])
            run.groups = 3
            reply = await service.reply(request)
            self.assertEqual(reply.WhichOneof("body"), "factor", reply)
            result = reply.factor
            self.assertEqual(list(result.symbols), list(run.symbols))
            self.assertEqual([item.horizon for item in result.horizons], [1, 3])
            self.assertEqual(len(result.days), len(DAYS))
            # The series begins a day later, when the open interest names its month;
            # from its third bar on every symbol has a value.
            self.assertEqual([day.symbols for day in result.days[:4]], [0, 0, 2, 3])
            self.assertGreater(result.horizons[0].days, 0)
            self.assertEqual(len(result.groups), 3)
            self.assertEqual(len(result.days[-1].groups), 3)
            run.params["lookback"].integer = 0
            failed = await service.reply(request)
            self.assertIn("lookback is at least one bar", failed.error.message)
            run.params.clear()
            run.symbols[:] = ["SHFE"]
            exchange = (await service.reply(request)).factor
            self.assertEqual(list(exchange.symbols), ["SHFE/rb"])

        asyncio.run(scenario())


if __name__ == "__main__":
    unittest.main()
