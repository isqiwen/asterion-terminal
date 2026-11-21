"""The strategy interface, the strategy repository and run records.

Run with the bundled interpreter: python3 -I -B strategy_test.py <source> <generated-python>
"""
import asyncio
import datetime
from dataclasses import replace
import json
import os
import select
import signal
import socket
import struct
from decimal import Decimal
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "apps/services/common/python"))
import tempfile
import time
import tomllib
import unittest
from unittest.mock import patch

sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1)]
import polars as pl  # noqa: E402
from asterion.common_pb2 import ErrorCode, Period  # noqa: E402
from asterion.lab_pb2 import Request  # noqa: E402
from asterion_lab import repository  # noqa: E402
from asterion_lab.runs import TABLES, Plan, Runs  # noqa: E402
from asterion_lab.service import Service  # noqa: E402
from asterion_strategy import Bar, Context, Strategy, quantize_weight, step  # noqa: E402

EXAMPLES = Path(__file__).resolve().parents[2] / "research"


def bar(symbol, minute, close):
    time = datetime.datetime(2026, 10, 30, 9) + datetime.timedelta(minutes=minute)
    return Bar(symbol, symbol, time, close, close, close, close, 10.0, 100.0)


class Follow(Strategy):
    def ready(self, ctx):
        return True

    params = {"lots": 2, "scale": 1.0, "name": "x", "short": False}

    def on_bar(self, ctx, bar):
        ctx.target(bar.symbol, -self.params["lots"] if self.params["short"] else self.params["lots"])


class Interface(unittest.TestCase):
    def test_lab_and_trading_lock_the_same_strategy_dependencies(self):
        services = EXAMPLES.parent / "apps/services"
        lab = tomllib.loads((services / "lab/pyproject.toml").read_text())
        trading = tomllib.loads((services / "trading/pyproject.toml").read_text())
        dependencies = trading["dependency-groups"]["runtime"]
        self.assertLessEqual(set(dependencies), set(lab["dependency-groups"]["runtime"]))
        locks = [tomllib.loads((services / app / "uv.lock").read_text()) for app in ("lab", "trading")]
        versions = [{package["name"]: package["version"] for package in lock["package"]}
                    for lock in locks]
        for dependency in dependencies:
            name, version = dependency.split("==")
            self.assertEqual([locked[name] for locked in versions], [version, version])

    def test_a_roll_scales_the_series_history_and_never_what_was_seen(self):
        class Watch(Strategy):
            seen = []

            def on_bar(self, ctx, bar):
                self.seen.append(ctx.history(bar.symbol, "close", 10))

        def month(contract, minute, close):
            return Bar("SHFE/rb", contract, bar("SHFE/rb", minute, close).time,
                       close, close, close, close, 10.0, 100.0)

        context, strategy = Context(["SHFE/rb"], {"SHFE/rb/2026-10": 1.2}), Watch()
        for minute, (contract, close) in enumerate([("SHFE/rb/2026-05", 100.0), ("SHFE/rb/2026-05", 110.0),
                                                    ("SHFE/rb/2026-10", 130.0)]):
            step(strategy, context, [month(contract, minute, close)])
        self.assertEqual([list(seen) for seen in strategy.seen],
                         [[100.0], [100.0, 110.0], [120.0, 132.0, 130.0]])
        self.assertEqual(list(context.history("SHFE/rb", "volume", 3)), [10.0] * 3)

    def test_readiness_discards_early_targets_and_restarts_empty(self):
        class Warm(Follow):
            def ready(self, ctx):
                return len(ctx.history("a", "close", self.params["lots"])) == self.params["lots"]
        context, strategy = Context(["a"]), Warm(lots=2)
        self.assertEqual(step(strategy, context, [bar("a", 0, 1.0)]), {})
        self.assertFalse(context.ready)
        self.assertEqual(step(strategy, context, [bar("a", 1, 2.0)]), {"a": 2})
        self.assertTrue(context.ready)
        restarted = Context(["a"])
        self.assertEqual(step(Warm(lots=2), restarted, [bar("a", 2, 3.0)]), {})
        self.assertFalse(restarted.ready)

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
            def ready(self, ctx):
                return len(ctx.history("a", "close", 1)) > 0 and len(ctx.history("b", "close", 1)) > 0

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
        self.assertEqual(quantize_weight(0.303185945), Decimal("0.30318594"))
        self.assertEqual(quantize_weight(0.303185955), Decimal("0.30318596"))

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
            "from asterion_strategy import Strategy\nfrom helpers import LOTS\nprint('loaded')\n"
            "class Noisy(Strategy):\n    params = {'lots': LOTS, 'scale': 0.5, 'tag': 'n', 'on': True}\n"
            "    def on_bar(self, ctx, bar):\n        pass\n")
        (self.root / "helpers.py").write_text("LOTS = 3\n")
        git(self.root, "init", "-q")
        git(self.root, "add", ".")
        git(self.root, "commit", "-qm", "strategies")

    def tearDown(self):
        self.temporary.cleanup()

    def test_a_package_holds_the_strategy_and_what_it_imports_without_importing_it(self):
        from asterion_lab.package import build
        # A large module the strategy does not use is left out of its package and its run.
        (self.root / "strategies/huge.py").write_text("x = 1\n" * (3 << 20))
        package = build(self.root, "ma_cross.MaCross")
        self.assertEqual(package.manifest.entry, "strategies.ma_cross:MaCross")
        self.assertEqual([file.path for file in package.files],
                         ["strategies/_common.py", "strategies/ma_cross.py"])
        self.assertEqual(list(package.manifest.files), ["strategies/_common.py", "strategies/ma_cross.py"])
        copy = Path(self.temporary.name) / "copy"
        repository.snapshot(self.root, copy, "strategies/ma_cross.py")
        self.assertEqual(sorted(path.relative_to(copy).as_posix() for path in copy.rglob("*.py")),
                         ["strategies/_common.py", "strategies/ma_cross.py"])
        # A strategy that uses a factor is published with the factor's module.
        ranked = build(self.root, "momentum_rank.MomentumRank")
        self.assertEqual([file.path for file in ranked.files],
                         ["factors/_rolling.py", "factors/price.py", "strategies/_common.py",
                          "strategies/momentum_rank.py"])
        composite = build(self.root, "composite.CompositePortfolio")
        self.assertEqual([file.path for file in composite.files],
                         ["factors/_rolling.py", "factors/positions.py", "factors/price.py", "factors/risk.py",
                          "factors/technical.py", "factors/term_structure.py", "strategies/_portfolio.py",
                          "strategies/composite.py", "strategies/risk_premia.py"])
        learned = build(self.root, "learning.RandomFeatureForecast")
        self.assertEqual([file.path for file in learned.files],
                         ["factors/_rolling.py", "factors/price.py", "factors/technical.py",
                          "strategies/_common.py", "strategies/_learning.py", "strategies/learning.py"])
        noisy = build(self.root, "noisy.Noisy")
        self.assertEqual([file.path for file in noisy.files], ["helpers.py", "strategies/noisy.py"])
        (self.root / "helpers.py").unlink()
        with self.assertRaisesRegex(ValueError, "strategies/noisy.py imports helpers, which is "
                                                "neither in the strategy directory"):
            build(self.root, "noisy.Noisy")
        with self.assertRaisesRegex(ValueError, "no strategy"):
            build(self.root, "missing.Missing")
        # A strategy or its factor may import SciPy because Trading provides it too.
        (self.root / "factors/stats_rank.py").write_text("import scipy.stats\n")
        self.assertEqual(repository.closure(self.root, "factors/stats_rank.py"),
                         ["factors/stats_rank.py"])
        (self.root / "factors/frames.py").write_text("import polars\n")
        with self.assertRaisesRegex(ValueError, "imports polars"):
            repository.closure(self.root, "factors/frames.py")

    def test_deployment_uses_the_selected_runs_frozen_source_and_configuration(self):
        service = Service("lab", Path(self.temporary.name), None)
        source = (self.root / "strategies/ma_cross.py").read_bytes()
        params = {"fast": 7, "slow": 23}
        for identity, sweep in (("chosen", ""), ("grid-0", "grid")):
            name = f"sweep-{sweep}" if sweep else f"run-{identity}"
            frozen, revision = service.copy_source(self.root, name, "strategies/ma_cross.py")
            if sweep:
                service.runs.plan_sweep(sweep, [(identity, params)])
            service.runs.start(identity, Plan(
                repository=str(self.root), strategy="ma_cross.MaCross", revision=revision,
                params=params, dataset_version="fixed-data", symbols=("SHFE/rb",),
                begin_day="2026-01-01", end_day="2026-01-30", costs={},
                risk={"max_gross_weight": "0.8", "max_net_weight": "0.4"}, statistics={},
                bars={"period": "5m"}))
            service.runs.succeed(identity, empty_tables(), {})
            (self.root / "strategies/ma_cross.py").write_text("raise RuntimeError('edited')\n")
            request = Request(build_package={"run_id": identity})
            response = asyncio.run(service.reply(request))
            self.assertFalse(response.HasField("error"), response)
            built = response.build_package
            self.assertEqual(next(file.content for file in built.files
                                  if file.path == "strategies/ma_cross.py"), source)
            research = built.manifest.research
            self.assertEqual(research.run_id, identity)
            self.assertEqual(research.params["fast"].integer, 7)
            self.assertEqual(research.params["slow"].integer, 23)
            self.assertEqual(research.period, Period.PERIOD_MINUTES_5)
            self.assertEqual(list(research.symbols), ["SHFE/rb"])
            self.assertEqual(research.dataset_version, "fixed-data")
            self.assertEqual(research.weight_limits.gross.text, "0.8")
            shutil.rmtree(frozen)
            with self.assertRaisesRegex(ValueError, "frozen source"):
                service.build_package(request.build_package)
            (self.root / "strategies/ma_cross.py").write_bytes(source)
        service.reads.shutdown()
        service.runs.index.close()

    def test_catalog_packages_match_research_decisions_in_the_trading_child(self):
        from asterion import strategy_pb2 as wire
        from asterion_lab.package import build
        runner = EXAMPLES.parent / "apps/services/trading/python/strategy_runner.py"
        cases = [("risk_premia.CarryRank", dict(window=3, volatility=3, count=1, rebalance=1)),
                 ("learning.RandomFeatureForecast", dict(fast=2, slow=6, train=12, features=32, rebalance=3)),
                 ("risk_premia.ContinuousMomentumRank", dict(window=3, volatility=3, count=1, rebalance=1)),
                 ("risk_premia.MaximumReturnRank", dict(window=3, volatility=3, count=1, rebalance=1)),
                 ("panel.PanelRidgeRank", dict(fast=2, window=6, train=12, volatility=3, count=1, rebalance=3)),
                 ("panel.PanelRandomFeatureRank", dict(fast=2, window=6, train=12, volatility=3, count=1,
                                                      rebalance=3, features=32, use_carry=True))]
        for case, (identity, params) in enumerate(cases):
            with self.subTest(strategy=identity):
                package = build(self.root, identity)
                folder = Path(self.temporary.name) / str(case)
                folder.mkdir()
                for file in package.files:
                    target = folder / file.path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(file.content)
                (folder / "strategy.json").write_text(json.dumps({"entry": package.manifest.entry}))
                endpoint = folder / "child.sock"
                child = subprocess.Popen([sys.executable, "-I", "-B", str(runner), str(endpoint),
                                          str(folder), str(os.getpid())], stderr=subprocess.PIPE)
                try:
                    deadline = time.monotonic() + 10
                    while not endpoint.exists() and child.poll() is None and time.monotonic() < deadline:
                        time.sleep(0.01)
                    self.assertTrue(endpoint.exists(), "Trading strategy child did not start")
                    with socket.socket(socket.AF_UNIX) as channel:
                        channel.settimeout(10)
                        channel.connect(str(endpoint))
                        def send(request):
                            raw = request.SerializeToString()
                            channel.sendall(struct.pack("!I", len(raw)) + raw)
                        def exact(size):
                            parts = bytearray()
                            while len(parts) < size:
                                part = channel.recv(size - len(parts))
                                if not part:
                                    raise EOFError("strategy child closed the socket")
                                parts.extend(part)
                            return bytes(parts)
                        def call(request):
                            send(request)
                            size, = struct.unpack("!I", exact(4))
                            response = wire.Response.FromString(exact(size))
                            self.assertNotEqual(response.WhichOneof("result"), "error", response)
                            return response
                        symbols = ["SHFE/rb", "SHFE/cu", "SHFE/al", "SHFE/zn"]
                        load = wire.Request()
                        load.load.symbols.extend(symbols)
                        for symbol in symbols:
                            exchange, product = symbol.split("/")
                            series = load.load.instance.series.add(exchange=exchange, product=product)
                            series.contracts.add(instrument=dict(exchange=exchange, symbol=product + "2605"),
                                                 delivery_month="2026-05")
                        for name, value in params.items():
                            if isinstance(value, bool):
                                load.load.instance.params[name].flag = value
                            else:
                                load.load.instance.params[name].integer = value
                        self.assertTrue(call(load).HasField("load"))
                        channel.settimeout(2)
                        research = repository.load(self.root, identity, params)
                        ctx = Context(symbols)
                        decisions = []
                        for i in range(25):
                            request = wire.Request(bars=dict(sequence=i + 1))
                            bars = []
                            for j, symbol in enumerate(symbols):
                                close = float(100 + i + (i % 3 - 1) * (j + 1))
                                current = bar(symbol, i, close)
                                current = replace(current, contract=symbol + "/2026-05", roll_yield=j * 0.01)
                                bars.append(current)
                                stamp = current.time.replace(tzinfo=datetime.timezone(datetime.timedelta(hours=8)))
                                item = request.bars.bars.add(instrument=dict(exchange="SHFE",
                                    symbol=symbol.split("/")[1] + "2605"), end_ms=int(stamp.timestamp() * 1000),
                                    volume=10, open_interest=dict(text="100"), roll_yield=dict(text=str(j * 0.01)))
                                for field in ("open", "high", "low", "close"):
                                    getattr(item, field).text = str(close)
                            expected = step(research, ctx, bars)
                            result = call(request).bars
                            self.assertEqual((result.sequence, result.ready), (i + 1, ctx.ready))
                            actual = {"SHFE/" + target.instrument.symbol[:-4]: target for target in result.targets}
                            self.assertEqual(actual.keys(), expected.keys())
                            for symbol, value in expected.items():
                                if ctx.mode == "weights":
                                    self.assertEqual(Decimal(actual[symbol].weight.text), quantize_weight(value))
                                else:
                                    self.assertEqual(actual[symbol].lots, value)
                            decisions.append(expected)
                        self.assertTrue(any(decisions))
                        send(wire.Request(stop={}))
                    _, errors = child.communicate(timeout=5)
                    self.assertEqual(child.returncode, 0, errors.decode())
                finally:
                    if child.poll() is None:
                        child.kill()
                    child.communicate()

    def test_listing(self):
        listed = repository.list_strategies(str(self.root))
        head = subprocess.run(["git", "-C", str(self.root), "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
        self.assertEqual((listed.commit, listed.changes), (head, ""))
        # Modules or classes whose names begin with an underscore hold internal helpers, not listed strategies.
        (self.root / "strategies/private_helper.py").write_text(
            "from asterion_strategy import Strategy\n"
            "class _Helper(Strategy):\n"
            "    def on_bar(self, ctx, bar): pass\n"
            "class Public(Strategy):\n"
            "    def on_bar(self, ctx, bar): pass\n"
        )
        try:
            with_helper = repository.list_strategies(str(self.root))
            self.assertIn("private_helper.Public", [entry.id for entry in with_helper.strategies])
            self.assertNotIn("private_helper._Helper", [entry.id for entry in with_helper.strategies])
        finally:
            (self.root / "strategies/private_helper.py").unlink()
        self.assertEqual([entry.id for entry in listed.strategies],
                         ["breakout.Breakout", "composite.CompositePortfolio", "composite.MultiFactorRank",
                          "learning.RandomFeatureForecast", "learning.RidgeForecast", "ma_cross.MaCross",
                          "momentum.Momentum", "momentum_rank.MomentumRank", "noisy.Noisy",
                          "oscillators.AtrBreakout", "oscillators.RsiReversion",
                          "panel.PanelRandomFeatureRank", "panel.PanelRidgeRank", "reversion.Reversion",
                          "risk_premia.CarryRank", "risk_premia.ContinuousMomentumRank",
                          "risk_premia.DownsideRiskRank", "risk_premia.DualMomentum",
                          "risk_premia.LowVolatilityRank", "risk_premia.MaximumReturnRank", "risk_premia.OpenInterestRank",
                          "risk_premia.SkewnessRank", "risk_premia.TrendCarryRank",
                          "trend.TurningPointMomentum", "trend.VolatilityTargetMomentum"])
        strategies = {entry.id: entry for entry in listed.strategies}
        self.assertTrue(strategies["ma_cross.MaCross"].description.startswith("Long while the mean"))
        self.assertEqual([(item.name, item.default.WhichOneof("value"))
                          for item in strategies["noisy.Noisy"].parameters],
                         [("lots", "integer"), ("scale", "number"), ("tag", "text"), ("on", "flag")])
        self.assertEqual(strategies["noisy.Noisy"].parameters[0].default.integer, 3)
        self.assertEqual([error.module for error in listed.errors], ["broken"])
        self.assertIn("missing_package", listed.errors[0].traceback)

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

    def test_a_strategy_reads_a_factor_of_the_directory(self):
        (self.root / "strategies/trend.py").write_text(
            "from asterion_strategy import Strategy\nfrom factors.price import momentum\n\n\n"
            "class Trend(Strategy):\n    params = {'lookback': 2}\n\n"
            "    def ready(self, ctx):\n        return True\n\n"
            "    def on_bar(self, ctx, bar):\n"
            "        value = ctx.factor(momentum, bar.symbol, 3, lookback=self.params['lookback'])\n"
            "        if value == value:\n"
            "            ctx.target(bar.symbol, 1 if value > 0 else -1)\n")
        strategy = repository.load(self.root, "trend.Trend", {})
        context = Context(["SHFE/rb"])
        wanted = [step(strategy, context, [bar("SHFE/rb", minute, close)])
                  for minute, close in enumerate([10.0, 11, 12, 9])]
        # No value until three bars are known, then the sign of the two-bar return.
        self.assertEqual(wanted, [{}, {}, {"SHFE/rb": 1}, {"SHFE/rb": -1}])
        listed = {entry.id: entry for entry in repository.list_strategies(str(self.root)).strategies}
        self.assertEqual(list(listed["trend.Trend"].factor_modules), ["price"])
        self.assertEqual(list(listed["ma_cross.MaCross"].factor_modules), [])

    def test_load_checks_parameters(self):
        strategy = repository.load(self.root, "ma_cross.MaCross", {"fast": 3, "slow": 6})
        self.assertEqual(strategy.params, {"fast": 3, "slow": 6, "lots": 1, "sides": "both"})
        context = Context(["SHFE/rb"])
        wanted = [step(strategy, context, [bar("SHFE/rb", minute, close)])
                  for minute, close in enumerate([10.0, 11, 12, 13, 14, 15, 9, 8, 7])]
        self.assertEqual(wanted, [{}] * 5 + [{"SHFE/rb": 1}] * 2 + [{"SHFE/rb": -1}] * 2)
        with self.assertRaisesRegex(ValueError, "unknown parameters: window"):
            repository.load(self.root, "ma_cross.MaCross", {"window": 3})
        with self.assertRaisesRegex(ValueError, "lots must be positive"):
            invalid_lots = repository.load(self.root, "ma_cross.MaCross", {"fast": 1, "slow": 2, "lots": 0})
            lots_ctx = Context(["SHFE/rb"])
            step(invalid_lots, lots_ctx, [bar("SHFE/rb", 0, 10.0)])
            step(invalid_lots, lots_ctx, [bar("SHFE/rb", 1, 11.0)])
        for invalid in ({"count": 0}, {"volatility": -1}, {"weight": 0.0}):
            with self.assertRaisesRegex(ValueError, "invalid ranking parameters"):
                step(repository.load(self.root, "momentum_rank.MomentumRank", invalid),
                     Context(["SHFE/rb", "SHFE/cu"]),
                     [bar("SHFE/rb", 0, 10.0), bar("SHFE/cu", 0, 10.0)])
        # MomentumRank reads roll-adjusted history: a flat roll with a 20% basis gap
        # does not outrank a true +5% move.
        rank = repository.load(self.root, "momentum_rank.MomentumRank",
                               {"lookback": 1, "rebalance": 1, "count": 1, "weight": 0.1})
        rank_ctx = Context(["SHFE/rb", "SHFE/cu"], {"SHFE/rb/2026-10": 1.2})
        step(rank, rank_ctx, [Bar("SHFE/rb", "SHFE/rb/2026-05", bar("SHFE/rb", 0, 100.0).time,
                                  100.0, 100.0, 100.0, 100.0, 10.0, 100.0),
                              Bar("SHFE/cu", "SHFE/cu/2026-05", bar("SHFE/cu", 0, 100.0).time,
                                  100.0, 100.0, 100.0, 100.0, 10.0, 100.0)])
        weights = step(rank, rank_ctx, [Bar("SHFE/rb", "SHFE/rb/2026-10", bar("SHFE/rb", 1, 120.0).time,
                                            120.0, 120.0, 120.0, 120.0, 10.0, 100.0),
                                        Bar("SHFE/cu", "SHFE/cu/2026-05", bar("SHFE/cu", 1, 105.0).time,
                                            105.0, 105.0, 105.0, 105.0, 10.0, 100.0)])
        self.assertEqual(weights, {"SHFE/cu": 0.1, "SHFE/rb": -0.1})
        with self.assertRaisesRegex(ValueError, "parameter slow is a int"):
            repository.load(self.root, "ma_cross.MaCross", {"slow": "20"})
        with self.assertRaisesRegex(ValueError, "no strategy"):
            repository.load(self.root, "ma_cross.Other", {})
        with self.assertRaisesRegex(ValueError, "failed to import"):
            repository.load(self.root, "broken.Any", {})

    def test_modules_are_created_changed_and_deleted_but_built_ins_only_read(self):
        builtin = self.root / "strategies/ma_cross.py"
        builtin.chmod(0o444)
        built_in = repository.read_source(self.root, "strategies/ma_cross.py")
        self.assertTrue(built_in.read_only)
        self.assertIn("MaCross", built_in.content)
        self.assertFalse(repository.read_source(self.root, "factors/price.py").read_only)
        with self.assertRaisesRegex(ValueError, "built-in module cannot be changed"):
            repository.write_source(self.root, "strategies/ma_cross.py", "x = 1\n", False)
        with self.assertRaisesRegex(ValueError, "built-in module cannot be deleted"):
            repository.delete_source(self.root, "strategies/ma_cross.py")
        with self.assertRaisesRegex(ValueError, "already exists"):
            repository.write_source(self.root, "strategies/ma_cross.py", "x = 1\n", True)
        with self.assertRaisesRegex(ValueError, "strategies/<name>.py"):
            repository.write_source(self.root, "../escape.py", "x = 1\n", True)
        created = repository.write_source(self.root, "strategies/mine.py", "x = 1\n", True)
        self.assertFalse(created.read_only)
        repository.write_source(self.root, "strategies/mine.py", "x = 2\n", False)
        self.assertEqual(repository.read_source(self.root, "strategies/mine.py").content, "x = 2\n")
        repository.delete_source(self.root, "strategies/mine.py")
        self.assertFalse((self.root / "strategies/mine.py").exists())
        builtin.chmod(0o644)

    def test_a_directory_outside_git_is_a_strategy_directory_too(self):
        plain = Path(self.temporary.name) / "plain"
        shutil.copytree(EXAMPLES, plain)
        listed = repository.list_strategies(str(plain))
        self.assertIn("ma_cross.MaCross", [entry.id for entry in listed.strategies])
        self.assertEqual(repository.revision(plain), repository.Revision("", ""))
        with self.assertRaises(ValueError):
            repository.list_strategies("relative")

    def test_a_subdirectory_of_a_work_tree_records_only_its_own_changes(self):
        project = Path(self.temporary.name) / "project"
        shutil.copytree(EXAMPLES, project / "research")
        git(project, "init", "-q")
        git(project, "add", ".")
        git(project, "commit", "-qm", "project")
        research = project / "research"
        self.assertEqual(repository.revision(research).changes, "")
        (project / "elsewhere.py").write_text("x = 1\n")
        self.assertEqual(repository.revision(research).changes, "")
        (research / "strategies/ma_cross.py").write_text("x = 2\n")
        self.assertNotEqual(repository.revision(research).changes, "")

    def test_service(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        service = Service("lab", Path(self.temporary.name), Log())
        request = Request(id=1)
        request.list_strategies.path = str(self.root)
        response = asyncio.run(service.reply(request))
        self.assertEqual(len(response.list_strategies.strategies), 25, response)
        request.list_strategies.path = str(Path(self.temporary.name) / "missing")
        response = asyncio.run(service.reply(request))
        self.assertEqual(response.error.code, ErrorCode.ERROR_CODE_INVALID_REQUEST)


    def test_cross_section_liquidity_and_combined_strategy(self):
        from asterion_strategy import (
            CombinedStrategy,
            cross_section_rank,
            cross_section_zscore,
            liquid_symbols,
        )

        ranks = cross_section_rank({"a": 10.0, "b": 20.0, "c": 20.0, "d": 40.0, "nan": float("nan")})
        self.assertEqual(set(ranks), {"a", "b", "c", "d"})
        self.assertAlmostEqual(ranks["a"], -1.0)
        self.assertAlmostEqual(ranks["b"], 0.0)
        self.assertAlmostEqual(ranks["c"], 0.0)
        self.assertAlmostEqual(ranks["d"], 1.0)

        zs = cross_section_zscore({"a": -10.0, "b": 0.0, "c": 10.0})
        self.assertAlmostEqual(zs["a"], -1.0)
        self.assertAlmostEqual(zs["b"], 0.0)
        self.assertAlmostEqual(zs["c"], 1.0)

        class SubA(Strategy):
            def ready(self, ctx):
                return len(ctx.history("SHFE/rb", "close", 2)) == 2

            def on_bars(self, ctx, bars):
                if len(ctx.history("SHFE/rb", "close", 2)) == 2:
                    ctx.weights({"SHFE/rb": 0.4, "SHFE/cu": -0.4})

        class SubB(Strategy):
            def ready(self, ctx):
                return len(ctx.history("SHFE/rb", "close", 3)) == 3

            def on_bars(self, ctx, bars):
                # Emits only on bar 3; SubA emitted on bar 2 and stands.
                if len(ctx.history("SHFE/rb", "close", 4)) == 3:
                    ctx.weights({"SHFE/rb": 0.2, "SHFE/cu": 0.2})

        class Pair(CombinedStrategy):
            def components(self):
                return [(SubA(), 0.5), (SubB(), 0.5)]

        ctx = Context(["SHFE/rb", "SHFE/cu"])
        comb = Pair()
        for idx in range(3):
            b_rb = Bar("SHFE/rb", "SHFE/rb", bar("SHFE/rb", idx, 100.0 + idx).time,
                       100.0, 100.0, 100.0, 100.0 + idx, 500.0, 1000.0)
            b_cu = Bar("SHFE/cu", "SHFE/cu", b_rb.time,
                       200.0, 200.0, 200.0, 200.0 + idx, 50.0, 200.0)
            out = step(comb, ctx, [b_rb, b_cu])
            if idx < 2:
                self.assertFalse(ctx.ready)
                self.assertEqual(out, {})
            else:
                self.assertTrue(ctx.ready)
                self.assertAlmostEqual(out["SHFE/rb"], 0.3)
                self.assertAlmostEqual(out["SHFE/cu"], -0.1)
        elig = liquid_symbols(ctx, {"SHFE/rb": b_rb, "SHFE/cu": b_cu},
                              min_bars=3, min_volume=100.0, min_open_interest=500.0)
        self.assertEqual(elig, ["SHFE/rb"])


def empty_tables():
    return {name: pl.DataFrame(schema=schema) for name, schema in TABLES.items()}


class Workers(unittest.TestCase):
    def test_tasks_stop_with_their_owner_even_during_import(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "owned_task.py").write_text(
                'import time\nprint("importing", flush=True)\ntime.sleep(60)\ndef main(): pass\n')
            bootstrap = (f"import sys; sys.path[:] = {[folder, *sys.path]!r}; "
                         "from asterion_lab import worker; ")
            prepared = subprocess.run(
                [sys.executable, "-I", "-c", bootstrap
                 + "import json; print(json.dumps(worker.command('owned_task')))"],
                capture_output=True, check=True, text=True)
            late = subprocess.run(json.loads(prepared.stdout), capture_output=True, timeout=5)
            self.assertNotEqual(late.returncode, 0)
            self.assertEqual(late.stdout, b"")

            pid_file = root / "child.pid"
            owner = subprocess.Popen(
                [sys.executable, "-I", "-c", bootstrap + "import subprocess; from pathlib import Path; "
                 "child = subprocess.Popen(worker.command('owned_task')); "
                 f"Path({str(pid_file)!r}).write_text(str(child.pid)); sys.stdin.buffer.read(1)"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            finished = False
            try:
                self.assertTrue(select.select([owner.stdout], [], [], 5)[0])
                self.assertEqual(owner.stdout.readline(), b"importing\n")
                owner.communicate(b"x", timeout=5)
                self.assertEqual(owner.returncode, 0)
                finished = True
            finally:
                if not finished:
                    if pid_file.exists():
                        try:
                            os.kill(int(pid_file.read_text()), signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    if owner.poll() is None:
                        owner.kill()
                    owner.communicate()


class Records(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.plan = Plan(repository="/strategies", strategy="ma_cross.MaCross",
                         revision=repository.Revision("c" * 40, ""), params={"fast": 5},
                         dataset_version="d" * 64, symbols=("SHFE/rb",),
                         begin_day="2025-01-02", end_day="2025-12-31",
                         costs={"fee_per_lot": "3"}, risk={"max_lots": 10}, statistics={})

    def tearDown(self):
        self.temporary.cleanup()

    def test_reports_follow_the_runs_saved_statistics(self):
        from asterion_lab import report

        runs = Runs(self.directory)
        equity, rows = Decimal("100000"), []
        for number in range(20):
            equity = (equity * (Decimal("1.01") if number % 2 == 0 else Decimal("0.995"))).quantize(
                Decimal("0.00000001"))
            rows.append(dict(trading_day=datetime.date(2026, 1, 1) + datetime.timedelta(days=number),
                             time_ns=number, balance=equity, equity=equity, realized=Decimal(0),
                             fees=Decimal(0), margin=Decimal(0), available=equity))
        tables = empty_tables()
        tables["settlements"] = pl.DataFrame(rows, schema=TABLES["settlements"])
        for number, (periods, zero, expected) in enumerate(
                ((240, True, 10.95445115), (252, True, 11.22497216),
                 (None, True, None), (240, False, None))):
            identity = f"report-{number}"
            runs.start(identity, replace(self.plan, costs={"deposit": "100000"},
                                         statistics={"annual_periods": periods,
                                                     "zero_risk_free": zero}))
            runs.succeed(identity, tables, {})
            figures = report.run(self.directory, identity)["figures"]
            self.assertIn("ulcer_index", figures)
            if expected is None:
                self.assertNotIn("sortino", figures)
            else:
                self.assertAlmostEqual(figures["sortino"], expected, places=6)
        runs.index.close()

    def test_a_run_deleted_during_listing_does_not_break_the_other_records(self):
        runs = Runs(self.directory)
        for identity in ("kept", "deleted"):
            runs.start(identity, self.plan)
            runs.end(identity, "stopped", "")
        read = runs.record

        def concurrent_delete(identity):
            if identity == "deleted":
                runs.delete([identity])
            return read(identity)

        with patch.object(runs, "record", concurrent_delete):
            self.assertEqual([row["id"] for row in runs.list()], ["kept"])
        self.assertIsNone(runs.get("deleted"))
        runs.index.close()

    def test_run_pages_rank_filter_and_group_the_whole_history(self):
        runs = Runs(self.directory)
        for number in range(7):
            identity = f"run-{number}"
            runs.start(identity, replace(self.plan, symbols=("DCE/i" if number == 0 else "SHFE/rb",)))
            runs.end(identity, "stopped", "")
            runs.index.execute("insert into metrics values (?, 'sharpe', ?)", (identity, 10 - number))
        runs.rename("run-0", "首个运行")
        runs.plan_sweep("sweep", [("run-5", {}), ("run-6", {})])
        first, more, strategies = runs.page(2, 0, "", "", "started", False)
        self.assertEqual([row["id"] for row in first], ["run-6", "run-4"])
        self.assertEqual(first[0]["sweep_size"], 2)
        self.assertTrue(more)
        self.assertEqual(strategies, [self.plan.strategy])
        last, more, _ = runs.page(2, 4, "", "", "started", False)
        self.assertEqual([row["id"] for row in last], ["run-1", "run-0"])
        self.assertFalse(more)
        ranked, _, _ = runs.page(2, 0, "", "", "sharpe", False)
        self.assertEqual([row["id"] for row in ranked], ["run-0", "run-1"])
        for search in ("dce", "首个"):
            found, more, _ = runs.page(2, 0, "", search, "started", False)
            self.assertEqual([row["id"] for row in found], ["run-0"])
            self.assertFalse(more)
        self.assertEqual([row["id"] for row in runs.recent()], ["run-4", "run-3", "run-2"])
        # An unreadable old result must not prevent a recent page or another run's report.
        (runs.root / "run-0" / "run.json").unlink()
        self.assertEqual(runs.get("run-4")["id"], "run-4")
        self.assertEqual(len(runs.page(2, 0, "", "", "started", False)[0]), 2)
        runs.index.close()

    def test_a_run_from_start_to_success(self):
        runs = Runs(self.directory)
        runs.start("run-1", self.plan)
        record = runs.record("run-1")
        self.assertEqual(record["strategy"]["commit"], "c" * 40)
        self.assertEqual(record["data"]["symbols"], ["SHFE/rb"])
        self.assertEqual(runs.list()[0]["state"], "running")
        tables = empty_tables()
        tables["equity"] = pl.DataFrame(
            {"time_ns": [1, 2], "equity": [Decimal("1000000.12345678"), Decimal("999999.5")],
             "event": ["mark", "settlement"]},
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
                         ("succeeded", {"total_return": -0.0000005, "sharpe": None, "fills": 0}))
        with self.assertRaises(ValueError):
            runs.end("run-1", "failed", "late")

    def test_runs_that_stop(self):
        runs = Runs(self.directory)
        runs.start("run-1", self.plan)
        runs.start("run-2", self.plan)
        runs.start("run-3", self.plan)
        runs.end("run-1", "failed", "data missing")
        runs.end("run-2", "interrupted", "")
        runs.end("run-3", "stopped", "")
        # A service restart must preserve an explicit owner stop.
        restarted = Runs(self.directory)
        restarted.interrupt()
        self.assertEqual({row["id"]: (row["state"], row["error"]) for row in restarted.list()},
                         {"run-1": ("failed", "data missing"), "run-2": ("interrupted", ""),
                          "run-3": ("stopped", "")})
        with self.assertRaises(ValueError):
            runs.start("run-1", self.plan)
        for identity in ("../x", "", "a b"):
            with self.assertRaises(ValueError):
                runs.start(identity, self.plan)
        with self.assertRaises(ValueError):
            runs.end("run-1", "succeeded", "")

    def test_liquid_symbols_excludes_all_nan_volume_without_warning(self):
        import warnings
        from asterion_strategy import Bar, Context, liquid_symbols, observe

        ctx = Context(["SHFE/rb", "DCE/i"])
        ts = datetime.datetime(2026, 1, 5, 15, 0)
        bars = observe(ctx, [
            Bar("SHFE/rb", "SHFE/rb/2026-05", ts, 3000.0, 3010.0, 2990.0, 3005.0, float("nan"), 1000.0),
            Bar("DCE/i", "DCE/i/2026-05", ts, 800.0, 810.0, 795.0, 805.0, 500.0, 1000.0),
        ])
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            eligible = liquid_symbols(ctx, bars, min_bars=1, min_volume=10.0)
        self.assertEqual(eligible, ["DCE/i"])
        self.assertEqual(caught, [])


if __name__ == "__main__":
    unittest.main()
