"""Trading sessions from the reviewed reference.

Run with the bundled interpreter: python3 -I -B sessions_test.py <source>
"""
import datetime
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, sys.argv.pop(1))
from asterion_lab.sessions import REFERENCE, Sessions, holiday_between  # noqa: E402

D = datetime.date


class Reference(unittest.TestCase):
    sessions = Sessions()

    def night(self, exchange, product, evening, trading_day):
        return self.sessions.night(exchange, product, evening, holiday_between(evening, trading_day))

    def test_dce_shortened_its_night_on_2019_03_29(self):
        before = self.night("DCE", "i", D(2019, 3, 27), D(2019, 3, 28))
        self.assertEqual((before.state, before.end), ("held", "2019-03-27 23:30"))
        after = self.night("DCE", "i", D(2019, 3, 29), D(2019, 4, 1))
        self.assertEqual((after.state, after.end), ("held", "2019-03-29 23:00"))
        self.assertIn("dce-2019-hours", after.sources)

    def test_a_friday_night_runs_past_midnight_for_monday(self):
        night = self.night("SHFE", "au", D(2025, 10, 31), D(2025, 11, 3))
        self.assertEqual((night.begin, night.end), ("2025-10-31 21:00", "2025-11-01 02:30"))

    def test_no_night_before_a_statutory_holiday(self):
        # 2025-09-30 is followed by the National Day holiday; the weekend alone is not.
        self.assertEqual(self.night("SHFE", "cu", D(2025, 9, 30), D(2025, 10, 9)).state,
                         "holiday_eve")
        self.assertEqual(self.night("DCE", "m", D(2025, 9, 26), D(2025, 9, 29)).state, "held")

    def test_what_no_document_establishes_is_unknown(self):
        # Rebar's night changed from 01:00 to 23:00 on a day not established.
        self.assertEqual(self.night("SHFE", "rb", D(2018, 1, 2), D(2018, 1, 3)).state, "unknown")
        self.assertEqual(self.night("SHFE", "rb", D(2023, 1, 3), D(2023, 1, 4)).end,
                         "2023-01-03 23:00")
        # The 2020 suspension.
        self.assertEqual(self.night("DCE", "p", D(2020, 3, 2), D(2020, 3, 3)).state, "unknown")
        self.assertFalse(self.sessions.day("SHFE", "cu", D(2025, 1, 2)).known)
        self.assertEqual(self.night("CZCE", "ma", D(2023, 6, 1), D(2023, 6, 2)).state, "unknown")

    def test_products_without_a_night_session(self):
        self.assertEqual(self.night("GFEX", "lc", D(2025, 6, 2), D(2025, 6, 3)).state, "none")
        index = self.sessions.day("CFFEX", "if", D(2025, 6, 3))
        self.assertEqual(index.segments[0], ("2025-06-03 09:30", "2025-06-03 11:30"))

    def test_overlapping_rules_are_refused(self):
        document = json.loads(REFERENCE.read_text(encoding="utf-8"))
        document["night_sessions"].append(
            {"exchange": "DCE", "products": ["i"], "from": "2020-01-01", "basis": "start",
             "end": "23:30", "sources": ["dce-2019-hours"]})
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "sessions.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "overlap"):
                Sessions(path)


if __name__ == "__main__":
    unittest.main()
