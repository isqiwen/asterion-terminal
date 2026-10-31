"""The strategy interface, the strategy repository and run records.

Run with the bundled interpreter: python3 -I -B strategy_test.py <source> <generated-python>
"""
import asyncio
import datetime
from decimal import Decimal
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1)]
import polars as pl  # noqa: E402
from asterion.lab_pb2 import Request  # noqa: E402
from asterion_lab import repository  # noqa: E402
from asterion_lab.runs import TABLES, Plan, Runs  # noqa: E402
from asterion_lab.service import Service  # noqa: E402
from asterion_lab.strategy import Bar, Context, Strategy, step  # noqa: E402

EXAMPLES = Path(__file__).parents[2] / "apps/services/lab/examples"


def bar(symbol, minute, close):
    time = datetime.datetime(2026, 10, 30, 9) + datetime.timedelta(minutes=minute)
    return Bar(symbol, symbol, time, close, close, close, close, 10.0, 100.0)


class Follow(Strategy):
    params = {"lots": 2, "scale": 1.0, "name": "x", "short": False}

    def on_bar(self, ctx, bar):
        ctx.target(bar.symbol, -self.params["lots"] if self.params["short"] else self.params["lots"])


class Interface(unittest.TestCase):
    def test_parameters(self):
        self.assertEqual(Follow().params, {"lots": 2, "scale": 1.0, "name": "x", "short": False})
        self.assertEqual(Follow(scale=2, short=True).params["scale"], 2.0)
        for values in ({"lots": 1.5}, {"lots": True}, {"short": 1}, {"scale": float("nan")},
                       {"name": 3}, {"other": 1}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                Follow(**values)
        with self.assertRaises(TypeError):
            type("Listed", (Strategy,), {"params": {"window": [5]}, "on_bar": Follow.on_bar})
        with self.assertRaises(TypeError):
            type("Idle", (Strategy,), {"params": {}})

    def test_history_through_the_current_bar(self):
        seen = []

        class Watch(Strategy):
            def on_bar(self, ctx, bar):
                seen.append(ctx.history(bar.symbol, "close", 3).tolist())

        context, strategy = Context(["a"]), Watch()
        for minute in range(300):  # past the first growth of the columns
            step(strategy, context, [bar("a", minute, float(minute))])
        self.assertEqual(seen[:3], [[0.0], [0.0, 1.0], [0.0, 1.0, 2.0]])
        self.assertEqual(seen[-1], [297.0, 298.0, 299.0])
        values = context.history("a", "close", 2)
        with self.assertRaises(ValueError):
            values[0] = 1.0
        for arguments in (("b", "close", 1), ("a", "settlement", 1)):
            with self.assertRaises(KeyError):
                context.history(*arguments)
        with self.assertRaises(ValueError):
            context.history("a", "close", 0)

    def test_targets_and_weights(self):
        context = Context(["a", "b"])
        self.assertEqual(step(Follow(), context, [bar("a", 0, 1.0)]), {"a": 2})
        self.assertEqual(step(Follow(short=True), context, [bar("a", 1, 1.0), bar("b", 1, 1.0)]),
                         {"a": -2, "b": -2})
        for lots in (1.0, True, "1"):
            with self.assertRaises(TypeError):
                context.target("a", lots)
        with self.assertRaises(KeyError):
            context.target("c", 1)
        with self.assertRaises(RuntimeError):
            context.weights({"a": 0.5})

        class Tilt(Strategy):
            def on_bars(self, ctx, bars):
                if len(bars) == 2:
                    ctx.weights({"a": 0.5})

        context = Context(["a", "b"])
        self.assertEqual(step(Tilt(), context, [bar("a", 0, 1.0)]), {})
        self.assertEqual(step(Tilt(), context, [bar("a", 1, 1.0), bar("b", 1, 1.0)]),
                         {"a": 0.5, "b": 0.0})
        for weight in (float("inf"), "0.5", True):
            with self.assertRaises(TypeError):
                context.weights({"a": weight})
        with self.assertRaises(RuntimeError):
            context.target("a", 1)

    def test_bars_come_in_order(self):
        context = Context(["a", "b"])
        step(Follow(), context, [bar("a", 1, 1.0)])
        for bars in ([bar("a", 1, 1.0)], [bar("a", 0, 1.0)], [bar("a", 2, 1.0), bar("b", 3, 1.0)],
                     [bar("c", 2, 1.0)], [bar("a", 2, 1.0), bar("a", 2, 1.0)], []):
            with self.subTest(bars=bars), self.assertRaises(ValueError):
                step(Follow(), context, bars)
        with self.assertRaises(ValueError):
            Context(["a", "a"])


def git(root, *arguments):
    subprocess.run(["git", "-C", str(root), "-c", "user.name=test", "-c", "user.email=test@invalid",
                    *arguments], check=True, capture_output=True)


class Repository(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "strategies-repository"
        shutil.copytree(EXAMPLES, self.root)
        (self.root / "strategies/broken.py").write_text("import missing_package\n")
        # A module that prints while it imports; the listing's reply is unharmed.
        (self.root / "strategies/noisy.py").write_text(
            "from asterion_lab.strategy import Strategy\nfrom helpers import LOTS\nprint('loaded')\n"
            "class Noisy(Strategy):\n    params = {'lots': LOTS, 'scale': 0.5, 'tag': 'n', 'on': True}\n"
            "    def on_bar(self, ctx, bar):\n        pass\n")
        (self.root / "helpers.py").write_text("LOTS = 3\n")
        git(self.root, "init", "-q")
        git(self.root, "add", ".")
        git(self.root, "commit", "-qm", "strategies")

    def tearDown(self):
        self.temporary.cleanup()

    def test_listing(self):
        listed = repository.list_strategies(str(self.root))
        head = subprocess.run(["git", "-C", str(self.root), "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
        self.assertEqual((listed.commit, listed.changes), (head, ""))
        # Modules whose names begin with an underscore hold shared code, not strategies.
        self.assertEqual([entry.id for entry in listed.strategies],
                         ["breakout.Breakout", "ma_cross.MaCross", "momentum.Momentum",
                          "momentum_rank.MomentumRank", "noisy.Noisy", "reversion.Reversion"])
        strategies = {entry.id: entry for entry in listed.strategies}
        self.assertTrue(strategies["ma_cross.MaCross"].description.startswith("Long while the mean"))
        self.assertEqual([(item.name, item.kind, item.default.WhichOneof("value"))
                          for item in strategies["noisy.Noisy"].parameters],
                         [("lots", "int", "integer"), ("scale", "float", "number"),
                          ("tag", "str", "text"), ("on", "bool", "flag")])
        self.assertEqual(strategies["noisy.Noisy"].parameters[0].default.integer, 3)
        self.assertEqual([error.module for error in listed.errors], ["broken"])
        self.assertIn("missing_package", listed.errors[0].message)

    def test_uncommitted_changes_are_named(self):
        (self.root / "helpers.py").write_text("LOTS = 4\n")
        changed = repository.revision(self.root).changes
        self.assertEqual(len(changed), 64)
        listed = {entry.id: entry for entry in repository.list_strategies(str(self.root)).strategies}
        self.assertEqual(listed["noisy.Noisy"].parameters[0].default.integer, 4)
        (self.root / "strategies/new.py").write_text("\n")
        self.assertNotIn(repository.revision(self.root).changes, ("", changed))
        git(self.root, "add", ".")
        git(self.root, "commit", "-qm", "more")
        self.assertEqual(repository.revision(self.root).changes, "")

    def test_load_checks_parameters(self):
        strategy = repository.load(self.root, "ma_cross.MaCross", {"fast": 3, "slow": 6})
        self.assertEqual(strategy.params, {"fast": 3, "slow": 6, "lots": 1, "sides": "both"})
        context = Context(["SHFE/rb"])
        wanted = [step(strategy, context, [bar("SHFE/rb", minute, close)])
                  for minute, close in enumerate([10.0, 11, 12, 13, 14, 15, 9, 8, 7])]
        self.assertEqual(wanted, [{}] * 5 + [{"SHFE/rb": 1}] * 2 + [{"SHFE/rb": -1}] * 2)
        with self.assertRaisesRegex(ValueError, "unknown parameters: window"):
            repository.load(self.root, "ma_cross.MaCross", {"window": 3})
        with self.assertRaisesRegex(ValueError, "parameter slow is a int"):
            repository.load(self.root, "ma_cross.MaCross", {"slow": "20"})
        with self.assertRaisesRegex(ValueError, "no strategy"):
            repository.load(self.root, "ma_cross.Other", {})
        with self.assertRaisesRegex(ValueError, "failed to import"):
            repository.load(self.root, "broken.Any", {})

    def test_not_a_repository(self):
        plain = Path(self.temporary.name) / "plain"
        plain.mkdir()
        for root in (str(plain), "relative", str(self.root / "strategies")):
            with self.subTest(root=root), self.assertRaises(ValueError):
                repository.list_strategies(root)

    def test_service(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        service = Service("lab", Path(self.temporary.name), Log())
        request = Request(version=1, service_id="lab")
        request.strategies.repository = str(self.root)
        response = asyncio.run(service.reply(request))
        self.assertEqual(len(response.strategies.strategies), 6, response)
        request.strategies.repository = str(Path(self.temporary.name) / "missing")
        response = asyncio.run(service.reply(request))
        self.assertEqual(response.error.code, "invalid_request")


def empty_tables():
    return {name: pl.DataFrame(schema=schema) for name, schema in TABLES.items()}


class Records(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.plan = Plan(repository="/strategies", strategy="ma_cross.MaCross",
                         revision=repository.Revision("c" * 40, ""), params={"fast": 5},
                         provider="tushare", dataset_version="d" * 64, symbols=("SHFE/rb",),
                         begin_day="2025-01-02", end_day="2025-12-31",
                         costs={"fee_per_lot": "3"}, risk={"max_lots": 10}, statistics={})

    def tearDown(self):
        self.temporary.cleanup()

    def test_a_run_from_start_to_success(self):
        runs = Runs(self.directory)
        runs.start("run-1", self.plan)
        record = runs.record("run-1")
        self.assertEqual(record["strategy"]["commit"], "c" * 40)
        self.assertEqual(record["data"]["symbols"], ["SHFE/rb"])
        self.assertEqual(len(record["engine"]["requirements"]), 64)
        self.assertEqual(runs.list()[0]["state"], "running")
        tables = empty_tables()
        tables["equity"] = pl.DataFrame(
            {"time_ns": [1, 2], "equity": [Decimal("1000000.12345678"), Decimal("999999.5")]},
            schema=TABLES["equity"])
        with self.assertRaises(ValueError):
            runs.succeed("run-1", {**tables, "equity": tables["equity"].cast({"equity": pl.Float64})},
                         {})
        with self.assertRaises(ValueError):
            runs.succeed("run-1", tables, {"sharpe": float("nan")})
        runs.succeed("run-1", tables, {"total_return": -0.0000005, "sharpe": None})
        self.assertEqual(runs.table("run-1", "equity")["equity"].to_list()[0], Decimal("1000000.12345678"))
        listed = Runs(self.directory).list()  # as the next lab process reads it
        self.assertEqual((listed[0]["state"], listed[0]["metrics"]),
                         ("succeeded", {"total_return": -0.0000005, "sharpe": None}))
        with self.assertRaises(ValueError):
            runs.fail("run-1", "failed", "late")

    def test_runs_that_stop(self):
        runs = Runs(self.directory)
        runs.start("run-1", self.plan)
        runs.start("run-2", self.plan)
        runs.fail("run-1", "failed", "data missing")
        runs.fail("run-2", "interrupted", "")
        self.assertEqual({row["id"]: (row["state"], row["error"]) for row in runs.list()},
                         {"run-1": ("failed", "data missing"), "run-2": ("interrupted", "")})
        with self.assertRaises(ValueError):
            runs.start("run-1", self.plan)
        for identity in ("../x", "", "a b"):
            with self.assertRaises(ValueError):
                runs.start(identity, self.plan)
        with self.assertRaises(ValueError):
            runs.fail("run-1", "succeeded", "")


if __name__ == "__main__":
    unittest.main()
