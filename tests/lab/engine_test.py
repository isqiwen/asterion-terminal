"""asterion_engine: core's Replay as the lab's Python backtests drive it.

Run with the bundled interpreter: python3 -I -B engine_test.py <engine-directory>
"""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [sys.argv.pop(1), str(ROOT / "apps/services/common/python")]
import asterion_engine  # noqa: E402
import numpy as np  # noqa: E402
from asterion_strategy import operators as op  # noqa: E402

R = 10 ** 8
DAY_NS = 86400 * 10 ** 9
START = 1767600000 * 10 ** 9


def contract(symbol, days, closes, volume=1000):
    bars = [(day, START + i * DAY_NS, close * R, (close + 10) * R, (close - 10) * R, close * R,
             volume * R) for i, (day, close) in enumerate(zip(days, closes))]
    return dict(venue="SHFE", symbol=symbol, price_increment=R, quantity_increment=R,
                multiplier=10 * R, increments=[], slippage_ticks=0, bars=bars,
                costs=[dict(effective_from=days[0], source="test",
                            values=[0, 3 * R, 3 * R, 3 * R, R // 10, 0, 0, 0])],
                settlements=[(day, close * R) for day, close in zip(days, closes)])


DAYS = ["2026-01-05", "2026-01-06", "2026-01-07", "2026-01-08"]


def run(replay, decide):
    seen = []
    while not replay.finished:
        while not replay.day_done:
            bars = replay.next()
            seen.append(bars)
            decide(replay, bars)
        replay.settle()
    return seen


class Engine(unittest.TestCase):
    def test_a_target_fills_on_the_next_bar_at_its_open(self):
        replay = asterion_engine.Replay(1_000_000 * R, [contract("rb2605", DAYS, [100, 110, 120, 115])],
                                        [], (10 * R, 10 * R, 10, 10 * R))
        self.assertEqual(replay.units, 1)

        def decide(replay, bars):
            replay.want(0, R)
            replay.decide()

        seen = run(replay, decide)
        self.assertEqual([bars[0][9] // R for bars in seen], [100, 110, 120, 115])
        # Decided after the first day, filled at the second day's open of 110.
        (fill,) = replay.fills()
        self.assertEqual((fill[1], fill[2], fill[3], fill[4], fill[5]),
                         ("strategy.0", R, 110 * R, 3 * R, START + DAY_NS))
        orders = replay.orders()
        self.assertEqual([(order[2], order[3], order[6]) for order in orders],
                         [("buy", "open", "filled")])
        day, _, balance, equity, realized, fees, margin, _ = replay.settlements[-1]
        self.assertEqual((day, realized, fees), ("2026-01-08", 50 * R, 3 * R))
        self.assertEqual(replay.positions[-1], ("2026-01-08", 0, R, 0, 115 * R, 115 * R))
        self.assertEqual([point[2] for point in replay.equity].count("settlement"), 4)

    def test_an_unfilled_target_expires_after_its_bar(self):
        # The second bar has zero volume: the order is cancelled after it.
        item = contract("rb2605", DAYS[:3], [100, 130, 140])
        item["bars"][1] = (*item["bars"][1][:6], 0)
        replay = asterion_engine.Replay(1_000_000 * R, [item], [], (10 * R, 10 * R, 10, 10 * R))
        decided = []

        def decide(replay, bars):
            if not decided:
                replay.want(0, R)
                replay.decide()
                decided.append(True)

        run(replay, decide)
        self.assertEqual(replay.fills(), [])
        self.assertEqual([order[6] for order in replay.orders()], ["cancelled"])

    def test_risk_rejections_and_untouched_units(self):
        replay = asterion_engine.Replay(1_000_000 * R, [contract("rb2605", DAYS, [100, 110, 120, 115]),
                                                        contract("rb2610", DAYS, [100, 110, 120, 115])],
                                        [], (5 * R, 2 * R, 10, 2 * R))

        def decide(replay, bars):
            replay.want(0, 5 * R)
            replay.want(1, None)
            replay.decide()

        run(replay, decide)
        self.assertTrue(replay.rejections)
        self.assertEqual(replay.rejections[0][2:], (0, 5 * R, "gross_quantity"))
        self.assertEqual(replay.fills(), [])

    def test_daily_price_limits_block_limit_locked_fills(self):
        item = contract("rb2605", DAYS[:3], [100, 107, 106])
        item["bars"][1] = (DAYS[1], START + DAY_NS, 107 * R, 107 * R, 107 * R, 107 * R, 1000 * R)
        item["price_limits"] = [(DAYS[1], 93 * R, 107 * R), (DAYS[2], 99 * R, 115 * R)]
        replay = asterion_engine.Replay(1_000_000 * R, [item], [], (10 * R, 10 * R, 10, 10 * R))

        def decide(replay, bars):
            replay.want(0, R)
            replay.decide()

        run(replay, decide)
        (fill,) = replay.fills()
        self.assertEqual((fill[1], fill[3], fill[6]), ("strategy.1", 106 * R, DAYS[2]))
        self.assertEqual([order[6] for order in replay.orders()], ["cancelled", "filled"])

    def test_invalid_input_is_refused(self):
        item = contract("rb2605", DAYS, [100, 110, 120, 115])
        for broken in (dict(item, costs=[]), dict(item, bars=[("2026-01-05", 1)]),
                       dict(item, price_limits=[("2026-01-05", 90 * R)])):
            with self.assertRaises(ValueError):
                asterion_engine.Replay(R, [broken], [], (R, R, 1, R))
        with self.assertRaises(ValueError):
            asterion_engine.Replay(R, [item], [], (R, R, 1))

    def test_cpp_operators_match_python_fallback_with_nans_constants_and_spikes(self):
        self.assertIs(op._engine, asterion_engine)
        rng = np.random.default_rng(20261126)
        n = 140
        rets = rng.normal(0.0004, 0.015, n)
        close = 100.0 * np.exp(np.cumsum(rets))
        other = close * np.exp(rng.normal(0.0, 0.01, n))
        volume = rng.uniform(50.0, 5000.0, n)

        # Inject NaNs, Inf, a zero-volume stretch, a constant stretch, and a huge outlier spike.
        x = close.copy()
        y = other.copy()
        v = volume.copy()
        x[12] = np.nan
        x[45] = np.inf
        y[30] = np.nan
        v[20:26] = 0.0
        v[50] = np.nan
        x[60:72] = 100000.1
        y[60:72] = 200000.2
        x[80] = 1e9  # Spike must leave zero subtraction drift once it exits the window.

        cases = [
            ("ts_mean_1", lambda: op.ts_mean(x, 1)),
            ("ts_mean_11", lambda: op.ts_mean(x, 11)),
            ("ts_max_1", lambda: op.ts_max(x, 1)),
            ("ts_max_11", lambda: op.ts_max(x, 11)),
            ("ts_min_1", lambda: op.ts_min(x, 1)),
            ("ts_min_11", lambda: op.ts_min(x, 11)),
            ("ts_std_ddof1", lambda: op.ts_std(x, 11, ddof=1)),
            ("ts_std_ddof0", lambda: op.ts_std(x, 11, ddof=0)),
            ("ts_zscore_ddof1", lambda: op.ts_zscore(x, 11, ddof=1)),
            ("ts_zscore_ddof0", lambda: op.ts_zscore(x, 11, ddof=0)),
            ("ts_rank_1", lambda: op.ts_rank(x, 1)),
            ("ts_rank_9", lambda: op.ts_rank(x, 9)),
            ("ts_corr", lambda: op.ts_corr(x, y, 11)),
            ("decay_linear_1", lambda: op.decay_linear(x, 1)),
            ("decay_linear_11", lambda: op.decay_linear(x, 11)),
            ("ts_ols_beta", lambda: op.ts_ols_beta(y, x, 11)),
            ("ts_ols_resid", lambda: op.ts_ols_resid(y, x, 11)),
            ("ts_ols_r2", lambda: op.ts_ols_r2(y, x, 11)),
            ("ou_half_life", lambda: op.ou_half_life(np.log(close), 20)),
            ("ts_autocorr_lag1", lambda: op.ts_autocorr(x, 11, lag=1)),
            ("ts_autocorr_large_lag", lambda: op.ts_autocorr(x, 7, lag=4)),
            ("ts_skew", lambda: op.ts_skew(x, 11)),
            ("ts_kurt", lambda: op.ts_kurt(x, 11)),
            ("ts_variance_ratio", lambda: op.ts_variance_ratio(x, 15, q=4)),
            ("ts_hurst", lambda: op.ts_hurst(x, window=24, max_lag=8)),
            ("ts_vwap_dev", lambda: op.ts_vwap_dev(x, v, 11)),
            ("ts_expected_shortfall_partial", lambda: op.ts_expected_shortfall(x, 11, tail=0.25)),
            ("ts_expected_shortfall_full", lambda: op.ts_expected_shortfall(x, 11, tail=1.0)),
            ("ts_ema", lambda: op.ts_ema(x, 10)),
            ("ts_ewma_vol", lambda: op.ts_ewma_vol(x, 10)),
            ("kalman_filter", lambda: op.kalman_filter(x)),
            ("kalman_hedge_beta", lambda: op.kalman_hedge(y, x)[0]),
            ("kalman_hedge_intercept", lambda: op.kalman_hedge(y, x)[1]),
            ("kalman_hedge_spread", lambda: op.kalman_hedge(y, x)[2]),
        ]

        for label, call in cases:
            with self.subTest(operator=label):
                cpp_out = call()
                saved = op._engine
                try:
                    op._engine = None
                    py_out = call()
                finally:
                    op._engine = saved
                np.testing.assert_allclose(cpp_out, py_out, rtol=1e-9, atol=1e-10, equal_nan=True)
                # After the 1e9 spike exits every rolling window, verify zero subtraction drift.
                if label not in ("ts_ema", "ts_ewma_vol", "kalman_filter",
                                 "kalman_hedge_beta", "kalman_hedge_intercept", "kalman_hedge_spread"):
                    np.testing.assert_allclose(
                        cpp_out[110:], py_out[110:], rtol=1e-11, atol=1e-12, equal_nan=True
                    )

        # Short-input edge lengths (0, 1, 3) never crash and match fallback.
        for short in (0, 1, 3):
            sx, sy, sv = close[:short], other[:short], volume[:short]
            for fn in (
                lambda: op.ts_mean(sx, 5),
                lambda: op.ts_max(sx, 5),
                lambda: op.ts_min(sx, 5),
                lambda: op.ts_std(sx, 5),
                lambda: op.ts_zscore(sx, 5),
                lambda: op.ts_rank(sx, 5),
                lambda: op.ts_corr(sx, sy, 5),
                lambda: op.decay_linear(sx, 5),
                lambda: op.ts_ols_beta(sy, sx, 5),
                lambda: op.ts_autocorr(sx, 5, lag=1),
                lambda: op.ts_skew(sx, 5),
                lambda: op.ts_kurt(sx, 5),
                lambda: op.ts_variance_ratio(sx, 5, q=2),
                lambda: op.ts_hurst(sx, window=8, max_lag=3),
                lambda: op.ts_vwap_dev(sx, sv, 5),
                lambda: op.ts_expected_shortfall(sx, 5, tail=0.4),
            ):
                cpp_out = fn()
                saved = op._engine
                try:
                    op._engine = None
                    py_out = fn()
                finally:
                    op._engine = saved
                np.testing.assert_allclose(cpp_out, py_out, equal_nan=True)


if __name__ == "__main__":
    unittest.main()
