"""Warehouse data quality audit tests.

Run with the bundled interpreter:
python3 -I -B audit_test.py <source>
"""
import datetime
from decimal import Decimal
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "apps/services/common/python"))
sys.path[:0] = [sys.argv.pop(1), str(Path(__file__).parent)]

from asterion_lab import audit  # noqa: E402
from warehouse_fixture import DAYS, warehouse  # noqa: E402
from warehouse_writer import Warehouse  # noqa: E402


def _minute_rows(contract: str, day: datetime.date, close: Decimal, total_vol: int, bad_bar: bool = False):
    start = int(datetime.datetime.combine(day, datetime.time(9),
                datetime.timezone(datetime.timedelta(hours=8))).timestamp()) * 1000
    per = total_vol // 5
    rem = total_vol - per * 5
    rows = []
    for i in range(5):
        c = close * Decimal("1.30") if (bad_bar and i == 3) else close
        rows.append(dict(
            exchange="SHFE", product="rb", contract=contract, day=day.isoformat(),
            start_ms=start + i * 60_000, partial=False,
            open=c, high=c, low=c, close=c,
            volume=Decimal(per + (rem if i == 4 else 0)),
            average_price=None, open_interest=Decimal(1000),
        ))
    return rows


class AuditTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.lab_dir = Path(self.tmp.name) / "lab"
        self.lab_dir.mkdir()
        warehouse(self.lab_dir)

    def tearDown(self):
        self.tmp.cleanup()

    def test_clean_dataset_passes_all_checks(self):
        writer = Warehouse(self.lab_dir).writer()
        # Add matching minute partitions for days 1..3 (daily volume in fixture is 100).
        for day in DAYS[1:4]:
            writer.put_rows(
                f"minutes/SHFE/{day.isoformat()}/rb/2026-05", "minutes",
                _minute_rows("SHFE/rb/2026-05", day, Decimal("3000"), 100),
                days=[day.isoformat()],
            )
        version = writer.commit()
        # Fixture jumps OI 1000 -> 100 on the synthetic roll day; test clean pass above that ratio.
        report = audit.audit(
            audit.LocalWarehouse(self.lab_dir),
            version_id=version.identity,
            oi_jump_ratio=Decimal("12"),
        )
        self.assertTrue(report["ok"], report)
        self.assertEqual(report["summary"]["minute_partitions_checked"], 3)
        self.assertIn("通过", audit.format_report(report))

    def test_detects_missing_days_volume_mismatch_price_and_oi_jumps(self):
        writer = Warehouse(self.lab_dir).writer()
        # 1) Remove DAYS[5] from daily/SHFE/2026-01 to create a missing trading day,
        #    and inject a +50% daily price jump + 10x OI jump + invalid OHLC on DAYS[10].
        store = Warehouse(self.lab_dir)
        jan_rows = [dict(r, day=r["day"].isoformat()) for r in store.read(store.current(), "daily/SHFE/2026-01").to_dicts()]
        jan_rows = [r for r in jan_rows if r["day"] != DAYS[5].isoformat()]
        for r in jan_rows:
            if r["contract"] == "SHFE/rb/2026-05" and r["day"] == DAYS[10].isoformat():
                r["close"] = Decimal("5000")
                r["high"] = Decimal("2900")  # invalid: high < close
                r["open_interest"] = Decimal("20000")  # > 5x jump from 1000
        writer.put_rows(
            "daily/SHFE/2026-01", "daily", jan_rows,
            days=sorted({r["day"] for r in jan_rows}),
        )
        # 2) Add minute partitions on DAYS[1] (volume mismatch: 125 vs daily 100 + intraday 30% jump)
        #    and DAYS[3] (leaving DAYS[2] missing!).
        writer.put_rows(
            f"minutes/SHFE/{DAYS[1].isoformat()}/rb/2026-05", "minutes",
            _minute_rows("SHFE/rb/2026-05", DAYS[1], Decimal("3000"), 125, bad_bar=True),
            days=[DAYS[1].isoformat()],
        )
        writer.put_rows(
            f"minutes/SHFE/{DAYS[3].isoformat()}/rb/2026-05", "minutes",
            _minute_rows("SHFE/rb/2026-05", DAYS[3], Decimal("3000"), 100),
            days=[DAYS[3].isoformat()],
        )
        version = writer.commit()

        out_path = Path(self.tmp.name) / "audit.json"
        with self.assertRaises(SystemExit) as ctx:
            audit.main([
                "--warehouse-dir", str(self.lab_dir),
                "--version", version.identity,
                "--output", str(out_path),
            ])
        self.assertEqual(ctx.exception.code, 1)
        report = json.loads(out_path.read_text())
        self.assertFalse(report["ok"])
        self.assertEqual([d["day"] for d in report["missing_daily_days"]], [DAYS[5].isoformat()])
        self.assertEqual([d["day"] for d in report["missing_minute_days"]], [DAYS[2].isoformat()])
        self.assertEqual(len(report["volume_mismatches"]), 1)
        self.assertEqual(report["volume_mismatches"][0]["diff"], "25")
        kinds = {item["kind"] for item in report["price_anomalies"]}
        self.assertIn("ohlc_invalid", kinds)
        self.assertIn("price_jump", kinds)
        self.assertIn("minute_price_jump", kinds)
        self.assertEqual(report["open_interest_anomalies"][0]["kind"], "open_interest_jump")

    def test_lab_dir_cli_and_source_empty_markers(self):
        # 1) --lab-dir works both on the Lab root and directly on warehouse-cache/research.
        store = Warehouse(self.lab_dir)
        version = store.current()
        for target in (self.lab_dir, self.lab_dir / "warehouse-cache" / "research"):
            out_path = Path(self.tmp.name) / f"lab-audit-{target.name}.json"
            with self.assertRaises(SystemExit) as ctx:
                audit.main([
                    "--lab-dir", str(target),
                    "--version", version.identity,
                    "--oi-jump-ratio", "12",
                    "--output", str(out_path),
                ])
            self.assertEqual(ctx.exception.code, 0)
            self.assertTrue(json.loads(out_path.read_text())["ok"])

        # 2) Marked empty days in acquisition/empty-daily.txt are skipped, not flagged missing.
        writer = Warehouse(self.lab_dir).writer()
        jan_rows = [
            dict(r, day=r["day"].isoformat())
            for r in store.read(version, "daily/SHFE/2026-01").to_dicts()
            if r["day"] != DAYS[5]
        ]
        writer.put_rows("daily/SHFE/2026-01", "daily", jan_rows, days=sorted({r["day"] for r in jan_rows}))
        updated = writer.commit()
        acq = self.lab_dir / "acquisition"
        acq.mkdir(parents=True, exist_ok=True)
        (acq / "empty-daily.txt").write_text(f"daily/SHFE/{DAYS[5].isoformat()}\n")
        report = audit.audit(
            audit.LocalWarehouse(self.lab_dir),
            version_id=updated.identity,
            oi_jump_ratio=Decimal("12"),
        )
        self.assertTrue(report["ok"], report)
        self.assertEqual(report["summary"]["missing_daily_days"], 0)
        self.assertEqual(report["summary"]["source_empty_skipped"], 1)


if __name__ == "__main__":
    unittest.main()
