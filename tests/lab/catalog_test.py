"""Built-in research signals: formulas, causal windows, portfolio targets and learned forecasts."""
import datetime
from dataclasses import replace
from decimal import Decimal
import importlib
import inspect
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "research"), str(ROOT / "apps/services/common/python")]

import numpy as np

from asterion_strategy import Bar, Context, check_factor, observe, quantize_weight, step
from factors import liquidity, price, risk, technical, term_structure
from strategies.composite import CompositePortfolio
from strategies.learning import RandomFeatureForecast, RidgeForecast
from strategies.oscillators import AtrBreakout, RsiReversion
from strategies.risk_premia import (
    CarryRank, DownsideRiskRank, DualMomentum, LowVolatilityRank, OpenInterestRank, SkewnessRank, TrendCarryRank,
)
from strategies.trend import TurningPointMomentum, VolatilityTargetMomentum


EXTRA_BAR = {
    "interest_change", "momentum", "reversal", "volatility", "skip_momentum", "momentum_strength",
    "blended_momentum", "roll_yield_momentum", "carry_to_risk", "average_true_range", "rsi",
    "channel_position", "efficiency_ratio", "downside_volatility", "upside_volatility",
    "realized_skewness", "excess_kurtosis", "expected_shortfall", "semivariance_balance",
    "money_flow_index", "return_per_volume", "volume_return_correlation", "volume_weighted_return",
}


def row(symbol, index, close, carry=0.0, contract=None):
    return Bar(symbol, contract or symbol, datetime.datetime(2026, 1, 1) + datetime.timedelta(hours=index),
               close, close + 1, close - 1, close, 100.0, 1000.0, carry)


class Factors(unittest.TestCase):
    def test_all_factors_match_prefixes_and_their_strategy_history_windows(self):
        rng = np.random.default_rng(42)
        close = 100 + rng.normal(size=35).cumsum()
        source = dict(open=close - 0.2, high=close + 1, low=close - 1, close=close,
                      volume=rng.uniform(1, 100, len(close)), open_interest=rng.uniform(500, 1000, len(close)),
                      roll_yield=rng.normal(0, 0.1, len(close)))
        checked = []
        for path in sorted((ROOT / "research/factors").glob("[!_]*.py")):
            module = importlib.import_module(f"factors.{path.stem}")
            for name, function in inspect.getmembers(module, inspect.isfunction):
                if name.startswith("_") or function.__module__ != module.__name__:
                    continue
                checked.append(name)
                signature = inspect.signature(function).parameters
                inputs = {field: source[field].copy() for field in signature if field in source}
                inputs[next(iter(inputs))][17] = np.nan
                for length in (5, 9):
                    settings = {key: parameter.default for key, parameter in signature.items() if key not in inputs}
                    for parameter in ("window", "lookback", "slow"):
                        if parameter in settings:
                            settings[parameter] = length
                    settings.update({key: value for key, value in {"fast": 2, "skip": 1,
                                     "tail": 0.4, "fast_share": 0.3, "periods": 12}.items() if key in settings})
                    count = length + (name in EXTRA_BAR)
                    with self.subTest(factor=name, length=length):
                        for size in (0, 1, 3, len(close)):
                            check_factor(function, {k: v[:size] for k, v in inputs.items()}, count, **settings)
                        expected = function(**inputs, **settings)
                        ctx = Context(["a"])
                        actual = []
                        for i in range(len(close)):
                            fields = {field: values[i] for field, values in source.items()}
                            fields.update({field: values[i] for field, values in inputs.items()})
                            observe(ctx, [Bar("a", "a", row("a", i, close[i]).time, **fields)])
                            actual.append(ctx.factor(function, "a", count, **settings))
                        np.testing.assert_allclose(actual, expected, equal_nan=True)
        self.assertEqual(len(checked), 36)

    def test_price_oscillators_distinguish_direction_flat_prices_and_breakouts(self):
        close = np.array([100., 100., 103.])
        self.assertAlmostEqual(technical.commodity_channel_index(close, close, close, 3)[-1], 100)
        self.assertEqual(technical.rsi(close, 2)[-1], 100)
        self.assertEqual(technical.rsi(np.ones(4), 3)[-1], 50)
        self.assertEqual(technical.efficiency_ratio(np.array([100., 102., 101.]), 2)[-1], 1 / 3)
        self.assertEqual(technical.price_zscore(np.full(4, 1.1), 3)[-1], 0)
        flat = np.full(4, 1.1)
        self.assertEqual(technical.commodity_channel_index(flat, flat, flat, 3)[-1], 0)
        self.assertAlmostEqual(technical.log_price_slope(np.exp(np.arange(5) * 0.1), 5)[-1], 0.1)
        self.assertAlmostEqual(technical.moving_average_gap(np.array([1., 2., 3., 4.]), 2, 4)[-1], 0.4)
        closes = np.array([100., 110., 108.])
        high, low = np.array([101., 111., 110.]), np.array([99., 109., 106.])
        self.assertEqual(technical.average_true_range(high, low, closes, 2)[-1], 7.5)
        self.assertEqual(technical.channel_position(high, low, closes, 2)[-1], 0.75)
        self.assertEqual(technical.stochastic(high, low, closes, 3)[-1], 75)
        closes[-1] = 120
        self.assertGreater(technical.channel_position(high, low, closes, 2)[-1], 1)
        values = np.array([100., 110., 121., 110., 100., 90.])
        self.assertAlmostEqual(price.skip_momentum(values, 3, 1)[-1], 100 / 121 - 1)
        self.assertAlmostEqual(price.distance_to_high(values, 4)[-1], 90 / 121 - 1)

    def test_distribution_and_range_risk_match_analytic_samples(self):
        close = np.r_[1., np.cumprod(1 + np.array([-0.1, 0.1, -0.2, 0.2]))]
        self.assertAlmostEqual(risk.downside_volatility(close, 4)[-1], np.sqrt(0.0125))
        self.assertAlmostEqual(risk.upside_volatility(close, 4)[-1], np.sqrt(0.0125))
        self.assertAlmostEqual(risk.realized_skewness(close, 4)[-1], 0)
        self.assertAlmostEqual(risk.excess_kurtosis(close, 4)[-1], -1.64)
        self.assertAlmostEqual(risk.expected_shortfall(close, 4, 0.5)[-1], 0.15)
        self.assertAlmostEqual(risk.semivariance_balance(close, 4)[-1], 0)
        high, low, flat = np.full(4, np.exp(0.1)), np.full(4, np.exp(-0.1)), np.ones(4)
        self.assertAlmostEqual(risk.parkinson_volatility(high, low, 4)[-1], np.sqrt(0.01 / np.log(2)))
        self.assertAlmostEqual(risk.garman_klass_volatility(flat, high, low, flat, 4)[-1], np.sqrt(0.02))
        self.assertAlmostEqual(risk.rogers_satchell_volatility(flat, high, low, flat, 4)[-1], np.sqrt(0.02))
        self.assertTrue(np.isnan(risk.realized_skewness(flat, 3)[-1]))

    def test_volume_and_carry_keep_their_units_and_missing_data(self):
        volume = np.array([10., 10., 20., 30.])
        close = np.r_[100., 100 * np.cumprod([0.9, 1.0, 1.1])]
        self.assertAlmostEqual(liquidity.relative_volume(volume, 3)[-1], 0.5)
        self.assertAlmostEqual(liquidity.volume_return_correlation(close, volume, 3)[-1], 1)
        self.assertAlmostEqual(liquidity.volume_weighted_return(close, volume, 3)[-1], 2 / 60)
        self.assertAlmostEqual(liquidity.turnover(volume, np.full(4, 100.), 3)[-1], 0.2)
        self.assertAlmostEqual(liquidity.return_per_volume(close, volume, 3)[-1], (0.1 / 10 + 0.1 / 30) / 3)
        self.assertEqual(liquidity.money_flow_index(np.ones(4), np.ones(4), np.ones(4), volume, 3)[-1], 50)
        self.assertTrue(np.isnan(liquidity.turnover(volume, np.zeros(4), 3)[-1]))
        carry = np.array([0.02, 0.04, 0.06, 0.08])
        self.assertAlmostEqual(term_structure.carry_to_risk(close, carry, 3, 100)[-1], 0.06)
        self.assertAlmostEqual(term_structure.carry_zscore(carry, 3)[-1], np.sqrt(1.5))
        carry[-1] = np.nan
        self.assertTrue(np.isnan(term_structure.roll_yield_carry(carry, 3)[-1]))


class Portfolios(unittest.TestCase):
    def carry_run(self, gap=False, missing=False, roll=False):
        strategy = CarryRank(window=4, volatility=3, count=1, rebalance=1)
        ctx = Context(["a", "b"], {"a2": 2.0})
        out = []
        for i, close in enumerate((100., 101., 99., 102., 104.)):
            if gap and i == 2:
                step(strategy, ctx, [row("a", i - 0.5, 500)])
            carry = np.nan if missing and i == 4 else 0.1
            contract = "a2" if roll and i >= 2 else "a"
            values = step(strategy, ctx, [row("a", i, close * (2 if roll and i >= 2 else 1), carry, contract),
                                          row("b", i, close, -0.1)])
            out.append(values)
        return out, ctx

    def test_carry_uses_real_roll_yield_and_waits_for_shared_history(self):
        output, ctx = self.carry_run()
        self.assertEqual(output[:3], [{}, {}, {}])
        self.assertTrue(ctx.ready)
        self.assertAlmostEqual(output[-1]["a"], 0.3)
        self.assertAlmostEqual(output[-1]["b"], -0.3)
        self.assertEqual(self.carry_run(gap=True)[0], output)
        self.assertEqual(self.carry_run(roll=True)[0], output)
        self.assertEqual(self.carry_run(missing=True)[0][-1], {"a": 0.0, "b": 0.0})

    def test_composite_carry_component_is_actual_carry(self):
        composite = CompositePortfolio(trend_lookback=6, carry_lookback=3, count=1,
                                       trend_share=0.0, carry_share=1.0)
        ctx = Context(["a", "b"])
        for i, close in enumerate((100, 101, 99, 102, 104, 103, 105)):
            output = step(composite, ctx, [row("a", i, close, -0.1), row("b", i, close, 0.1)])
        self.assertEqual(output, {"a": -0.3, "b": 0.3})

    def test_dual_momentum_leaves_the_negative_side_unused_in_rising_markets(self):
        strategy = DualMomentum(window=3, volatility=3, count=1, rebalance=1)
        ctx = Context(["a", "b"])
        for i, close in enumerate((100., 103., 102., 108.)):
            output = step(strategy, ctx, [row("a", i, close), row("b", i, 100 + (close - 100) / 2)])
        self.assertEqual(output, {"a": 0.3, "b": 0.0})

    def test_risk_and_position_rankings_follow_their_factor_direction(self):
        cases = [(SkewnessRank, [-0.1, 0.01, 0.01, 0.01], [0.1, -0.01, -0.01, -0.01]),
                 (LowVolatilityRank, [0.01, 0.02, 0.01, 0.02], [0.05, 0.1, 0.05, 0.1]),
                 (DownsideRiskRank, [-0.01, 0.02, -0.01, 0.02], [-0.05, 0.1, -0.05, 0.1]),
                 (OpenInterestRank, [-0.01, 0.02, -0.01, 0.02], [-0.01, 0.02, -0.01, 0.02])]
        for cls, first, second in cases:
            with self.subTest(strategy=cls.__name__):
                strategy = cls(window=4, volatility=4, count=1, rebalance=1)
                ctx = Context(["a", "b"])
                closes = [100 * np.r_[1, np.cumprod(1 + np.array(changes))] for changes in (first, second)]
                for i, (a, b) in enumerate(zip(*closes)):
                    output = step(strategy, ctx, [replace(row("a", i, a), open_interest=1000 + 50 * i),
                                                  replace(row("b", i, b), open_interest=1000 - 50 * i)])
                self.assertEqual(output, {"a": 0.3, "b": -0.3})

    def test_trend_carry_ranks_the_intersection_and_closes_unrankable_ties(self):
        strategy = TrendCarryRank(window=3, volatility=3, count=1, rebalance=1, trend_share=0.0)
        ctx = Context(["a", "b"])
        for i, close in enumerate((100., 101., 99., 102., 104., 103., 105.)):
            output = step(strategy, ctx, [row("a", i, close, -0.1 if i < 4 else 0.1),
                                          row("b", i, close, 0.1)])
            if i == 3:
                self.assertAlmostEqual(output["a"], -0.3)
                self.assertAlmostEqual(output["b"], 0.3)
        self.assertEqual(output, {"a": 0.0, "b": 0.0})

    def test_volatility_target_reduces_risk_and_respects_both_weight_limits(self):
        strategy = VolatilityTargetMomentum(lookback=3, volatility=3, periods=1, rebalance=1,
                                             target_volatility=0.001, max_weight=0.1, gross_weight=0.15)
        ctx = Context(["a", "b"])
        for i, value in enumerate((100., 102., 101., 103.)):
            output = step(strategy, ctx, [row("a", i, value), row("b", i, 100 + 2 * (value - 100))])
        self.assertGreater(output["a"], output["b"])
        self.assertLessEqual(max(output.values()), 0.1)
        self.assertLessEqual(sum(output.values()), 0.15)
        strategy.params["target_volatility"] = 10.0
        output = step(strategy, ctx, [row("a", 4, 105), row("b", 4, 110)])
        self.assertEqual(output, {"a": 0.075, "b": 0.075})

    def test_turning_point_blend_is_flat_in_both_disagreement_states(self):
        for closes, weight in (((100, 110, 121, 120), 0.0), ((100, 90, 81, 82), 0.0),
                               ((100, 110, 121, 125), 0.1), ((100, 90, 81, 80), -0.1)):
            with self.subTest(closes=closes):
                ctx = Context(["a"])
                strategy = TurningPointMomentum(fast=1, slow=3, rebalance=1)
                for i, close in enumerate(closes):
                    output = step(strategy, ctx, [row("a", i, close)])
                self.assertEqual(output, {"a": weight})

    def test_wire_precision_keeps_large_portfolios_inside_gross_and_net_budgets(self):
        rng = np.random.default_rng(7)
        prices = 100 * np.cumprod(1 + rng.uniform(-0.02, 0.04, (4, 64)), axis=0)
        symbols = [str(i) for i in range(64)]
        for strategy in (CarryRank(window=3, volatility=3, count=10, rebalance=1),
                         VolatilityTargetMomentum(lookback=3, volatility=3, periods=1, rebalance=1)):
            ctx = Context(symbols)
            for i, values in enumerate(prices):
                output = step(strategy, ctx, [row(s, i, values[j], j / 100) for j, s in enumerate(symbols)])
            weights = list(map(quantize_weight, output.values()))
            gross = sum(map(abs, weights), Decimal(0))
            self.assertLessEqual(gross, Decimal(str(strategy.params["gross_weight"])))
            if isinstance(strategy, CarryRank):
                self.assertEqual(gross, Decimal("0.6"))
                self.assertEqual(sum(weights, Decimal(0)), 0)

    def test_rsi_reversion_enters_exits_and_respects_allowed_sides(self):
        for sides, final in (("both", -1), ("long", 0)):
            strategy = RsiReversion(window=2, sides=sides)
            ctx = Context(["a"])
            outputs = [step(strategy, ctx, [row("a", i, close)])
                       for i, close in enumerate((100, 99, 98, 99, 100))]
            self.assertEqual(outputs, [{}, {}, {"a": 1}, {"a": 0}, {"a": final}])

    def test_atr_breakout_uses_the_prior_band_and_exits_at_the_mean(self):
        strategy, ctx = AtrBreakout(window=2, width=2.0), Context(["a"])
        outputs = [step(strategy, ctx, [row("a", i, close)])
                   for i, close in enumerate((100, 100, 100, 110, 106, 103))]
        self.assertEqual(outputs, [{}, {}, {}, {"a": 1}, {"a": 1}, {"a": 0}])


class Learning(unittest.TestCase):
    def test_learned_forecasts_keep_the_training_mean_and_wait_for_labels(self):
        for cls in (RidgeForecast, RandomFeatureForecast):
            for rate in (0.0, 0.01, -0.01):
                with self.subTest(model=cls.__name__, rate=rate):
                    strategy = cls(fast=2, slow=4, train=8, rebalance=1)
                    closes = 100 * (1 + rate) ** np.arange(strategy.need)
                    self.assertAlmostEqual(strategy.forecast(closes, closes), rate, places=10)
                    ctx = Context(["a"])
                    for i, close in enumerate(closes):
                        output = step(strategy, ctx, [row("a", i, close)])
                        if i < strategy.need - 1:
                            self.assertFalse(ctx.ready)
                            self.assertEqual(output, {})
                    self.assertEqual(output, {"a": int(rate > 0) - int(rate < 0)})

    def test_forecasts_learn_returns_between_executable_opens_over_the_rebalance_horizon(self):
        for cls in (RidgeForecast, RandomFeatureForecast):
            strategy = cls(fast=2, slow=4, train=8, rebalance=3)
            opens = 100 * 1.01 ** np.arange(strategy.need)
            closes = np.full(strategy.need, 100.0)
            self.assertAlmostEqual(strategy.forecast(opens, closes), 1.01 ** 3 - 1, places=10)
            shock = np.full(strategy.need, 100.0)
            shock[-1] = 120.0
            self.assertAlmostEqual(strategy.forecast(shock, closes), 0.2 / 8, places=10)
            ctx = Context(["a"])
            for i, (opening, close) in enumerate(zip(opens, closes)):
                output = step(strategy, ctx, [replace(row("a", i, close), open=opening, high=opening + 1)])
                if i < strategy.need - 1:
                    self.assertEqual(output, {})
            self.assertEqual(output, {"a": 1})

    def test_random_features_replay_identically_from_a_frozen_seed(self):
        rng = np.random.default_rng(9)
        closes = 100 * np.exp(np.cumsum(rng.normal(0.001, 0.02, 80)))
        def replay(values):
            strategy = RandomFeatureForecast(fast=2, slow=6, train=12, features=32, rebalance=3)
            ctx = Context(["a"])
            return [dict(step(strategy, ctx, [row("a", i, close)])) for i, close in enumerate(values)]
        full = replay(closes)
        changed = closes.copy()
        changed[50:] *= 2
        self.assertEqual(full[:50], replay(closes[:50]))
        self.assertEqual(full[:50], replay(changed)[:50])
        self.assertEqual(full, replay(closes))
        self.assertTrue(any(value.get("a") for value in full))


if __name__ == "__main__":
    unittest.main()
