"""Tushare mapping, acquisition and the warehouse, against a scripted vendor.

Run with the bundled interpreter: python3 -I -B warehouse_test.py <source> <generated-python>
"""
from decimal import Decimal
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest

sys.path[:0] = [sys.argv.pop(1), sys.argv.pop(1)]
from asterion_lab.acquisition import Acquisition  # noqa: E402
from asterion_lab.tushare import Budget, SourceError, Tushare  # noqa: E402
from asterion_lab.warehouse import Warehouse  # noqa: E402

TOKEN = "fixture-token-0123456789"


class Log:
    def write(self, *args, **kwargs):
        pass


def table(fields, rows):
    return {"code": 0, "msg": "", "data": {"fields": fields, "items": rows}}


CONTRACT_FIELDS = ["ts_code", "exchange", "name", "fut_code", "multiplier", "trade_unit", "per_unit",
                   "quote_unit", "quote_unit_desc", "d_mode_desc", "list_date", "delist_date",
                   "d_month", "last_ddate", "trade_time_desc"]
DAILY_FIELDS = ["ts_code", "trade_date", "pre_close", "pre_settle", "open", "high", "low", "close",
                "settle", "vol", "amount", "oi", "oi_chg", "delv_settle"]


def contract(code, exchange, product, month, listed, delisted):
    return [code, exchange, product, product.upper(), 10, "吨", 10, "元/吨", "1元/吨", "实物交割",
            listed, delisted, month, None, "21:00-23:00"]


class Vendor:
    """Answers like Tushare, from the requests it is sent. Raw text keeps
    decimals as Tushare writes them."""

    def __init__(self):
        self.calls = []
        self.daily = {}

    def __call__(self, body: bytes) -> bytes:
        request = json.loads(body)
        assert request["token"] == TOKEN
        api, params = request["api_name"], request["params"]
        self.calls.append((api, params))
        exchange = params.get("exchange")
        if api == "trade_cal":
            year = params["start_date"][:4]
            rows = [[exchange, f"{year}0105", 1, None], [exchange, f"{year}0106", 1, f"{year}0105"],
                    [exchange, f"{year}0107", 0, f"{year}0106"]]
            return json.dumps(table(["exchange", "cal_date", "is_open", "pretrade_date"], rows)).encode()
        if api == "fut_basic":
            rows = {"SHFE": [contract("RB2605.SHF", "SHFE", "rb", "202605", "20250516", "20260515"),
                             contract("RB.SHF", "SHFE", "rb", "", "20000101", "20991231")],
                    "CZCE": [contract("AP003.ZCE", "CZCE", "ap", "202003", "20190301", "20200316"),
                             contract("AP1003.ZCE", "CZCE", "ap", "201003", "20090301", "20100316")]}
            return json.dumps(table(CONTRACT_FIELDS, rows.get(exchange, []))).encode()
        if api == "fut_daily":
            text = self.daily.get((exchange, params["trade_date"]))
            return (text or json.dumps(table(DAILY_FIELDS, []))).encode()
        raise AssertionError(api)


def daily_text(rows):
    # Hand-written JSON keeps the vendor's decimal spelling (3089.0, 1.5e3).
    items = ",".join("[" + ",".join(row) + "]" for row in rows)
    return '{"code":0,"msg":"","data":{"fields":%s,"items":[%s]}}' % (json.dumps(DAILY_FIELDS), items)


class Mapping(unittest.TestCase):
    def client(self, vendor):
        return Tushare(TOKEN, Budget(500), post=vendor)

    def test_contracts_keep_dated_codes_and_name_the_delivery_month(self):
        listings = self.client(Vendor()).contracts("SHFE")
        self.assertEqual([listing.contract for listing in listings], ["SHFE/rb/2026-05"])
        self.assertEqual(listings[0].row["multiplier"], Decimal(10))

    def test_czce_codes_repeat_every_decade_and_the_listed_one_is_meant(self):
        vendor = Vendor()
        vendor.daily[("CZCE", "20200105")] = daily_text([[
            '"AP1003.ZCE"', '"20200105"', "7000", "7001", "0", "0", "0", "0", "7002", "3",
            "0.0021", "10", "-1", "null"]])
        rows = self.client(vendor).daily("CZCE", "2020-01-05")
        self.assertEqual(rows[0]["contract"], "CZCE/ap/2020-03")
        # Zeros of CZCE are the absence of a trade, not a price.
        self.assertEqual([rows[0][name] for name in ("open", "high", "low", "close")], [None] * 4)
        self.assertEqual(rows[0]["settlement"], Decimal("7002"))
        self.assertEqual(rows[0]["amount"], Decimal("21"))  # published in ten thousands

    def test_vendor_series_are_left_out_and_decimals_stay_exact(self):
        vendor = Vendor()
        vendor.daily[("SHFE", "20260105")] = daily_text([
            ['"RB2605.SHF"', '"20260105"', "3089.0", "3090", "3091", "3100.5", "3080", "1.5e3",
             "3095", "100", "309.5", "2000", "5", "null"],
            ['"RB.SHF"', '"20260105"', "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "1", "null"]])
        rows = self.client(vendor).daily("SHFE", "2026-01-05")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["close"], Decimal("1500"))
        self.assertEqual(rows[0]["high"], Decimal("3100.5"))

    def test_a_full_answer_may_be_cut_short_and_is_refused(self):
        vendor = Vendor()
        row = ['"RB2605.SHF"', '"20260105"'] + ["1"] * 11 + ["null"]
        vendor.daily[("SHFE", "20260105")] = daily_text([row] * 2000)
        with self.assertRaisesRegex(SourceError, "limit of 2000 rows"):
            self.client(vendor).daily("SHFE", "2026-01-05")

    def test_a_refusal_never_repeats_the_token(self):
        def refuse(body):
            return json.dumps({"code": 40001, "msg": f"token {TOKEN} 不正确"}).encode()
        with self.assertRaises(SourceError) as caught:
            Tushare(TOKEN, Budget(500), post=refuse).calendar("SHFE", 2026)
        self.assertEqual(caught.exception.code, "invalid_credential")
        self.assertNotIn(TOKEN, str(caught.exception))


class Acquiring(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.warehouse = Warehouse(Path(self.folder.name))
        self.vendor = Vendor()
        self.vendor.daily[("SHFE", "20260105")] = daily_text([[
            '"RB2605.SHF"', '"20260105"', "3089", "3090", "3091", "3100", "3080", "3095", "3094",
            "100", "309.5", "2000", "5", "null"]])
        self.vendor.daily[("SHFE", "20260106")] = daily_text([[
            '"RB2605.SHF"', '"20260106"', "3095", "3094", "3096", "3110", "3090", "3105", "3101",
            "120", "370.1", "2010", "10", "null"]])

    def tearDown(self):
        self.folder.cleanup()

    def run_task(self, identity):
        acquisition = Acquisition(self.warehouse, Log())
        task = acquisition.submit(identity, "2026-01-01", "2026-01-31",
                                  lambda cancelled: Tushare(TOKEN, Budget(500), post=self.vendor,
                                                            cancelled=cancelled))
        for thread in threading.enumerate():
            if thread.name == f"acquisition-{identity}":
                thread.join(30)
        return task

    def test_a_task_stores_a_version_and_a_second_asks_only_what_is_missing(self):
        task = self.run_task("first")
        self.assertEqual(task.state, "succeeded", task.error)
        # Six exchanges, two open days each; only SHFE has rows.
        self.assertEqual((task.days_total, task.days_empty), (12, 10))
        first = self.warehouse.current()
        self.assertEqual(task.version, first.identity)
        self.assertEqual(first.days("SHFE"), {"2026-01-05", "2026-01-06"})
        bars = self.warehouse.query(first, "select day, close from daily order by day", ("daily",))
        self.assertEqual([str(close) for _, close in bars], ["3095.000000", "3105.000000"])
        self.vendor.calls.clear()
        second = self.run_task("second")
        self.assertEqual(second.state, "succeeded", second.error)
        daily = [params for api, params in self.vendor.calls if api == "fut_daily"]
        self.assertEqual(len(daily), 10, "stored days are not asked again; empty ones are")
        # The first version and its files are kept as they were.
        self.assertTrue(all((self.warehouse.root / entry["file"]).exists()
                            for entry in first.manifest["partitions"].values()))

    def test_a_number_beyond_six_decimals_fails_the_task_without_rounding(self):
        self.vendor.daily[("SHFE", "20260105")] = daily_text([[
            '"RB2605.SHF"', '"20260105"', "1", "1", "1", "1", "1", "1.0000001", "1", "1", "1",
            "1", "1", "null"]])
        task = self.run_task("precise")
        self.assertEqual(task.state, "failed")
        self.assertIn("six decimal places", task.error)


if __name__ == "__main__":
    unittest.main()
