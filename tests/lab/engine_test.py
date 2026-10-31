"""asterion_engine: core's Replay as the lab's Python backtests drive it.

Run with the bundled interpreter: python3 -I -B engine_test.py <engine-directory>
"""
import sys
import unittest

sys.path[:0] = [sys.argv.pop(1)]
import asterion_engine  # noqa: E402

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
    def test_a_target_fills_on_the_next_bar_at_the_decided_close(self):
        replay = asterion_engine.Replay(1_000_000 * R, [contract("rb2605", DAYS, [100, 110, 120, 115])],
                                        [], (10 * R, 10 * R, 10))
        self.assertEqual(replay.units, 1)

        def decide(replay, bars):
            replay.want(0, R)
            replay.decide()

        seen = run(replay, decide)
        self.assertEqual([bars[0][9] // R for bars in seen], [100, 110, 120, 115])
        # Decided at 100 on the first day, filled on the second, whose low reaches it.
        (fill,) = replay.fills()
        self.assertEqual((fill[1], fill[2], fill[3], fill[4], fill[5]),
                         ("strategy.0", R, 100 * R, 3 * R, START + DAY_NS))
        orders = replay.orders()
        self.assertEqual([(order[2], order[3], order[6]) for order in orders],
                         [("buy", "open", "filled")])
        day, _, balance, equity, realized, fees, margin, _ = replay.settlements[-1]
        self.assertEqual((day, realized, fees), ("2026-01-08", 150 * R, 3 * R))
        self.assertEqual(replay.positions[-1], ("2026-01-08", 0, R, 0, 115 * R, 115 * R))
        self.assertEqual([point[2] for point in replay.equity].count("settlement"), 4)

    def test_an_unfilled_target_expires_after_its_bar(self):
        # The second bar never trades down to 100: the order is cancelled after it.
        item = contract("rb2605", DAYS[:3], [100, 130, 140])
        replay = asterion_engine.Replay(1_000_000 * R, [item], [], (10 * R, 10 * R, 10))
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
                                        [], (2 * R, 2 * R, 10))

        def decide(replay, bars):
            replay.want(0, 5 * R)
            replay.want(1, None)
            replay.decide()

        run(replay, decide)
        self.assertTrue(replay.rejections)
        self.assertEqual(replay.rejections[0][1:], ("strategy.0", 5 * R, "order_quantity"))
        self.assertEqual(replay.fills(), [])

    def test_invalid_input_is_refused(self):
        item = contract("rb2605", DAYS, [100, 110, 120, 115])
        for broken in (dict(item, costs=[]), dict(item, bars=[("2026-01-05", 1)])):
            with self.assertRaises(ValueError):
                asterion_engine.Replay(R, [broken], [], (R, R, 1))
        with self.assertRaises(ValueError):
            asterion_engine.Replay(R, [item], [], (R, R))


if __name__ == "__main__":
    unittest.main()
