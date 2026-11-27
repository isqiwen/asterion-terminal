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

    def test_factor_defaults_use_full_prefix_in_standalone_and_combined_strategies(self):
        import numpy as np
        from asterion_strategy import CombinedStrategy, ts_ema

        def trend(close, span=252):
            return ts_ema(close, span)

        class Trend(Strategy):
            def ready(self, ctx):
                return True

            def on_bar(self, ctx, bar):
                self.length = len(ctx.history(bar.symbol, "close"))
                self.score = ctx.factor(trend, bar.symbol)
                ctx.weights({bar.symbol: 0.5 if np.isfinite(self.score) else 0.0})

        component = Trend()

        class Portfolio(CombinedStrategy):
            def components(self):
                return [(component, 1.0)]

        close = 100.0 + np.arange(300) * 0.1 + np.sin(np.arange(300) / 13)
        expected = trend(close)
        for strategy, probe in ((Trend(), None), (Portfolio(), component)):
            context = Context(["a"])
            probe = strategy if probe is None else probe
            scores = []
            for i, value in enumerate(close):
                wanted = step(strategy, context, [bar("a", i, float(value))])
                scores.append(probe.score)
                self.assertEqual(probe.length, i + 1)
                self.assertEqual(wanted, {"a": 0.5 if i >= 251 else 0.0})
            np.testing.assert_allclose(scores, expected)
            self.assertAlmostEqual(context.factor(trend, "a", None), expected[-1])
            self.assertAlmostEqual(context.factor(trend, "a", span=20), ts_ema(close, 20)[-1])
            self.assertNotAlmostEqual(context.factor(trend, "a", 252), expected[-1])

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
                          "composite.RiskParityPortfolio",
                          "fundamentals.InventoryCarryRank", "fundamentals.MemberFlowRank",
                          "learning.RandomFeatureForecast", "learning.RidgeForecast", "ma_cross.MaCross",
                          "momentum.Momentum", "momentum_rank.MomentumRank", "noisy.Noisy",
                          "oscillators.AtrBreakout", "oscillators.RsiReversion",
                          "pairs.KalmanPairs",
                          "panel.PanelRandomFeatureRank", "panel.PanelRidgeRank", "reversion.Reversion",
                          "risk_premia.BasisMomentumRank", "risk_premia.CarryRank",
                          "risk_premia.ContinuousMomentumRank",
                          "risk_premia.DownsideRiskRank", "risk_premia.DualMomentum",
                          "risk_premia.LowVolatilityRank", "risk_premia.MaximumReturnRank", "risk_premia.OpenInterestRank",
                          "risk_premia.SectorNeutralRank", "risk_premia.SkewnessRank", "risk_premia.TrendCarryRank",
                          "trend.MultiHorizonTrend", "trend.TurningPointMomentum", "trend.VolatilityTargetMomentum"])
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
        self.assertEqual(len(response.list_strategies.strategies), 32, response)
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

    def test_causal_operators_and_cross_sectional_hygiene(self):
        import numpy as np
        from asterion_strategy import (
            check_factor,
            decay_linear,
            orthogonalize,
            product_sector,
            sector_neutralize,
            ts_corr,
            ts_delta,
            ts_mean,
            ts_pct_change,
            ts_rank,
            ts_std,
            ts_zscore,
            winsorize_mad,
        )

        x = np.array([10.0, 20.0, 30.0, 20.0, 40.0, 50.0])
        y = np.array([1.0, 2.0, 3.0, 2.0, 4.0, 5.0])
        np.testing.assert_allclose(ts_mean(x, 3), [np.nan, np.nan, 20.0, 70 / 3, 30.0, 110 / 3], equal_nan=True)
        np.testing.assert_allclose(ts_std(x, 3)[-1], np.std([20.0, 40.0, 50.0], ddof=1))
        np.testing.assert_allclose(ts_delta(x, 2), [np.nan, np.nan, 20.0, 0.0, 10.0, 30.0], equal_nan=True)
        np.testing.assert_allclose(ts_pct_change(x, 1)[1:3], [1.0, 0.5])
        np.testing.assert_allclose(ts_rank(x, 3), [np.nan, np.nan, 1.0, 0.25, 1.0, 1.0], equal_nan=True)
        self.assertAlmostEqual(ts_zscore(np.array([5.0, 5.0, 5.0]), 3)[-1], 0.0)
        self.assertAlmostEqual(ts_corr(x, y, 4)[-1], 1.0)
        self.assertAlmostEqual(decay_linear(np.array([1.0, 2.0, 3.0]), 3)[-1], (1 + 4 + 9) / 6)

        for fn, args in (
            (lambda close: ts_mean(close, 3), {"close": x}),
            (lambda close: ts_std(close, 3), {"close": x}),
            (lambda close: ts_delta(close, 2), {"close": x}),
            (lambda close: ts_pct_change(close, 2), {"close": x}),
            (lambda close: ts_rank(close, 3), {"close": x}),
            (lambda close: ts_zscore(close, 3), {"close": x}),
            (lambda close, volume: ts_corr(close, volume, 3), {"close": x, "volume": y}),
            (lambda close: decay_linear(close, 3), {"close": x}),
        ):
            check_factor(fn, args, 3)

        self.assertEqual(product_sector("SHFE/rb"), "black")
        self.assertEqual(product_sector("SHFE/rb/2026-10"), "black")
        self.assertEqual(product_sector("CFFEX/IF"), "financial")
        self.assertEqual(product_sector("SHFE/unknown_prod"), "")

        clipped = winsorize_mad({"a": 1.0, "b": 2.0, "c": 3.0, "d": 4.0, "e": 100.0}, n=3.0)
        self.assertLess(clipped["e"], 20.0)
        self.assertEqual(clipped["b"], 2.0)

        neutral = sector_neutralize({"SHFE/rb": 10.0, "DCE/i": 20.0, "SHFE/cu": 100.0, "SHFE/al": 110.0})
        self.assertAlmostEqual(neutral["SHFE/rb"], -5.0)
        self.assertAlmostEqual(neutral["DCE/i"], 5.0)
        self.assertAlmostEqual(neutral["SHFE/cu"], -5.0)
        self.assertAlmostEqual(neutral["SHFE/al"], 5.0)

        # Target = 2 * ctrl + 3 + alpha; OLS residual removes the ctrl component cleanly.
        ctrl = {"a": 1.0, "b": 2.0, "c": 3.0, "d": 4.0}
        alpha = {"a": 1.0, "b": -1.0, "c": -1.0, "d": 1.0}
        target = {s: 2.0 * ctrl[s] + 3.0 + alpha[s] for s in ctrl}
        resid = orthogonalize(target, ctrl)
        for s in ctrl:
            self.assertAlmostEqual(resid[s], alpha[s], places=10)
        self.assertEqual(orthogonalize({"a": 1.0, "b": 2.0}, {"a": 1.0, "b": 2.0}), {})


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

    def test_listing_reads_the_index_without_touching_run_files_and_ignores_deleted_runs(self):
        runs = Runs(self.directory)
        for identity in ("kept", "deleted"):
            runs.start(identity, self.plan)
            runs.end(identity, "stopped", "")
        with patch.object(
            runs, "record", side_effect=AssertionError("list should not read run.json")
        ):
            self.assertEqual([row["id"] for row in runs.list()], ["deleted", "kept"])
        runs.delete(["deleted"])
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

    def test_recent_runs_keep_the_latest_success_and_leave_out_rolling_results(self):
        runs = Runs(self.directory)
        for identity in ("good", "first", "second", "third", "scan-1", "scan-rolling"):
            runs.start(identity, self.plan)
            runs.end(identity, "stopped", "")
        runs.index.execute("update runs set state = 'succeeded' where id in ('good', 'scan-rolling')")
        runs.plan_sweep("scan", [("scan-1", {})])
        self.assertEqual([row["id"] for row in runs.recent()], ["third", "second", "first", "good"])
        runs.index.close()

    def test_recent_runs_keep_the_latest_outcome_and_success_of_each_settings(self):
        runs = Runs(self.directory)
        other = replace(self.plan, costs={**self.plan.costs, "deposit": "2000000"})
        for identity, plan in (("old", self.plan), ("kept", self.plan), ("broken", self.plan),
                               ("a", other), ("b", other), ("c", other)):
            runs.start(identity, plan)
            runs.end(identity, "failed" if identity == "broken" else "stopped", "")
        runs.index.execute("update runs set state = 'succeeded' where id != 'broken'")
        self.assertEqual([row["id"] for row in runs.recent()], ["c", "b", "a", "broken", "kept"])
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

    def test_outdated_runs_schema_reports_clear_path(self):
        import sqlite3

        db = self.directory / "runs.sqlite"
        with sqlite3.connect(db) as con:
            con.execute("""
                create table runs (
                    id text primary key, strategy text not null, commit_id text not null,
                    changes text not null, dataset_version text not null, state text not null,
                    error text not null default '', started_ns integer not null,
                    finished_ns integer)""")
        runs = Runs(self.directory)
        runs.interrupt()
        with self.assertRaises(RuntimeError) as caught:
            runs.recent()
        self.assertEqual(str(caught.exception), f"lab runs database schema is outdated: {db}")

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

    def test_strategy_portfolio_primitives_and_vol_balanced_combiner(self):
        import numpy as np
        from asterion_strategy import (
            CombinedStrategy,
            cap_sector_weights,
            check_factor,
            drawdown_scale,
            smooth_weights,
            ts_ewma_vol,
            vol_target_weights,
        )

        # EWMA volatility is strictly causal.
        closes = 100.0 * np.exp(np.cumsum(np.sin(np.arange(35) * 0.4) * 0.015))
        check_factor(ts_ewma_vol, {"close": closes}, count=35, span=10)
        ewma = ts_ewma_vol(closes, span=10)
        self.assertTrue(np.isnan(ewma[9]))
        self.assertTrue(np.isfinite(ewma[10]) and ewma[10] > 0)

        # Volatility targeting scales inversely with risk, clips, and caps gross exposure.
        sized = vol_target_weights(
            {"SHFE/rb": 1.0, "SHFE/cu": 1.0, "DCE/i": -1.0},
            {"SHFE/rb": 0.10, "SHFE/cu": 0.40, "DCE/i": 0.20},
            target_vol=0.10,
            gross_weight=0.5,
            max_weight=0.30,
        )
        self.assertLessEqual(sum(abs(v) for v in sized.values()), 0.5 + 1e-8)
        self.assertGreater(sized["SHFE/rb"], sized["SHFE/cu"])
        self.assertLess(sized["DCE/i"], 0.0)

        # Sector capping bounds gross & net exposure per China futures sector without flipping sides.
        capped = cap_sector_weights(
            {"SHFE/rb": 0.30, "SHFE/hc": 0.30, "DCE/i": -0.10, "SHFE/cu": 0.25, "SHFE/au": 0.12},
            max_sector_gross=0.35,
            max_sector_net=0.15,
        )
        black_gross = abs(capped["SHFE/rb"]) + abs(capped["SHFE/hc"]) + abs(capped["DCE/i"])
        black_net = capped["SHFE/rb"] + capped["SHFE/hc"] + capped["DCE/i"]
        self.assertLessEqual(black_gross, 0.35 + 1e-7)
        self.assertLessEqual(abs(black_net), 0.15 + 1e-7)
        self.assertLess(capped["DCE/i"], 0.0)
        self.assertAlmostEqual(capped["SHFE/cu"], 0.15)
        self.assertAlmostEqual(capped["SHFE/au"], 0.12)

        # Smoothing suppresses tiny same-sign changes under deadband but allows closes and flips.
        sm = smooth_weights(
            {"a": 0.20, "b": 0.20, "c": 0.20},
            {"a": 0.22, "b": 0.0, "c": -0.20},
            alpha=0.5,
            deadband=0.05,
        )
        self.assertAlmostEqual(sm["a"], 0.20)
        self.assertAlmostEqual(sm["b"], 0.10)
        self.assertAlmostEqual(sm["c"], 0.0)

        # Drawdown scaling reduces risk linearly between soft and hard thresholds.
        self.assertAlmostEqual(drawdown_scale([100.0, 110.0, 108.0], soft_dd=0.05, hard_dd=0.15), 1.0)
        self.assertAlmostEqual(drawdown_scale([100.0, 100.0, 90.0], soft_dd=0.05, hard_dd=0.15, min_scale=0.2), 0.6)
        self.assertAlmostEqual(drawdown_scale([100.0, 80.0], soft_dd=0.05, hard_dd=0.15, min_scale=0.2), 0.2)

        # CombinedStrategy with vol_balance gives higher effective share to the lower-vol component.
        class LowVolSub(Strategy):
            def ready(self, ctx):
                return True

            def on_bars(self, ctx, bars):
                ctx.weights({"SHFE/rb": 0.2, "SHFE/cu": 0.0})

        class HighVolSub(Strategy):
            def ready(self, ctx):
                return True

            def on_bars(self, ctx, bars):
                ctx.weights({"SHFE/rb": 0.0, "SHFE/cu": 0.8})

        class Balanced(CombinedStrategy):
            vol_balance = True
            vol_window = 5
            max_sector_gross = 0.5
            smooth_alpha = 0.5

            def components(self):
                return [(LowVolSub(), 0.5), (HighVolSub(), 0.5)]

        ctx = Context(["SHFE/rb", "SHFE/cu"])
        comb = Balanced()
        rb_price, cu_price = 3000.0, 70000.0
        for step_idx in range(10):
            rb_price *= 1.0 + (0.002 if step_idx % 2 == 0 else -0.001)
            cu_price *= 1.0 + (0.03 if step_idx % 2 == 0 else -0.025)
            wanted = step(comb, ctx, [bar("SHFE/rb", step_idx, rb_price), bar("SHFE/cu", step_idx, cu_price)])
        self.assertEqual(len(comb.last_components), 2)
        self.assertGreater(comb.last_components[0]["share"], comb.last_components[1]["share"])
        self.assertLessEqual(abs(wanted["SHFE/rb"]), 0.5 + 1e-7)

    def test_bounded_context_compaction_roll_scaling_and_lazy_nan_columns(self):
        import numpy as np
        from asterion_strategy import CombinedStrategy, Context, Strategy, observe

        # Lazy NaN columns stay unallocated while only NaN values arrive.
        lazy_ctx = Context(["SHFE/rb"], max_history=16)
        for i in range(40):
            observe(lazy_ctx, [bar("SHFE/rb", i, 100.0 + i)])
        self.assertIsNone(lazy_ctx.columns["SHFE/rb"]["inventory_total"].values)
        self.assertEqual(lazy_ctx.columns["SHFE/rb"]["inventory_total"].nbytes, 0)
        nan_hist = lazy_ctx.history("SHFE/rb", "inventory_total")
        self.assertEqual(len(nan_hist), 16)
        self.assertTrue(np.all(np.isnan(nan_hist)))
        self.assertFalse(nan_hist.flags.writeable)

        # Bounded compaction matches unbounded history exactly on every step and across rolls.
        bounded = Context(["SHFE/rb"], rolls={"SHFE/rb/2026-09": 1.1}, max_history=5)
        unbounded = Context(["SHFE/rb"], rolls={"SHFE/rb/2026-09": 1.1})
        views = []
        for i in range(25):
            contract = "SHFE/rb/2026-05" if i < 14 else "SHFE/rb/2026-09"
            b = replace(bar("SHFE/rb", i, 100.0 + i), contract=contract)
            observe(bounded, [b])
            observe(unbounded, [b])
            b_last = bounded.history("SHFE/rb", "close")
            u_last = unbounded.history("SHFE/rb", "close", 5)
            np.testing.assert_allclose(b_last, u_last)
            np.testing.assert_allclose(bounded.history("SHFE/rb", "close", None), u_last)
            self.assertEqual(bounded.factor(lambda close: close.cumsum(), "SHFE/rb"), u_last.sum())
            full = unbounded.history("SHFE/rb", "close")
            self.assertEqual(len(full), i + 1)
            self.assertFalse(full.flags.writeable)
            self.assertFalse(b_last.flags.writeable)
            views.append((b_last, b_last.copy()))
            views.append((full, full.copy()))
        # Previous views remain untouched after compaction and roll scaling.
        for view, snapshot in views:
            np.testing.assert_allclose(view, snapshot)
        # Buffer never grows past 2 * max_history.
        self.assertLessEqual(len(bounded.columns["SHFE/rb"]["close"].values), 10)
        with self.assertRaisesRegex(ValueError, "exceeds history_limit"):
            bounded.history("SHFE/rb", "close", 6)

        # Strategy.history_limit validation and CombinedStrategy inheritance.
        with self.assertRaises(TypeError):
            class BadLimit(Strategy):
                history_limit = 0
                def on_bar(self, ctx, b): pass

        class SubA(Strategy):
            history_limit = 12
            def on_bars(self, ctx, bars): pass

        class SubB(Strategy):
            history_limit = 30
            def on_bars(self, ctx, bars): pass

        class BothBounded(CombinedStrategy):
            def components(self):
                return [(SubA(), 0.5), (SubB(), 0.5)]

        self.assertEqual(BothBounded().history_limit, 30)

    def test_ledoit_wolf_cov_hrp_weights_and_combined_hrp_allocation(self):
        import math
        import numpy as np
        from asterion_strategy import (
            CombinedStrategy,
            Context,
            Strategy,
            hrp_weights,
            ledoit_wolf_cov,
            step,
        )

        # Ledoit-Wolf shrinkage yields a symmetric strictly PD matrix even when T < N.
        rng = np.random.default_rng(7)
        small_sample = rng.normal(0.0, 0.02, size=(8, 12))
        sample_cov = np.cov(small_sample, rowvar=False, ddof=0)
        self.assertLessEqual(float(np.min(np.linalg.eigvalsh(sample_cov))), 1e-14)
        shrunk = ledoit_wolf_cov(small_sample)
        self.assertEqual(shrunk.shape, (12, 12))
        np.testing.assert_allclose(shrunk, shrunk.T, atol=1e-15)
        self.assertGreater(float(np.min(np.linalg.eigvalsh(shrunk))), 1e-8)
        with_nan = np.vstack([small_sample, np.full((1, 12), np.nan)])
        np.testing.assert_allclose(ledoit_wolf_cov(with_nan), shrunk, atol=1e-15)

        # HRP splits cluster risk between correlated duplicates (a1, a2) and uncorrelated b.
        t = np.arange(120, dtype=float)
        f1 = 0.015 * np.sin(t * 0.35)
        f2 = 0.015 * np.cos(t * 0.35)
        a1 = f1 + 0.0005 * np.sin(t * 1.7)
        a2 = f1 - 0.0005 * np.sin(t * 1.7)
        b = f2
        w_dict = hrp_weights({"a1": a1, "a2": a2, "b": b})
        w_arr = hrp_weights(np.column_stack([a1, a2, b]))
        np.testing.assert_allclose([w_dict["a1"], w_dict["a2"], w_dict["b"]], w_arr, atol=1e-12)
        self.assertAlmostEqual(sum(w_dict.values()), 1.0, places=12)
        self.assertTrue(all(v > 0.0 for v in w_dict.values()))
        self.assertGreater(w_dict["b"], 0.42)
        self.assertGreater(w_dict["b"], w_dict["a1"] * 1.4)
        self.assertGreater(w_dict["b"], w_dict["a2"] * 1.4)
        self.assertEqual(hrp_weights({"only": a1}), {"only": 1.0})
        w_raw = hrp_weights({"a1": a1, "a2": a2, "b": b}, shrinkage=False)
        self.assertGreater(w_raw["b"], w_raw["a1"] * 1.4)

        # CombinedStrategy with allocation="hrp" tilts away from correlated duplicate sub-strategies.
        class ConstSub(Strategy):
            params = {"symbol": "SHFE/rb"}

            def ready(self, ctx):
                return True

            def on_bars(self, ctx, bars):
                ctx.weights({self.params["symbol"]: 0.3})

        class HrpPortfolio(CombinedStrategy):
            allocation = "hrp"
            vol_window = 12

            def components(self):
                return [
                    (ConstSub(symbol="SHFE/rb"), 1.0 / 3.0),
                    (ConstSub(symbol="SHFE/hc"), 1.0 / 3.0),
                    (ConstSub(symbol="SHFE/au"), 1.0 / 3.0),
                ]

        ctx = Context(["SHFE/rb", "SHFE/hc", "SHFE/au"])
        comb = HrpPortfolio()
        rb, hc, au = 3000.0, 3100.0, 500.0
        for idx in range(30):
            r_black = 0.015 * math.sin(idx * 0.5)
            r_gold = 0.015 * math.cos(idx * 0.5)
            rb *= 1.0 + r_black + 0.0004 * math.sin(idx * 1.9)
            hc *= 1.0 + r_black - 0.0004 * math.sin(idx * 1.9)
            au *= 1.0 + r_gold
            step(comb, ctx, [
                bar("SHFE/rb", idx, rb),
                bar("SHFE/hc", idx, hc),
                bar("SHFE/au", idx, au),
            ])
        shares = [item["share"] for item in comb.last_components]
        self.assertAlmostEqual(sum(shares), 1.0, places=9)
        self.assertGreater(shares[2], shares[0] * 1.3)
        self.assertGreater(shares[2], shares[1] * 1.3)

        with self.assertRaisesRegex(ValueError, "allocation"):
            class BadAlloc(CombinedStrategy):
                allocation = "unknown"

                def components(self):
                    return [(ConstSub(symbol="SHFE/rb"), 1.0)]

            BadAlloc()

    def test_covariance_allocations_use_common_bar_returns_after_a_component_pauses(self):
        import numpy as np
        from asterion_strategy import CombinedStrategy, erc_weights, hrp_weights

        class Component(Strategy):
            params = {"symbol": "SHFE/rb", "pause_minute": -1}

            def ready(self, ctx):
                return ctx.time.minute != self.params["pause_minute"]

            def on_bars(self, ctx, bars):
                ctx.weights({self.params["symbol"]: 0.5})

        returns = np.array([
            [0.0, 0.0], [0.01, 0.04], [0.08, -0.03], [-0.04, 0.06],
            [0.15, -0.12], [0.02, -0.01], [-0.09, 0.03], [0.12, -0.05],
        ])
        for mode, allocate in (("hrp", hrp_weights), ("erc", erc_weights)):
            with self.subTest(allocation=mode):
                class Portfolio(CombinedStrategy):
                    allocation = mode
                    vol_window = 5

                    def components(self):
                        return [(Component(), 0.5), (Component(symbol="SHFE/au", pause_minute=3), 0.5)]

                strategy = Portfolio()
                context = Context(["SHFE/rb", "SHFE/au"])
                prices = np.array([100.0, 100.0])
                common = []
                for index, row in enumerate(returns):
                    prices *= 1 + row
                    step(strategy, context, [bar(symbol, index, price)
                                             for symbol, price in zip(context.symbols, prices)])
                    # The minute-3 pause leaves no second-component target for minute 4.
                    if index and index != 4:
                        common.append(row * 0.5)
                    expected = allocate(np.array(common[-5:]), shrinkage=True) \
                        if len(common) >= 5 else [0.5, 0.5]
                    np.testing.assert_allclose(
                        [item["share"] for item in strategy.last_components], expected, atol=1e-10,
                    )

    def test_causal_timeseries_kalman_and_microstructure_primitives(self):
        import math
        import numpy as np
        from asterion_strategy import (
            check_factor,
            corwin_schultz_spread,
            garman_klass_vol,
            kalman_filter,
            kalman_hedge,
            ou_half_life,
            parkinson_vol,
            ts_autocorr,
            ts_ema,
            ts_hurst,
            ts_kurt,
            ts_ols_beta,
            ts_ols_r2,
            ts_ols_resid,
            ts_skew,
            ts_variance_ratio,
            ts_vwap_dev,
            yang_zhang_vol,
        )

        rng = np.random.default_rng(20260401)
        n = 160
        rets = rng.normal(0.0003, 0.012, n)
        close = 100.0 * np.exp(np.cumsum(rets))
        opening = close * np.exp(rng.normal(0.0, 0.004, n))
        high = np.maximum(opening, close) * (1.0 + rng.uniform(0.002, 0.015, n))
        low = np.minimum(opening, close) * (1.0 - rng.uniform(0.002, 0.015, n))
        volume = rng.uniform(200.0, 4000.0, n)

        # Causal EMA tracks a step change smoothly and passes prefix causality.
        step_arr = np.concatenate([np.zeros(5), np.ones(10)])
        ema = ts_ema(step_arr, 3)
        self.assertTrue(np.isnan(ema[1]))
        self.assertAlmostEqual(ema[2], 0.0)
        self.assertGreater(ema[-1], 0.95)
        check_factor(lambda close: ts_ema(close, 5), {"close": close[:25]}, count=25)

        # Rolling OLS recovers exact linear slope, zero residual, and R^2 = 1.
        x = np.linspace(10.0, 30.0, 30)
        y = 2.5 * x - 4.0
        np.testing.assert_allclose(ts_ols_beta(y, x, 10)[9:], 2.5, atol=1e-12)
        np.testing.assert_allclose(ts_ols_resid(y, x, 10)[9:], 0.0, atol=1e-12)
        np.testing.assert_allclose(ts_ols_r2(y, x, 10)[9:], 1.0, atol=1e-12)
        check_factor(lambda close, far_close: ts_ols_beta(close, far_close, 8),
                     {"close": close[:25], "far_close": opening[:25]}, count=8)
        check_factor(lambda close, far_close: ts_ols_resid(close, far_close, 8),
                     {"close": close[:25], "far_close": opening[:25]}, count=8)
        check_factor(lambda close, far_close: ts_ols_r2(close, far_close, 8),
                     {"close": close[:25], "far_close": opening[:25]}, count=8)

        # 1D Kalman filter reduces observation noise and is strictly causal.
        noisy = 100.0 + rng.normal(0.0, 2.0, 60)
        filtered = kalman_filter(noisy, process_var=1e-4, obs_var=4.0)
        self.assertLess(float(np.std(filtered[10:])), float(np.std(noisy[10:])) * 0.5)
        check_factor(lambda close: kalman_filter(close, process_var=1e-3, obs_var=1.0),
                     {"close": close[:25]}, count=25)

        # 2-state Kalman hedge tracks a regime shift in beta and yields causal innovation spread.
        x_pair = 50.0 + np.cumsum(rng.normal(0.0, 1.0, 120))
        true_beta = np.where(np.arange(120) < 60, 1.5, 2.5)
        y_pair = true_beta * x_pair + 10.0 + rng.normal(0.0, 0.1, 120)
        kb, ki, ks = kalman_hedge(y_pair, x_pair, delta=1e-3, obs_var=0.05)
        self.assertTrue(np.isnan(ks[0]))
        self.assertAlmostEqual(float(np.mean(kb[30:55])), 1.5, delta=0.15)
        self.assertAlmostEqual(float(np.mean(kb[95:120])), 2.5, delta=0.20)
        self.assertTrue(np.all(np.isfinite(ki[2:])))
        check_factor(lambda close, far_close: kalman_hedge(close, far_close)[0],
                     {"close": y_pair[:25], "far_close": x_pair[:25]}, count=25)
        check_factor(lambda close, far_close: kalman_hedge(close, far_close)[2],
                     {"close": y_pair[:25], "far_close": x_pair[:25]}, count=25)

        # Higher-order moments & autocorrelation detect alternating vs persistent series.
        alt = np.array([1.0 if i % 2 == 0 else -1.0 for i in range(20)])
        self.assertAlmostEqual(float(ts_autocorr(alt, 10, lag=1)[-1]), -1.0, places=10)
        self.assertGreater(float(ts_skew(np.array([0.0, 0.0, 0.0, 0.0, 10.0]), 5)[-1]), 1.0)
        self.assertGreater(float(ts_kurt(np.array([0.0] * 18 + [10.0, -10.0]), 20)[-1]), 3.0)
        check_factor(lambda close: ts_autocorr(close, 8, lag=1), {"close": close[:25]}, count=8)
        check_factor(lambda close: ts_skew(close, 8), {"close": close[:25]}, count=8)
        check_factor(lambda close: ts_kurt(close, 8), {"close": close[:25]}, count=8)

        # Variance ratio & Hurst distinguish trending vs mean-reverting dynamics.
        mr = np.empty(180)
        mr[0] = 0.0
        for i in range(1, 180):
            mr[i] = 0.55 * mr[i - 1] + rng.normal(0.0, 0.02)
        mr_price = 100.0 * np.exp(mr)
        tr_inc = np.empty(180)
        tr_inc[0] = 0.005
        for i in range(1, 180):
            tr_inc[i] = 0.7 * tr_inc[i - 1] + rng.normal(0.0, 0.005)
        tr_price = 100.0 * np.exp(np.cumsum(tr_inc))
        self.assertLess(float(ts_variance_ratio(mr_price, 60, q=5)[-1]), 0.75)
        self.assertGreater(float(ts_variance_ratio(tr_price, 60, q=5)[-1]), 1.5)
        self.assertLess(float(ts_hurst(mr_price, window=120, max_lag=15)[-1]), 0.35)
        self.assertGreater(float(ts_hurst(tr_price, window=120, max_lag=15)[-1]), 0.65)
        check_factor(lambda close: ts_variance_ratio(close, 12, q=3), {"close": close[:30]}, count=13)
        check_factor(lambda close: ts_hurst(close, window=20, max_lag=8), {"close": close[:35]}, count=20)

        # Ornstein-Uhlenbeck half-life matches theoretical -ln(2) / ln(phi) on AR(1) and NaN on trend.
        phi = 0.8
        ou = np.empty(200)
        ou[0] = 0.0
        for i in range(1, 200):
            ou[i] = phi * ou[i - 1] + rng.normal(0.0, 0.05)
        hl = ou_half_life(ou, window=120)
        expected_hl = -math.log(2.0) / math.log(phi)
        self.assertAlmostEqual(float(hl[-1]), expected_hl, delta=1.5)
        self.assertTrue(np.isnan(ou_half_life(np.arange(1.0, 40.0), window=20)[-1]))
        check_factor(lambda close: ou_half_life(close, window=15), {"close": mr_price[:30]}, count=16)

        # OHLC range volatilities, Corwin-Schultz spread, and VWAP deviation are causal and positive.
        pv = parkinson_vol(high, low, window=20, annualize=252.0)
        gk = garman_klass_vol(opening, high, low, close, window=20, annualize=252.0)
        yz = yang_zhang_vol(opening, high, low, close, window=20, annualize=252.0)
        self.assertTrue(np.isfinite(pv[-1]) and pv[-1] > 0.0)
        self.assertTrue(np.isfinite(gk[-1]) and gk[-1] > 0.0)
        self.assertTrue(np.isfinite(yz[-1]) and yz[-1] > 0.0)
        check_factor(lambda high, low: parkinson_vol(high, low, window=8),
                     {"high": high[:25], "low": low[:25]}, count=8)
        check_factor(lambda open, high, low, close: garman_klass_vol(open, high, low, close, window=8),
                     {"open": opening[:25], "high": high[:25], "low": low[:25], "close": close[:25]}, count=8)
        check_factor(lambda open, high, low, close: yang_zhang_vol(open, high, low, close, window=8),
                     {"open": opening[:25], "high": high[:25], "low": low[:25], "close": close[:25]}, count=9)

        # Wider symmetric bid-ask spread produces strictly larger Corwin-Schultz spread estimate.
        mid = 100.0 * np.exp(np.cumsum(rng.normal(0.0, 0.005, 80)))
        tight_h, tight_l = mid * 1.003, mid * 0.997
        wide_h, wide_l = mid * 1.015, mid * 0.985
        cs_tight = corwin_schultz_spread(tight_h, tight_l, window=20)
        cs_wide = corwin_schultz_spread(wide_h, wide_l, window=20)
        self.assertGreater(float(cs_wide[-1]), float(cs_tight[-1]) + 0.005)
        check_factor(lambda high, low: corwin_schultz_spread(high, low, window=5),
                     {"high": high[:25], "low": low[:25]}, count=6)

        # VWAP deviation equals exact close / VWAP - 1.
        c_v = np.array([100.0, 110.0, 120.0])
        v_v = np.array([1.0, 1.0, 2.0])
        expected_vwap = (100.0 * 1.0 + 110.0 * 1.0 + 120.0 * 2.0) / 4.0
        self.assertAlmostEqual(float(ts_vwap_dev(c_v, v_v, window=3)[-1]), 120.0 / expected_vwap - 1.0)
        check_factor(lambda close, volume: ts_vwap_dev(close, volume, window=6),
                     {"close": close[:25], "volume": volume[:25]}, count=6)

    def test_erc_weights_combined_erc_and_turnover_aware_mvo_weights(self):
        import math
        import numpy as np
        from asterion_strategy import (
            CombinedStrategy,
            Context,
            Strategy,
            erc_weights,
            mvo_weights,
            step,
        )

        # ERC on a known covariance matrix equalizes marginal risk contributions w_i (Sigma w)_i / V = 1/N.
        cov = np.array([
            [0.04, 0.015, 0.002],
            [0.015, 0.09, 0.004],
            [0.002, 0.004, 0.01],
        ])
        w_erc = erc_weights(cov, is_cov=True)
        self.assertAlmostEqual(float(np.sum(w_erc)), 1.0, places=10)
        self.assertTrue(np.all(w_erc > 0.0))
        rc = (w_erc * (cov @ w_erc)) / float(w_erc @ cov @ w_erc)
        np.testing.assert_allclose(rc, np.full(3, 1.0 / 3.0), atol=1e-6)

        # Box-constrained ERC caps the dominant low-vol asset while keeping sum == 1.
        w_cap = erc_weights(cov, is_cov=True, max_weight=0.45)
        self.assertAlmostEqual(float(np.sum(w_cap)), 1.0, places=9)
        self.assertLessEqual(float(np.max(w_cap)), 0.45 + 1e-8)

        # Dict returns input with Ledoit-Wolf shrinkage and CombinedStrategy(allocation="erc").
        t = np.arange(80, dtype=float)
        r_low = 0.005 * np.sin(t * 0.4)
        r_high = 0.03 * np.cos(t * 0.4)
        w_dict = erc_weights({"low": r_low, "high": r_high})
        self.assertAlmostEqual(sum(w_dict.values()), 1.0, places=9)
        self.assertGreater(w_dict["low"], w_dict["high"] * 3.0)

        class ConstSub(Strategy):
            params = {"symbol": "SHFE/rb"}

            def ready(self, ctx):
                return True

            def on_bars(self, ctx, bars):
                ctx.weights({self.params["symbol"]: 0.5})

        class ErcPortfolio(CombinedStrategy):
            allocation = "erc"
            vol_window = 10

            def components(self):
                return [
                    (ConstSub(symbol="SHFE/rb"), 0.5),
                    (ConstSub(symbol="SHFE/au"), 0.5),
                ]

        ctx = Context(["SHFE/rb", "SHFE/au"])
        comb = ErcPortfolio()
        rb, au = 3000.0, 500.0
        for idx in range(25):
            rb *= 1.0 + 0.025 * math.sin(idx * 0.6)
            au *= 1.0 + 0.005 * math.cos(idx * 0.6)
            step(comb, ctx, [bar("SHFE/rb", idx, rb), bar("SHFE/au", idx, au)])
        shares = [item["share"] for item in comb.last_components]
        self.assertAlmostEqual(sum(shares), 1.0, places=9)
        self.assertGreater(shares[1], shares[0] * 2.0)

        # MVO respects gross, net, max_weight, long_only, and China futures sector gross limits.
        mu = {"SHFE/rb": 0.12, "SHFE/hc": 0.10, "SHFE/cu": -0.09, "SHFE/au": 0.04}
        mvo_cov = np.diag([0.04, 0.04, 0.04, 0.02])
        w_ls = mvo_weights(
            mu,
            mvo_cov,
            risk_aversion=2.0,
            gross_limit=0.80,
            net_limit=0.20,
            max_weight=0.35,
            max_sector_gross=0.40,
        )
        self.assertLessEqual(sum(abs(v) for v in w_ls.values()), 0.80 + 1e-7)
        self.assertLessEqual(abs(sum(w_ls.values())), 0.20 + 1e-7)
        self.assertLessEqual(max(abs(v) for v in w_ls.values()), 0.35 + 1e-7)
        # SHFE/rb + SHFE/hc belong to the 'black' sector.
        self.assertLessEqual(abs(w_ls["SHFE/rb"]) + abs(w_ls["SHFE/hc"]), 0.40 + 1e-7)
        self.assertGreater(w_ls["SHFE/rb"], 0.0)
        self.assertLess(w_ls["SHFE/cu"], 0.0)

        w_long = mvo_weights(mu, mvo_cov, risk_aversion=2.0, gross_limit=0.80, max_weight=0.35, long_only=True)
        self.assertTrue(all(v >= 0.0 for v in w_long.values()))
        self.assertEqual(w_long["SHFE/cu"], 0.0)

        # Turnover cost monotonically reduces L1 trade distance from previous weights.
        prev = {"SHFE/rb": 0.15, "SHFE/hc": 0.15, "SHFE/cu": 0.10, "SHFE/au": -0.10}
        w_free = mvo_weights(mu, mvo_cov, previous_weights=prev, risk_aversion=10.0, turnover_cost=0.0, gross_limit=1.0, max_weight=0.4)
        w_mid = mvo_weights(mu, mvo_cov, previous_weights=prev, risk_aversion=10.0, turnover_cost=0.03, gross_limit=1.0, max_weight=0.4)
        w_high = mvo_weights(mu, mvo_cov, previous_weights=prev, risk_aversion=10.0, turnover_cost=0.5, gross_limit=1.0, max_weight=0.4)
        def dist(a, b):
            return sum(abs(a[k] - b[k]) for k in prev)

        self.assertGreater(dist(w_free, prev), dist(w_mid, prev))
        self.assertGreater(dist(w_mid, prev), dist(w_high, prev))
        self.assertLess(dist(w_high, prev), 1e-5)


if __name__ == "__main__":
    unittest.main()
