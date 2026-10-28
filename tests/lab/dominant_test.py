"""The dominant series rule, as the C++ Data service's tests state it.

Run with the bundled interpreter: python3 -I -B dominant_test.py <source>
"""
from decimal import Decimal
import sys
import unittest

sys.path.insert(0, sys.argv.pop(1))
from asterion_lab.dominant import Month, plan  # noqa: E402

DAYS = ["2026-03-02", "2026-03-03", "2026-03-04", "2026-03-05", "2026-03-06", "2026-03-09"]


def month(contract, settlements, interests, traded_days=None):
    result = Month(contract, contract.rsplit("/", 1)[1])
    for day, price, interest in zip(DAYS, settlements, interests):
        if price is None:
            continue
        result.settlement[day] = Decimal(price)
        result.open_interest[day] = Decimal(interest)
    days = [day for day, price in zip(DAYS, settlements) if price is not None]
    result.last_bar = max(traded_days or days)
    return result


class Dominant(unittest.TestCase):
    def test_the_most_held_month_leads_from_the_next_day_and_earlier_ones_are_scaled(self):
        # May holds more until October overtakes it on the third day.
        may = month("SHFE/rb/2026-05", ["3100", "3110", "3120", "3130", "3140", "3150"],
                    [500, 450, 300, 200, 100, 50])
        october = month("SHFE/rb/2026-10", ["3200", "3210", "3220", "3230", "3240", "3250"],
                        [300, 400, 400, 450, 500, 550])
        result = plan([may, october], traded=False)
        # The first day only says what was held.
        self.assertEqual([(s.month, s.begin) for s in result.segments],
                         [(0, "2026-03-03"), (1, "2026-03-05")])
        # October over May at the settlements of the day before the roll: 3220 / 3120.
        self.assertEqual(result.ratios[1], Decimal("1.03205128"))
        self.assertEqual(result.segments[0].factor, Decimal("1.03205128"))
        self.assertEqual(result.segments[1].factor, Decimal(1))

    def test_a_month_still_dominant_on_its_last_day_hands_over_on_the_next(self):
        # April holds the most until it is gone; nothing of it is left on the fourth day.
        april = month("SHFE/cu/2026-04", ["70000", "70100", "70200", None, None, None],
                      [500, 500, 500, 0, 0, 0])
        june = month("SHFE/cu/2026-06", [str(72000 + 100 * d) for d in range(6)], [100] * 6)
        read = plan([april, june], traded=False)
        self.assertEqual(read.segments[1].begin, "2026-03-05")
        # June over April at the settlements of April's last day: 72200 / 70200.
        self.assertEqual(read.ratios[1], Decimal("1.02849003"))
        # A traded series must leave April on its last day, while it still trades.
        traded = plan([april, june], traded=True)
        self.assertEqual(traded.segments[1].begin, "2026-03-04")
        # June over April at the settlements of the day before: 72100 / 70100.
        self.assertEqual(traded.ratios[1], Decimal("1.02853067"))

    def test_a_traded_series_refuses_a_roll_away_from_a_month_without_bars(self):
        may = month("SHFE/rb/2026-05", ["3100", "3110", "3120", "3130", "3140", "3150"],
                    [500, 450, 300, 200, 100, 50], traded_days=["2026-03-03"])
        october = month("SHFE/rb/2026-10", ["3200", "3210", "3220", "3230", "3240", "3250"],
                        [300, 400, 400, 450, 500, 550])
        with self.assertRaisesRegex(ValueError, "no bars from the roll day on"):
            plan([may, october], traded=True)


if __name__ == "__main__":
    unittest.main()
