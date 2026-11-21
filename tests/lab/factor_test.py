"""Factors: found in the strategy repository, evaluated across symbols.

Run with the bundled interpreter:
python3 -I -B factor_test.py <source> <generated-python> <engine-directory>
"""
import asyncio
from decimal import Decimal
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "apps/services/common/python"))
import tempfile
import unittest

sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1), sys.argv.pop(1), str(Path(__file__).parent)]
import numpy as np  # noqa: E402
import polars as pl  # noqa: E402
from asterion.lab_pb2 import Request  # noqa: E402
from asterion_lab import factor, repository  # noqa: E402
from asterion_strategy import check_factor  # noqa: E402
from service_fixture import Service  # noqa: E402
from warehouse_writer import Warehouse  # noqa: E402
from warehouse_fixture import DAYS, warehouse  # noqa: E402

EXAMPLES = Path(__file__).resolve().parents[2] / "research"


def git(root, *arguments):
    subprocess.run(["git", "-C", str(root), "-c", "user.name=test", "-c", "user.email=test@invalid",
                    *arguments], check=True, capture_output=True)


def frame(values, rates, bars=5):
    """Symbols s1.. whose closes grow by `rates` each bar and whose factor is `values`."""
    rows = []
    for symbol, (value, rate) in enumerate(zip(values, rates), 1):
        for bar in range(bars):
            rows.append(dict(symbol=f"s{symbol}", day=f"2026-01-{bar + 1:02d}", bar=bar,
                             close=100 * (1 + rate) ** bar, value=float(value)))
    return pl.DataFrame(rows)


class Evaluation(unittest.TestCase):
    def test_factor_check_identifies_future_dependent_values(self):
        with self.assertRaisesRegex(ValueError, "changes historical values"):
            check_factor(lambda close: np.roll(close, -1), {"close": [100, 110, 120]}, 3)

    def test_factor_check_identifies_an_insufficient_history_window(self):
        with self.assertRaisesRegex(ValueError, "history window 2 differs"):
            check_factor(lambda close: close / close[0] - 1, {"close": [100, 110, 120]}, 2)

    def test_correlations_distinguish_missing_samples_from_zero_correlation(self):
        few = frame([1, 2], [0.01, 0.02])
        constant = frame([1, 1, 1, 1], [0.01] * 4)
        for rows in (few, constant):
            matrix = factor.factor_correlations([rows, rows])
            self.assertEqual(matrix, [{"days": 0, "samples": 0}] * 4)
        first = frame([1, 2, 3, 4], [0.01] * 4)
        second = frame([2, 4, 1, 3], [0.01] * 4)
        matrix = factor.factor_correlations([first, second])
        self.assertAlmostEqual(matrix[1]["value"], 0.0)
        self.assertEqual((matrix[1]["days"], matrix[1]["samples"]), (5, 20))
        self.assertEqual(matrix[1], matrix[2])

    def test_development_ic_uses_only_closes_before_holdout(self):
        rows = frame([1, 2, 3], [0.01, 0.02, 0.03], bars=6)
        before = factor.evaluate(rows, [1, 2], 2, "2026-01-04")
        changed = rows.with_columns(
            pl.when(pl.col("day") == "2026-01-04").then(200 - pl.col("close"))
            .otherwise(pl.col("close")).alias("close"))
        after = factor.evaluate(changed, [1, 2], 2, "2026-01-04")
        self.assertEqual([row["days"] for row in before["development_horizons"]], [2, 1])
        self.assertEqual(before["development_horizons"], after["development_horizons"])

    def test_groups_hold_one_bar_and_t_counts_overlapping_horizons_once(self):
        # Every close grows 1% a bar: ten bars return 10.46% in every group.
        closes = frame([1, 2, 3], [0.01, 0.01, 0.01], bars=11)
        for horizons in ([1], [5]):
            result = factor.evaluate(closes, horizons, 2)
            last = [day["groups"] for day in result["days"] if day["groups"]][-1]
            self.assertAlmostEqual(last[0] - 1, 1.01 ** 10 - 1)
        ic = np.array([0.1, 0.3, -0.2, 0.4, 0.0, 0.2])
        one, five = factor.statistics(1, ic), factor.statistics(5, ic)
        self.assertAlmostEqual(one["t"], one["ir"] * 6 ** 0.5)
        self.assertAlmostEqual(five["t"], five["ir"] * (6 / 5) ** 0.5)

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

    def test_segments_keep_point_in_time_prices_and_returns_align_by_trading_day(self):
        # Segment 0 sees unadjusted [100, 110]; segment 1 sees [120, 132, 135].
        symbols = {"s": {"day": ["2026-01-01", "2026-01-02", "2026-01-03"],
                         "close": np.array([120.0, 132.0, 135.0]),
                         "segments": [(0, {"close": np.array([100.0, 110.0])}),
                                      (2, {"close": np.array([120.0, 132.0, 135.0])})]}}
        evaluated = factor.values(lambda close: close, ["close"], {}, symbols)
        self.assertEqual(evaluated["value"].to_list(), [100.0, 110.0, 135.0])
        # s2 misses 2026-01-02: its 1-day return on 2026-01-01 is absent rather than taken from 2026-01-03.
        gapped = frame([1, 2, 3], [0.01, 0.02, 0.03], bars=3).filter(
            ~((pl.col("symbol") == "s2") & (pl.col("day") == "2026-01-02")))
        result = factor.evaluate(gapped, [1, 2], 2)
        self.assertEqual(result["horizons"][0]["days"], 0)
        self.assertEqual(result["horizons"][1]["days"], 1)
        self.assertAlmostEqual(result["horizons"][1]["mean"], 1.0)


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
        self.assertEqual(sorted(entries), [
            "liquidity.chaikin_money_flow", "liquidity.money_flow_index", "liquidity.relative_volume",
            "liquidity.return_per_volume",
            "liquidity.turnover", "liquidity.volume_return_correlation", "liquidity.volume_weighted_return",
            "positions.interest_change", "price.blended_momentum", "price.distance_to_high",
            "price.information_discreteness", "price.momentum", "price.momentum_strength",
            "price.return_autocorrelation", "price.reversal", "price.skip_momentum", "price.variance_ratio", "price.volatility",
            "risk.downside_volatility", "risk.excess_kurtosis", "risk.expected_shortfall",
            "risk.garman_klass_volatility", "risk.maximum_return", "risk.parkinson_volatility", "risk.realized_skewness",
            "risk.rogers_satchell_volatility", "risk.semivariance_balance", "risk.upside_volatility",
            "risk.yang_zhang_volatility",
            "technical.average_true_range", "technical.channel_position", "technical.commodity_channel_index",
            "technical.efficiency_ratio", "technical.log_price_slope", "technical.moving_average_gap",
            "technical.price_zscore", "technical.rsi", "technical.stochastic",
            "term_structure.carry_to_risk", "term_structure.carry_zscore",
            "term_structure.roll_yield_carry", "term_structure.roll_yield_momentum"])
        momentum = entries["price.momentum"]
        self.assertEqual(list(momentum.inputs), ["close"])
        self.assertEqual([(item.name, item.default.integer) for item in momentum.parameters],
                         [("lookback", 20)])
        carry = entries["term_structure.roll_yield_carry"]
        self.assertEqual(list(carry.inputs), ["roll_yield"])
        self.assertEqual([(item.name, item.default.integer) for item in carry.parameters],
                         [("window", 5)])
        (error,) = listed.errors
        self.assertEqual(error.module, "factors/broken")
        self.assertIn("parameter window is no bar field", error.traceback)

    def test_research_rejects_factor_dependencies_unavailable_to_trading(self):
        (self.repository / "factors/frames.py").write_text(
            "import polars\n\ndef signal(close):\n    return close\n")
        (self.repository / "strategies/frames.py").write_text(
            "from asterion_strategy import Strategy\nfrom factors.frames import signal\n"
            "class UsesFrames(Strategy):\n    def on_bar(self, ctx, bar):\n        pass\n")
        listed = repository.list_strategies(str(self.repository))
        self.assertNotIn("frames.signal", [entry.id for entry in listed.factors])
        self.assertNotIn("frames.UsesFrames", [entry.id for entry in listed.strategies])
        for module in ("frames", "factors/frames"):
            self.assertIn("imports polars", next(error.traceback for error in listed.errors
                                                 if error.module == module))

        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            request = Request(id=1)
            run = request.evaluate_factor
            run.id, run.repository, run.factor = "unsupported", str(self.repository), "frames.signal"
            run.symbols.append("SHFE/rb")
            run.first_day, run.last_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
            run.horizons.append(1)
            run.groups = 2
            response = await service.reply(request)
            self.assertIn("imports polars", response.error.message)

        asyncio.run(scenario())

    def test_the_service_evaluates_a_factor_on_what_a_strategy_sees(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        async def scenario():
            service = Service("lab", self.directory, Log())
            request = Request(id=1)
            run = request.evaluate_factor
            run.id = "momentum-1"
            run.repository, run.factor = str(self.repository), "price.momentum"
            run.params["lookback"].integer = 2
            run.symbols.extend(["SHFE/rb/2026-05", "SHFE/rb/2026-10", "SHFE/rb"])
            run.first_day, run.last_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
            run.horizons.extend([1, 3])
            run.groups = 3
            reply = await service.reply(request)
            self.assertEqual(reply.WhichOneof("result"), "evaluate_factor", reply)
            result = reply.evaluate_factor
            self.assertEqual(list(result.symbols), list(run.symbols))
            self.assertEqual([item.horizon for item in result.horizons], [1, 3])
            self.assertEqual(len(result.days), len(DAYS))
            # The series begins a day later, when the open interest names its month;
            # from its third bar on every symbol has a value.
            self.assertEqual([day.symbols for day in result.days[:4]], [0, 0, 2, 3])
            self.assertGreater(result.horizons[0].days, 0)
            self.assertEqual(len(result.groups), 3)
            self.assertEqual(len(result.days[-1].groups), 3)
            self.assertIn("used", (await service.reply(request)).error.message)
            run.id = "momentum-2"
            run.params["lookback"].integer = 0
            failed = await service.reply(request)
            self.assertIn("lookback is at least one bar", failed.error.message)
            self.assertEqual(list((self.directory / "sources").glob("factor-momentum-2-*")), [])
            # The result keeps the code it evaluated; a later edit is a later evaluation's.
            module = self.repository / "factors" / "price.py"
            evaluated = module.read_text()
            module.write_text(evaluated + "\n# edited\n")
            # Factor evaluation places no orders and needs no price_ticks coverage.
            writer = Warehouse(self.directory).writer()
            writer.put_rows("price_ticks/SHFE", "price_ticks", [])
            writer.commit()
            run.id = "momentum-3"
            run.params.clear()
            run.symbols[:] = ["SHFE"]
            exchange = (await service.reply(request)).evaluate_factor
            kept = (await service.reply(Request(id=2, get_factor_result={"id": "momentum-1"})))
            self.assertEqual(kept.get_factor_result.code, evaluated)
            self.assertEqual(kept.get_factor_result.evaluation.factor, "price.momentum")
            self.assertEqual(kept.get_factor_result, result)
            self.assertTrue(exchange.code.endswith("# edited\n"))
            listed = (await service.reply(Request(id=3, list_factor_results={}))).list_factor_results
            self.assertEqual([item.id for item in listed.results], ["momentum-3", "momentum-1"])
            self.assertFalse(listed.results[0].days)
            self.assertEqual(len(listed.results[1].groups), 3)
            self.assertEqual(list(exchange.symbols), ["SHFE/rb"])
            self.assertFalse(exchange.skipped)
            self.assertEqual(exchange.pinned.dataset_version,
                             Warehouse(self.directory).current().identity)

        asyncio.run(scenario())

    def test_exchange_panel_reads_parquet_tables_once_and_prepares_each_product_once(self):
        from unittest.mock import patch
        from asterion_lab.backtest import Data
        from warehouse_fixture import closes, contract_row, daily

        products = [f"p{i:02d}" for i in range(20)]
        writer = Warehouse(self.directory).writer()
        contracts = []
        jan, feb = [], []
        for idx, product in enumerate(products):
            m1, m2 = f"SHFE/{product}/2026-05", f"SHFE/{product}/2026-10"
            contracts.extend([contract_row(m1), contract_row(m2)])
            for day_i, (day, base) in enumerate(zip(DAYS, closes(len(DAYS)))):
                price = base + idx * 10
                target = jan if day.isoformat() < "2026-02" else feb
                target.append(daily(m1, day, price, 1000 if day_i < 15 else 100))
                target.append(daily(m2, day, price + 20, 500 if day_i < 15 else 2000))
        # A single-month product is skipped without failing the exchange panel.
        contracts.append(contract_row("SHFE/solo/2026-05"))
        for day, base in zip(DAYS, closes(len(DAYS))):
            (jan if day.isoformat() < "2026-02" else feb).append(
                daily("SHFE/solo/2026-05", day, base, 300))
        writer.put_rows("contracts/SHFE", "contracts", contracts)
        writer.add_days("SHFE", "2026-01", jan,
                        [day.isoformat() for day in DAYS if day.isoformat() < "2026-02"])
        writer.add_days("SHFE", "2026-02", feb,
                        [day.isoformat() for day in DAYS if day.isoformat() >= "2026-02"])
        version = writer.commit().identity

        store = Warehouse(self.directory)
        queries, real_query = [], store.query
        def count_query(*args, **kwargs):
            queries.append(args[2])
            return real_query(*args, **kwargs)
        prepares, real_prepare = [], factor.prepare
        def count_prepare(*args, **kwargs):
            prepares.append(args[1].symbols)
            return real_prepare(*args, **kwargs)
        with patch.object(store, "query", count_query), patch.object(factor, "prepare", count_prepare):
            data = Data(store, DAYS[0].isoformat(), DAYS[-1].isoformat(), version=version)
            prepared = {}
            chosen, skipped = factor.symbols_of(data, ["SHFE"], prepared)
            panel = {symbol: factor.bars(data, symbol, prepared[symbol]) for symbol in chosen}
        self.assertEqual(len(chosen), 20)
        self.assertEqual([item[0] for item in skipped], ["SHFE/solo"])
        # Calendar once, daily once, contract lives once, contract units once.
        self.assertLessEqual(len(queries), 4, queries)
        # 20 valid products + 1 skipped product, each prepared once instead of twice.
        self.assertEqual(len(prepares), 21)
        function, inputs, defaults = repository.factor(self.repository, "price.momentum")
        evaluated = factor.evaluate(
            factor.values(function, inputs, {"lookback": 3}, panel), [1, 3], 4)
        self.assertGreater(evaluated["horizons"][0]["days"], 20)

    def test_multi_factor_correlation_combination_standardization_and_holdout(self):
        from asterion.lab_pb2 import Request

        class Log:
            def write(self, *args, **kwargs):
                pass

        products = [f"q{i:02d}" for i in range(6)]
        writer = Warehouse(self.directory).writer()
        contracts, jan, feb = [], [], []
        from warehouse_fixture import closes, contract_row, daily
        for idx, product in enumerate(products):
            m1, m2 = f"SHFE/{product}/2026-05", f"SHFE/{product}/2026-10"
            contracts.extend([contract_row(m1), contract_row(m2)])
            for day_i, (day, base) in enumerate(zip(DAYS, closes(len(DAYS)))):
                price = base + idx * (15 + day_i)
                target = jan if day.isoformat() < "2026-02" else feb
                target.append(daily(m1, day, price, 1000 + idx * 50 + day_i * 10 if day_i < 15 else 100))
                target.append(daily(m2, day, price + 20, 500 if day_i < 15 else 2000 + idx * 60 + day_i * 10))
        writer.put_rows("contracts/SHFE", "contracts", contracts)
        writer.add_days("SHFE", "2026-01", jan,
                        [day.isoformat() for day in DAYS if day.isoformat() < "2026-02"])
        writer.add_days("SHFE", "2026-02", feb,
                        [day.isoformat() for day in DAYS if day.isoformat() >= "2026-02"])
        writer.commit()

        async def scenario():
            service = Service("lab", self.directory, Log())
            req = Request(id=1)
            ev = req.evaluate_factor
            ev.id = "multi-ic-holdout"
            ev.repository = str(self.repository)
            ev.factor = "price.momentum"
            ev.params["lookback"].integer = 3
            ev.extra_factors.add(factor="price.reversal")
            ev.extra_factors.add(factor="positions.interest_change")
            ev.combination = "ic_weighted"
            ev.standardize = "rank"
            ev.holdout_day = DAYS[18].isoformat()
            ev.symbols.extend([f"SHFE/{p}" for p in products])
            ev.first_day, ev.last_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
            ev.horizons.extend([1, 3])
            ev.groups = 3
            res = (await service.reply(req)).evaluate_factor
            self.assertEqual(list(res.factor_names),
                             ["price.momentum", "price.reversal", "positions.interest_change"])
            self.assertEqual(len(res.correlations), 9)
            self.assertAlmostEqual(res.correlations[0].value, 1.0)
            self.assertAlmostEqual(res.correlations[4].value, 1.0)
            self.assertAlmostEqual(res.correlations[8].value, 1.0)
            # momentum and reversal (-momentum) have -1 rank correlation
            self.assertAlmostEqual(res.correlations[1].value, -1.0, places=5)
            self.assertEqual(len(res.development_horizons), 2)
            self.assertEqual(len(res.holdout_horizons), 2)
            self.assertGreater(res.development_horizons[0].days, 0)
            self.assertGreater(res.holdout_horizons[0].days, 0)
            self.assertIn("def interest_change", res.code)
            listed = (await service.reply(Request(id=2, list_factor_results={}))).list_factor_results
            summary = next(item for item in listed.results if item.id == "multi-ic-holdout")
            self.assertFalse(summary.days)
            self.assertFalse(summary.correlations)
            self.assertEqual(len(summary.groups), 3)
            self.assertEqual(len(summary.holdout_horizons), 2)
            self.assertEqual(list(summary.factor_names),
                             ["price.momentum", "price.reversal", "positions.interest_change"])

        asyncio.run(scenario())

    def test_term_structure_roll_yield_factors_evaluate_across_dominant_series(self):
        class Log:
            def write(self, *args, **kwargs):
                pass

        # 4 products with distinct backwardation/contango slopes that predict forward returns.
        writer = Warehouse(self.directory).writer()
        contracts, jan, feb = [], [], []
        from warehouse_fixture import contract_row, daily
        specs = [
            ("c0", 1.04, 0.008),   # strong backwardation -> rises fastest
            ("c1", 1.01, 0.004),   # mild backwardation
            ("c2", 0.99, -0.002),  # mild contango
            ("c3", 0.96, -0.006),  # strong contango -> falls fastest
        ]
        for product, near_far_ratio, daily_drift in specs:
            m1, m2, m3 = (
                f"SHFE/{product}/2026-05",
                f"SHFE/{product}/2026-10",
                f"SHFE/{product}/2027-01",
            )
            contracts.extend([contract_row(m1), contract_row(m2), contract_row(m3)])
            for day_i, day in enumerate(DAYS):
                near = Decimal(f"{3000 * ((1 + daily_drift) ** day_i):.2f}")
                mid = Decimal(f"{float(near) / near_far_ratio:.2f}")
                far = Decimal(f"{float(mid) / near_far_ratio:.2f}")
                target = jan if day.isoformat() < "2026-02" else feb
                target.append(daily(m1, day, near, 5000 if day_i < 15 else 200))
                target.append(daily(m2, day, mid, 2000 if day_i < 15 else 6000))
                target.append(daily(m3, day, far, 500 if day_i < 15 else 1500))
        writer.put_rows("contracts/SHFE", "contracts", contracts)
        writer.add_days("SHFE", "2026-01", jan,
                        [day.isoformat() for day in DAYS if day.isoformat() < "2026-02"])
        writer.add_days("SHFE", "2026-02", feb,
                        [day.isoformat() for day in DAYS if day.isoformat() >= "2026-02"])
        writer.commit()

        async def scenario():
            service = Service("lab", self.directory, Log())
            req = Request(id=1)
            ev = req.evaluate_factor
            ev.id = "carry-1"
            ev.repository = str(self.repository)
            ev.factor = "term_structure.roll_yield_carry"
            ev.params["window"].integer = 3
            ev.symbols.extend([f"SHFE/{p}" for p, _, _ in specs])
            ev.first_day, ev.last_day = DAYS[0].isoformat(), DAYS[-1].isoformat()
            ev.horizons.extend([1, 3])
            ev.groups = 2
            reply = await service.reply(req)
            self.assertEqual(reply.WhichOneof("result"), "evaluate_factor", reply)
            res = reply.evaluate_factor
            self.assertGreater(res.horizons[0].days, 10)
            # Higher backwardation (roll_yield) matches higher forward drift -> positive IC = 1.0.
            self.assertAlmostEqual(res.horizons[0].mean, 1.0, places=5)
            # Top carry group outperforms bottom carry group.
            self.assertGreater(res.groups[1].mean_return, res.groups[0].mean_return)

        asyncio.run(scenario())

    def test_ic_weighted_combine_normalizes_missing_factor_weights(self):
        import polars as pl
        from asterion_lab import factor as factor_mod

        rows = []
        # 5 days x 4 symbols: f1 predicts forward close perfectly (+1), f2 is inverse (-1) on sym D only.
        for day_i in range(5):
            day = f"2026-01-{day_i + 1:02d}"
            for s_idx, sym in enumerate(("A", "B", "C", "D")):
                close = 100.0 * ((1.0 + 0.01 * (s_idx + 1)) ** day_i)
                rows.append((sym, day, day_i, close, float(s_idx + 1), None if sym == "D" else float(s_idx + 1)))
        f1 = pl.DataFrame(
            [(r[0], r[1], r[2], r[3], r[4]) for r in rows],
            schema=["symbol", "day", "bar", "close", "value"],
            orient="row",
        )
        f2 = pl.DataFrame(
            [(r[0], r[1], r[2], r[3], r[5]) for r in rows],
            schema=["symbol", "day", "bar", "close", "value"],
            orient="row",
        )
        combined = factor_mod.combine_factors([f1, f2], "ic_weighted", horizon=1)
        last_day = combined.filter(pl.col("day") == "2026-01-05").sort("symbol")
        values = dict(zip(last_day["symbol"].to_list(), last_day["value"].to_list()))
        # Symbol D is missing f2; after dividing by den = |w1|, its score equals the full z-score of f1(D)
        # (1.5 / sqrt(5/3) ≈ 1.1619) instead of being halved to ~0.5809.
        expected_d = 1.5 / ((5.0 / 3.0) ** 0.5)
        self.assertAlmostEqual(values["D"], expected_d, places=5)
        # Symbol A has both f1 (-expected_d) and f2 (-1.0 over [1, 2, 3]), averaged equally.
        self.assertAlmostEqual(values["A"], 0.5 * (-expected_d - 1.0), places=5)


if __name__ == "__main__":
    unittest.main()
