"""Statistical significance and overfitting detection tests (PSR, DSR, NW-HAC, Bootstrap, CSCV-PBO).

Run with the bundled interpreter:
python3 -I -B statistics_test.py <source>
"""
import sys
import unittest

import numpy as np

sys.path[:0] = [sys.argv.pop(1)]
from asterion_lab import statistics as lab_stats  # noqa: E402


class StatisticsTest(unittest.TestCase):
    def test_psr_and_dsr_penalize_multiple_trials_and_negative_skew(self):
        rng = np.random.default_rng(123)
        # Positive-drift series: high PSR under 1 trial, lower DSR as trials grow.
        rets = rng.normal(0.0012, 0.01, size=252)
        psr = lab_stats.probabilistic_sharpe_ratio(rets, benchmark_sr=0.0, annual_periods=252.0)
        self.assertIsNotNone(psr)
        self.assertGreater(psr["psr"], 0.90)

        dsr_1 = lab_stats.deflated_sharpe_ratio(rets, trials=1, annual_periods=252.0)
        dsr_50 = lab_stats.deflated_sharpe_ratio(rets, trials=50, annual_periods=252.0)
        dsr_500 = lab_stats.deflated_sharpe_ratio(rets, trials=500, annual_periods=252.0)
        self.assertAlmostEqual(dsr_1["dsr"], psr["psr"], places=12)
        self.assertGreater(dsr_1["dsr"], dsr_50["dsr"])
        self.assertGreater(dsr_50["dsr"], dsr_500["dsr"])
        self.assertGreater(dsr_500["expected_max_sharpe"], dsr_50["expected_max_sharpe"])

        # Negatively skewed fat-tailed series with same mean/std has lower PSR than symmetric series.
        sym = np.tile(np.array([0.012, -0.008]), 120)
        crash = np.full(240, 0.004)
        crash[::40] = -0.05
        # Scale crash to match sym mean and std closely while retaining strong negative skew.
        crash = (crash - np.mean(crash)) / np.std(crash, ddof=1) * np.std(sym, ddof=1) + np.mean(sym)
        psr_sym = lab_stats.probabilistic_sharpe_ratio(sym, annual_periods=252.0)
        psr_crash = lab_stats.probabilistic_sharpe_ratio(crash, annual_periods=252.0)
        self.assertLess(psr_crash["skewness"], -1.0)
        self.assertLess(psr_crash["psr"], psr_sym["psr"])

    def test_newey_west_hac_reduces_inflated_t_on_autocorrelated_series(self):
        rng = np.random.default_rng(42)
        # Construct an MA(5) overlapping return series with positive serial correlation.
        eps = rng.normal(0.002, 0.01, size=305)
        overlapping = np.convolve(eps, np.ones(6) / 6.0, mode="valid")
        plain = lab_stats.newey_west_tstat(overlapping, max_lag=0)
        hac = lab_stats.newey_west_tstat(overlapping, max_lag=5)
        auto = lab_stats.newey_west_tstat(overlapping)
        self.assertIsNotNone(plain)
        self.assertIsNotNone(hac)
        self.assertIsNotNone(auto)
        # Lag-0 matches ordinary t = mean / (std / sqrt(N)).
        expected_t0 = float(np.mean(overlapping) / (np.std(overlapping, ddof=1) / np.sqrt(len(overlapping))))
        self.assertAlmostEqual(plain["t"], expected_t0, places=10)
        # Positive autocorrelation inflates ordinary t, so HAC t is strictly smaller in magnitude.
        self.assertGreater(hac["se"], plain["se"])
        self.assertLess(abs(hac["t"]), abs(plain["t"]))
        self.assertLess(abs(auto["t"]), abs(plain["t"]))

    def test_block_bootstrap_ci_is_reproducible_and_brackets_sample_estimates(self):
        rng = np.random.default_rng(7)
        rets = rng.normal(0.001, 0.01, size=200)
        ci1 = lab_stats.block_bootstrap_ci(rets, annual_periods=252.0, n_samples=400, seed=99)
        ci2 = lab_stats.block_bootstrap_ci(rets, annual_periods=252.0, n_samples=400, seed=99)
        self.assertEqual(ci1, ci2)
        self.assertLess(ci1["sharpe_ci"][0], ci1["sharpe_ci"][1])
        self.assertLess(ci1["annual_return_ci"][0], ci1["annual_return_ci"][1])
        self.assertLessEqual(ci1["max_drawdown_ci"][0], ci1["max_drawdown_ci"][1])
        self.assertLessEqual(ci1["max_drawdown_ci"][1], 0.0)
        sample_sr = float(np.mean(rets) / np.std(rets, ddof=1) * np.sqrt(252.0))
        self.assertLessEqual(ci1["sharpe_ci"][0], sample_sr)
        self.assertGreaterEqual(ci1["sharpe_ci"][1], sample_sr)

    def test_cscv_pbo_distinguishes_true_skill_from_pure_noise(self):
        rng = np.random.default_rng(2026)
        # 1) Pure zero-mean noise across 12 candidates -> PBO near 0.5 (>= 0.35).
        noise = rng.normal(0.0, 0.01, size=(160, 12))
        noise_pbo = lab_stats.cscv_pbo(noise, n_partitions=8)
        self.assertIsNotNone(noise_pbo)
        self.assertEqual(noise_pbo["partitions"], 8)
        self.assertEqual(noise_pbo["combinations"], 70)
        self.assertGreater(noise_pbo["pbo"], 0.30)

        # 2) One genuinely strong candidate among noisy peers -> PBO near 0.0, positive rank corr.
        skilled = noise.copy()
        skilled[:, 0] = rng.normal(0.005, 0.008, size=160)
        skilled[:, 1] = rng.normal(0.002, 0.009, size=160)
        skilled[:, 2] = rng.normal(-0.003, 0.009, size=160)
        skill_pbo = lab_stats.cscv_pbo(skilled, n_partitions=8)
        self.assertIsNotNone(skill_pbo)
        self.assertLess(skill_pbo["pbo"], 0.10)
        self.assertGreater(skill_pbo["is_oos_rank_corr"], 0.25)


if __name__ == "__main__":
    unittest.main()
