"""Tests for LHS and Gaussian Process Expected Improvement parameter search.

Run with the bundled interpreter:
python3 -I -B optimize_test.py <source>
"""
import sys
import unittest

sys.path[:0] = [sys.argv.pop(1)]
from asterion_lab import optimize  # noqa: E402


class Optimize(unittest.TestCase):
    def test_lhs_sample_covers_strata_without_duplicates_and_is_deterministic(self):
        first = optimize.lhs_sample([6, 6, 6], 18, seed=42)
        second = optimize.lhs_sample([6, 6, 6], 18, seed=42)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 18)
        self.assertEqual(len(set(first)), 18)
        # Every axis is spread across nearly all discrete levels.
        for axis in range(3):
            covered = {combo[axis] for combo in first}
            self.assertGreaterEqual(len(covered), 5)

        # Exhausting a small grid returns all unique cells, respecting exclude.
        small = optimize.lhs_sample([2, 3], 10, exclude={(0, 0), (1, 2)}, seed=7)
        self.assertEqual(len(small), 4)
        self.assertEqual(len(set(small)), 4)
        self.assertNotIn((0, 0), small)
        self.assertNotIn((1, 2), small)

    def test_gp_suggest_converges_to_optimum_on_2d_surface(self):
        xs = optimize.encode_axis(list(range(10)))
        ys = optimize.encode_axis(list(range(10)))
        axis_coords = [xs, ys]

        def objective(combo):
            i, j = combo
            # Smooth unimodal surface peaking at (7, 3) with max value 2.0.
            return 2.0 - 0.15 * ((i - 7) ** 2 + (j - 3) ** 2)

        tried = set()
        observed = []
        init = optimize.lhs_sample([10, 10], 6, seed=123)
        for combo in init:
            tried.add(combo)
            observed.append((combo, objective(combo)))

        for wave in range(5):
            batch = optimize.gp_suggest(
                axis_coords, observed, batch_size=3, tried=tried, seed=123 + wave
            )
            self.assertEqual(len(batch), 3)
            self.assertEqual(len(set(batch)), 3)
            for combo in batch:
                self.assertNotIn(combo, tried)
                tried.add(combo)
                observed.append((combo, objective(combo)))

        best_combo, best_val = max(observed, key=lambda pair: pair[1])
        # Out of 100 cells, 21 GP-EI evaluations find the peak (or immediate neighbor).
        self.assertGreaterEqual(best_val, 1.7)
        self.assertLessEqual(abs(best_combo[0] - 7) + abs(best_combo[1] - 3), 2)

    def test_gp_suggest_handles_huge_grid_without_enumerating_cartesian_product(self):
        # 20^5 = 3,200,000 combinations (> 10,000 exact pool limit).
        coords = [optimize.encode_axis(list(range(20))) for _ in range(5)]
        init = optimize.lhs_sample([20] * 5, 8, seed=9)
        obs = [(combo, -sum((c - 10) ** 2 for c in combo)) for combo in init]
        batch = optimize.gp_suggest(coords, obs, batch_size=4, tried=set(init), seed=99)
        self.assertEqual(len(batch), 4)
        self.assertEqual(len(set(batch)), 4)
        for combo in batch:
            self.assertNotIn(combo, init)


if __name__ == "__main__":
    unittest.main()
