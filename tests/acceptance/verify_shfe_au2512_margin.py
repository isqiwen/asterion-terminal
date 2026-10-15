"""Verify the bounded SHFE au2512 evidence capture offline; never fetch or import data."""
import hashlib
import json
from pathlib import Path
import sys

capture = Path(sys.argv[1])
fixture_path = Path(__file__).resolve().parents[1] / "data/fixtures/shfe_au2512_margin.json"
fixture = json.loads(fixture_path.read_text())
sources = json.loads((capture / "sources.json").read_text())
for record in sources:
    body = (capture / record["file"]).read_bytes()
    assert hashlib.sha256(body).hexdigest() == record["sha256"], record["url"]
    assert len(body) == record["bytes"] and record["http_status"] == 200

expected = ["0.14", "0.14", "0.16", "0.16", "0.16"]
for event, rate in zip(fixture["events"], expected, strict=True):
    record = event["source"]
    assert record in sources
    original = json.loads((capture / record["file"]).read_text(), parse_float=str)
    key = {"settlement": "Settlement", "trading_parameters": "ContractDailyTradeArgument",
           "settlement_crosscheck": "o_cursor"}[event["kind"]]
    rows = [row for row in original[key] if row["INSTRUMENTID"] == "au2512"]
    assert rows == [event["raw_au2512_row"]]
    assert original["report_date"] == event["report_date"]
    assert original["update_date"] == event["update_date"]
    assert rows[0].get("TRADINGDAY", original["report_date"]) == event["report_date"]
    assert rows[0][event["long_field"]] == rows[0][event["short_field"]] == rate
    assert event["long_rate"] == event["short_rate"] == rate

html = (capture / "raw/prmsummary.html").read_text()
start = html.index('id="settlement-f-component"')
template = html[start:html.index("</script>", start)]
for side, label in [("LONG", "买"), ("SHORT", "卖")]:
    assert f"一般持仓{label}保证金率(%)" in template
    assert f"SPEC_{side}MARGINRATIO*100" in template
assert "api_option_settlement_f(vm.current_date)" in (
    capture / "raw/generateData_settlement_f.js").read_text()
assert "'/data/busiparamdata/future/Settlement' + date + '.dat" in (
    capture / "raw/api.js").read_text()
config = json.loads((capture / "raw/report-config.dat").read_text())
assert any(note["begineffectivedate"] <= "20251021" <= note["endeffectivedate"] and
           "交易时段内本表数据为上一交易日的结算参数" in note["propvalue"] and
           "当日结算参数将在当日日终结算完成后正式发布" in note["propvalue"]
           for note in config["shfe_settlement"]["report_1_note_total"])
settlement, crosscheck = fixture["events"][2], fixture["events"][4]
assert settlement["update_date"] == "20251021 15:50:00"
assert crosscheck["update_date"] == "20251021 15:25:02"
assert settlement["raw_au2512_row"]["SETTLEMENTPRICE"] == "991.44"
assert settlement["raw_au2512_row"]["LONGMARGINRATIO"] == "0.15"
assert settlement["raw_au2512_row"]["SHORTMARGINRATIO"] == "0.15"
print(json.dumps({"verified_public_files": len(sources), "verified_au2512_rows": 5,
                  "contract": fixture["contract"], "fixture_sha256": hashlib.sha256(
                      fixture_path.read_bytes()).hexdigest(),
                  "speculative_rates": expected, "unit": "fraction",
                  "separate_stages": True, "first_publication_proved": False,
                  "tushare_mapping_proved": False, "network_requests": 0,
                  "warehouse_writes": 0}, ensure_ascii=False, indent=2))
