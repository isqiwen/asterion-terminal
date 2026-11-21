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
from factors import liquidity, positions, price, risk, technical, term_structure
from strategies.composite import CompositePortfolio
from strategies.learning import RandomFeatureForecast, RidgeForecast
from strategies.oscillators import AtrBreakout, RsiReversion
from strategies.panel import PanelRandomFeatureRank, PanelRidgeRank
from strategies.risk_premia import (
    CarryRank, ContinuousMomentumRank, DownsideRiskRank, DualMomentum, LowVolatilityRank, MaximumReturnRank,
    OpenInterestRank, SkewnessRank, TrendCarryRank,
)
from strategies.trend import TurningPointMomentum, VolatilityTargetMomentum


EXTRA_BAR = {
    "interest_change", "momentum", "reversal", "volatility", "skip_momentum", "momentum_strength",
    "blended_momentum", "roll_yield_momentum", "carry_to_risk", "average_true_range", "rsi",
    "channel_position", "efficiency_ratio", "downside_volatility", "upside_volatility",
    "realized_skewness", "excess_kurtosis", "expected_shortfall", "semivariance_balance",
    "money_flow_index", "return_per_volume", "volume_return_correlation", "volume_weighted_return",
    "information_discreteness", "return_autocorrelation", "variance_ratio", "maximum_return",
    "yang_zhang_volatility",
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
        self.assertEqual(len(checked), 42)

    def test_continuity_and_serial_dependence_distinguish_smooth_and_jumpy_paths(self):
        smooth = np.array([100., 101., 103., 106., 110.])
        jumpy = np.array([100., 99., 98., 97., 110.])
        self.assertEqual(price.information_discreteness(smooth, 4)[-1], -1)
        self.assertEqual(price.information_discreteness(jumpy, 4)[-1], 0.5)
        self.assertEqual(price.information_discreteness(np.array([100., 100., 101., 101., 102.]), 4)[-1], -0.5)
        self.assertEqual(price.information_discreteness(np.array([100., 110., 120., 130., 100.]), 4)[-1], 0)
        alternating = 100 * np.r_[1, np.cumprod([1.1, 0.9, 1.1, 0.9])]
        self.assertAlmostEqual(price.return_autocorrelation(alternating, 4)[-1], -1)
        self.assertAlmostEqual(price.variance_ratio(np.exp(np.array([0., 1., 3., 6., 10.])), 4)[-1], 1.6)
        self.assertAlmostEqual(price.variance_ratio(np.exp(np.array([0., 1., 0., 1., 0.])), 4)[-1], 0)
        for function in (price.return_autocorrelation, price.variance_ratio):
            self.assertTrue(np.isnan(function(np.ones(5), 4)[-1]))

    def test_max_return_keeps_losses_and_yang_zhang_measures_opening_gaps(self):
        close = 100 * np.r_[1, np.cumprod([0.9, 0.98, 0.95])]
        self.assertAlmostEqual(risk.maximum_return(close, 3)[-1], -0.02)
        flat_intraday = np.exp(np.array([0., 0.1, 0.3, 0.2]))
        self.assertAlmostEqual(risk.yang_zhang_volatility(
            flat_intraday, flat_intraday, flat_intraday, flat_intraday, 3)[-1],
            np.std([0.1, 0.2, -0.1], ddof=1))
        opening = np.ones(4)
        close = np.exp(np.array([0., 0.1, -0.1, 0.2]))
        high, low = np.exp(np.maximum(0, np.log(close)) + 0.1), np.exp(np.minimum(0, np.log(close)) - 0.1)
        k = 0.34 / (1.34 + 2)
        expected = np.sqrt(np.var([0., -0.1, 0.1], ddof=1) + k * np.var([0.1, -0.1, 0.2], ddof=1)
                           + (1 - k) * np.mean([0.04, 0.04, 0.06]))
        self.assertAlmostEqual(risk.yang_zhang_volatility(opening, high, low, close, 3)[-1], expected)

    def test_chaikin_flow_weights_close_locations_by_volume(self):
        high, low = np.array([12., 12., 10.]), np.array([8., 8., 10.])
        close, volume = np.array([12., 8., 10.]), np.array([30., 10., 10.])
        self.assertAlmostEqual(liquidity.chaikin_money_flow(high, low, close, volume, 3)[-1], 0.4)
        self.assertTrue(np.isnan(liquidity.chaikin_money_flow(high, low, close, np.zeros(3), 3)[-1]))

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
                 (MaximumReturnRank, [-0.01, 0.02, -0.01, 0.02], [-0.05, 0.1, -0.05, 0.1]),
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

    def test_continuous_momentum_excludes_a_jumpy_winner(self):
        strategy = ContinuousMomentumRank(window=4, volatility=4, count=1, rebalance=1)
        ctx = Context(["a", "b", "c"])
        for i, prices in enumerate(zip((100, 101, 103, 106, 110), (100, 99, 97, 94, 90),
                                        (100, 99, 98, 97, 120))):
            output = step(strategy, ctx, [row(s, i, close) for s, close in zip(ctx.symbols, prices)])
        self.assertEqual(output, {"a": 0.3, "b": -0.3, "c": 0.0})

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


class PanelLearning(unittest.TestCase):
    def sample(self, length):
        rng = np.random.default_rng(15)
        data = {}
        for symbol in ("a", "b", "c", "d"):
            close = 100 * np.exp(rng.normal(0.001, 0.02, length).cumsum())
            opening = close * np.exp(rng.normal(0, 0.005, length))
            data[symbol] = dict(open=opening, high=np.maximum(opening, close) + 1,
                                low=np.minimum(opening, close) - 1, close=close,
                                volume=rng.uniform(100, 300, length), open_interest=rng.uniform(1000, 2000, length),
                                roll_yield=rng.normal(0, 0.05, length), next_open=np.r_[opening[1:], np.nan])
        return data

    def test_pooled_forecasts_share_labels_and_exclude_unfinished_training_features(self):
        for cls in (PanelRidgeRank, PanelRandomFeatureRank):
            for horizon in (1, 3):
                with self.subTest(model=cls.__name__, horizon=horizon):
                    strategy = cls(fast=2, window=4, train=8, volatility=3, rebalance=horizon)
                    n = strategy.need
                    data = {}
                    for symbol, rate in (("a", 0.01), ("b", 0.02)):
                        opening = 100 * (1 + rate) ** np.arange(n)
                        data[symbol] = dict(open=opening, high=np.full(n, 200.), low=np.full(n, 1.),
                                            close=np.full(n, 100.), volume=np.full(n, 100.),
                                            open_interest=np.full(n, 1000.), roll_yield=np.full(n, np.nan),
                                            next_open=np.r_[opening[1:], np.nan])
                    expected = ((1.01 ** horizon - 1) + (1.02 ** horizon - 1)) / 2
                    np.testing.assert_allclose(list(strategy.scores(data).values()), [expected] * 2, atol=1e-12)
                    for row_data in data.values():
                        row_data["close"][-horizon - 1:] = 150
                    np.testing.assert_allclose(list(strategy.scores(data).values()), [expected] * 2, atol=1e-12)

    def test_pooled_ridge_matches_an_augmented_least_squares_fit(self):
        strategy = PanelRidgeRank(fast=2, window=4, train=8, volatility=3, rebalance=2, ridge=0.7)
        data = self.sample(strategy.need)
        x, y, query = [], [], []
        for values in data.values():
            close = values["close"]
            features = np.column_stack((price.momentum(close, 2), price.momentum(close, 4),
                price.volatility(close, 2), technical.price_zscore(close, 4), technical.rsi(close, 2),
                technical.efficiency_ratio(close, 4), price.information_discreteness(close, 4),
                risk.maximum_return(close, 4),
                liquidity.chaikin_money_flow(values["high"], values["low"], close, values["volume"], 4),
                positions.interest_change(values["open_interest"], 4)))
            for time in range(4, 12):
                x.append(features[time])
                y.append(values["open"][time + 3] / values["open"][time + 1] - 1)
            query.append(features[-1])
        x, query = np.asarray(x), np.asarray(query)
        mean, scale = x.mean(axis=0), x.std(axis=0)
        x = np.column_stack((np.ones(len(x)), (x - mean) / scale))
        query = np.column_stack((np.ones(len(query)), (query - mean) / scale))
        penalty = np.diag([0.] + [np.sqrt(0.7 * len(x))] * 10)
        beta = np.linalg.lstsq(np.vstack((x, penalty)), np.r_[y, np.zeros(11)], rcond=None)[0]
        np.testing.assert_allclose(list(strategy.scores(data).values()), query @ beta, atol=1e-12)

    def test_labels_use_the_next_symbol_open_between_shared_observations(self):
        for cls in (PanelRidgeRank, PanelRandomFeatureRank):
            class Observed(cls):
                def scores(self, data):
                    self.prediction = super().scores(data)
                    return self.prediction
            strategy = Observed(fast=2, window=4, train=8, volatility=3, rebalance=1, count=1)
            ctx = Context(["a", "b"])
            for i in range(strategy.need):
                if i:
                    step(strategy, ctx, [row("a", i - 0.5, 100 * 1.02 ** i)])
                close = 100 + i + (-1) ** i
                openings = (100 * (1 + 0.08 * i), 100 * 1.02 ** i)
                bars = [replace(row(s, i, close), open=opening, high=max(opening, close) + 1)
                        for s, opening in zip(ctx.symbols, openings)]
                output = step(strategy, ctx, bars)
                self.assertEqual(ctx.ready, i == strategy.need - 1)
                if not ctx.ready:
                    self.assertEqual(output, {})
            np.testing.assert_allclose(list(strategy.prediction.values()), [0.02, 0.02], atol=1e-12)

    def test_missing_carry_is_optional_or_excluded_from_the_panel(self):
        for cls in (PanelRidgeRank, PanelRandomFeatureRank):
            params = dict(fast=2, window=4, train=8, volatility=3, rebalance=2)
            strategy = cls(use_carry=True, **params)
            data = self.sample(strategy.need)
            self.assertEqual(set(strategy.scores(data)), set(data))
            data["a"]["roll_yield"][-1] = np.nan
            self.assertEqual(set(strategy.scores(data)), {"b", "c", "d"})
            for values in data.values():
                values["roll_yield"][:] = np.nan
            self.assertEqual(strategy.scores(data), {})
            self.assertEqual(set(cls(**params).scores(data)), set(data))

    def test_panel_replay_preserves_prefixes_seed_and_portfolio_budgets(self):
        data = self.sample(65)
        changed = {s: {field: values.copy() for field, values in fields.items()} for s, fields in data.items()}
        for fields in changed.values():
            for field in ("open", "high", "low", "close"):
                fields[field][50:] *= 2
        for cls in (PanelRidgeRank, PanelRandomFeatureRank):
            def replay(source, stop=65, reverse=False):
                strategy = cls(fast=2, window=6, train=12, volatility=3, rebalance=3, count=2)
                ctx = Context(list(reversed(source)) if reverse else list(source))
                return [dict(step(strategy, ctx, [Bar(s, s, row(s, i, 100).time,
                            **{field: values[i] for field, values in source[s].items() if field != "next_open"})
                            for s in ctx.symbols])) for i in range(stop)]
            full = replay(data)
            self.assertEqual(full[:50], replay(data, 50))
            self.assertEqual(full[:50], replay(changed)[:50])
            self.assertEqual(full, replay(data, reverse=True))
            self.assertTrue(any(any(weights.values()) for weights in full))
            for weights in full:
                values = list(map(quantize_weight, weights.values()))
                self.assertLessEqual(sum(map(abs, values), Decimal(0)), Decimal("0.6"))
                self.assertEqual(sum(values, Decimal(0)), 0)

    def test_panel_roll_reprices_training_entries_and_exits_with_the_price_history(self):
        data = self.sample(40)
        for cls in (PanelRidgeRank, PanelRandomFeatureRank):
            def replay(roll):
                strategy = cls(fast=2, window=6, train=12, volatility=3, rebalance=3, count=1)
                ctx = Context(list(data), {"a2": 2.0})
                decisions = []
                for i in range(40):
                    bars = []
                    for s, source in data.items():
                        rolled = roll and s == "a" and i >= 25
                        fields = {field: values[i] * (2 if rolled and field in ("open", "high", "low", "close") else 1)
                                  for field, values in source.items() if field != "next_open"}
                        bars.append(Bar(s, "a2" if rolled else s, row(s, i, 100).time, **fields))
                    decisions.append(dict(step(strategy, ctx, bars)))
                return decisions
            self.assertEqual(replay(False), replay(True))


if __name__ == "__main__":
    unittest.main()
