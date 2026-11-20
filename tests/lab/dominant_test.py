"""The dominant series rule.

Run with the bundled interpreter: python3 -I -B dominant_test.py <source>
"""
from decimal import Decimal
import sys
import unittest

sys.path.insert(0, sys.argv.pop(1))
from asterion_lab.dominant import Month, annualized_roll_yield, plan  # noqa: E402

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
        # Ending the requested window on April's last day keeps the same handover.
        short = plan([april, june], traded=True, end="2026-03-04")
        self.assertEqual([(s.month, s.begin) for s in short.segments],
                         [(s.month, s.begin) for s in traded.segments])
        self.assertEqual(short.ratios, traded.ratios)

    def test_a_traded_series_refuses_a_roll_away_from_a_month_without_bars(self):
        may = month("SHFE/rb/2026-05", ["3100", "3110", "3120", "3130", "3140", "3150"],
                    [500, 450, 300, 200, 100, 50], traded_days=["2026-03-03"])
        october = month("SHFE/rb/2026-10", ["3200", "3210", "3220", "3230", "3240", "3250"],
                        [300, 400, 400, 450, 500, 550])
        with self.assertRaisesRegex(ValueError, "no bars from the roll day on"):
            plan([may, october], traded=True)

    def test_second_dominant_picks_most_held_later_month_and_roll_yield_sign(self):
        may = month("SHFE/rb/2026-05", ["3500"] * 6, [800, 700, 200, 100, 50, 20])
        october = month("SHFE/rb/2026-10", ["3400"] * 6, [300, 350, 600, 550, 500, 450])
        january = month("SHFE/rb/2027-01", ["3350"] * 6, [400, 200, 250, 300, 350, 400])
        result = plan([may, october, january], traded=True)
        # Day 1 (prev day 0): may dominant, january (400) > october (300) -> second is january (2).
        # Day 2 (prev day 1): may dominant, october (350) > january (200) -> second is october (1).
        # Day 3+ (prev day 2+): october dominant, only later month is january (2).
        self.assertEqual(result.second[DAYS[1]], 2)
        self.assertEqual(result.second[DAYS[2]], 1)
        self.assertEqual(result.second[DAYS[3]], 2)

        # Last month dominant has no later second-dominant.
        only_two = plan([may, october], traded=True)
        self.assertEqual(only_two.second[DAYS[1]], 1)
        self.assertNotIn(DAYS[3], only_two.second)

        # Backwardation (near > far) is positive; contango (near < far) is negative.
        back = annualized_roll_yield(Decimal("3500"), Decimal("3400"), "2026-05", "2026-10")
        cont = annualized_roll_yield(Decimal("3400"), Decimal("3500"), "2026-05", "2026-10")
        self.assertGreater(back, Decimal(0))
        self.assertEqual(back, -cont)
        self.assertEqual(
            annualized_roll_yield(Decimal("3500"), Decimal("3500"), "2026-05", "2026-10"),
            Decimal("0.00000000"),
        )
        with self.assertRaises(ValueError):
            annualized_roll_yield(Decimal("3500"), Decimal("3400"), "2026-10", "2026-05")

    def test_delist_date_controls_expiry_roll_without_future_settlement_rows(self):
        # Window ends on 2026-03-04; april delists on 2026-03-04, june delists later.
        april = month("SHFE/cu/2026-04", ["70000", "70100", "70200"], [500, 500, 500])
        june = month("SHFE/cu/2026-06", ["72000", "72100", "72200"], [100, 100, 100])
        april.delist_date = "2026-03-04"
        june.delist_date = "2026-06-15"
        rolled = plan([april, june], traded=True, end="2026-03-04")
        self.assertEqual([(s.month, s.begin) for s in rolled.segments],
                         [(0, "2026-03-03"), (1, "2026-03-04")])
        # When april has NOT reached delist_date yet, it stays dominant on 2026-03-04 even if
        # june has extra settlement rows later in the warehouse.
        april.delist_date = "2026-04-15"
        june_extra = month("SHFE/cu/2026-06", [str(72000 + 100 * d) for d in range(6)], [100] * 6)
        june_extra.delist_date = "2026-06-15"
        kept = plan([april, june_extra], traded=True, end="2026-03-04")
        self.assertEqual([(s.month, s.begin) for s in kept.segments], [(0, "2026-03-03")])


if __name__ == "__main__":
    unittest.main()
