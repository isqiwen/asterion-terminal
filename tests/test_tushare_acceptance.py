import importlib.util
import json
from datetime import date, timedelta
from pathlib import Path

import pytest
from credential_helpers import provider_secrets

from asterion.data.providers.tushare import Tushare
from asterion.data.sync import Credentials

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("verify_tushare", ROOT / "scripts/verify_tushare.py")
assert spec and spec.loader
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)


def case():
    return acceptance.Case.model_validate_json(
        (ROOT / "examples/validation/tushare-rb2505.json").read_bytes()
    )


def rows(partition):
    if partition.api == "fut_basic":
        values = {
            "ts_code": "RB2505.SHF",
            "symbol": "RB2505",
            "exchange": "SHFE",
            "name": "测试合约",
            "fut_code": "RB",
            "d_month": "202505",
            "last_ddate": "20250520",
            "per_unit": 10,
            "trade_unit": "吨",
            "quote_unit": "人民币元/吨",
            "quote_unit_desc": "1人民币元/吨",
            "list_date": "20240516",
            "delist_date": "20250515",
        }
        return [{k: values.get(k) for k in partition.fields}]
    start, end = date.fromisoformat(partition.start), date.fromisoformat(partition.end)
    result = []
    while start <= end:
        opened = start.weekday() < 5
        values = {
            "exchange": "SHFE",
            "cal_date": start.isoformat(),
            "is_open": int(opened),
            "pretrade_date": (start - timedelta(days=3 if start.weekday() == 0 else 1)).isoformat(),
            "ts_code": "RB2505.SHF",
            "trade_date": start.isoformat(),
            "open": 3200 + start.day,
            "high": 3300,
            "low": 3100,
            "close": 3200 + start.day,
            "vol": 100,
            "settle": 3200,
            "trading_fee_rate": "0.1",
            "trading_fee": "0",
            "long_margin_rate": "0.07",
        }
        if partition.api == "fut_trade_cal" or opened:
            result.append({k: values.get(k) for k in partition.fields})
        start += timedelta(days=1)
    return result


def test_acceptance_retains_fixed_inputs_and_replays_offline(tmp_path, monkeypatch):
    monkeypatch.setattr(Tushare, "fetch", lambda self, partition, configuration: rows(partition))
    root = tmp_path / "result"
    report = acceptance.Report(root, case())
    acceptance.suite(report, case(), {"token": "fixture-private-token"})
    value = json.loads((root / "report.json").read_text())
    assert value["status"] == "PASSED" and value["exact_reproduction"]
    assert value["coverage"] == "COVERED"
    assert value["preparation"]["daily_state"] == "SUBMITTED"
    assert value["preparation"]["catalog_id"] == value["identity"]["release_id"]
    assert value["identity"]["contract_id"] == "SHFE.RB.202505.20240516"
    assert not value["identity"]["historical_knowledge_attested"]
    assert (root / "reference-catalog.json").exists()
    assert value["versions"]["daily"]["rows"] == 11
    assert value["steps"][-1] == {"name": "offline_replay", "status": "PASSED"}
    package = json.loads((root / "research.asterion.json").read_text())
    periods = package["content"]["request"]["rules"]["spec"]["periods"]
    assert all(
        p["start"] > p["settlement_basis"]["evidence"]["row"]["trading_day"] for p in periods
    )
    assert periods[0]["open_fee"] == "0.0001"
    assert (root / "runtime.key").stat().st_mode & 0o777 == 0o600
    for path in root.rglob("*"):
        if path.is_file():
            assert b"fixture-private-token" not in path.read_bytes()


def test_failed_stage_keeps_completed_evidence_and_redacts_exception(tmp_path, monkeypatch):
    def fetch(self, partition, configuration):
        if partition.api == "fut_basic":
            raise RuntimeError("private-token-and-payload")
        return rows(partition)

    monkeypatch.setattr(Tushare, "fetch", fetch)
    report = acceptance.Report(tmp_path / "result", case())
    with pytest.raises(RuntimeError):
        acceptance.suite(report, case(), {"token": "private-token-and-payload"})
    value = json.loads((report.root / "report.json").read_text())
    assert value["status"] == "FAILED"
    assert value["steps"][-1] == {
        "name": "contracts",
        "status": "FAILED",
        "error_type": "RuntimeError",
    }
    assert value["versions"]["calendar"]["rows"] == 15
    assert list((report.root / "sources").rglob("*.json"))
    assert "private-token-and-payload" not in json.dumps(value)


def test_output_refuses_overwrite(tmp_path):
    root = tmp_path / "result"
    root.mkdir()
    marker = root / "report.json"
    marker.write_text("preserve")
    with pytest.raises(FileExistsError):
        acceptance.Report(root, case())
    assert marker.read_text() == "preserve"


@pytest.mark.parametrize(
    "change",
    [
        {"settlement_start": "2025-01-06"},
        {"end": "2026-01-01"},
        {"fee_unit": "yuan_per_lot"},
        {"multiplier": "NaN"},
        {"symbol": "RB.SHF"},
    ],
)
def test_case_validation_precedes_network(change):
    with pytest.raises(ValueError):
        acceptance.Case.model_validate(case().model_dump() | change)


def test_reading_credentials_does_not_create_or_modify_directories(tmp_path):
    credentials = Credentials(tmp_path / "absent", provider_secrets("fixture-secret"))
    with pytest.raises(ValueError):
        credentials.read_configuration("a" * 64, "tushare", 1, 0)
    assert not (tmp_path / "absent").exists()
    ref = credentials.freeze_configuration("tushare", 1, {"token": "fixture"}, 1)
    credentials.root.chmod(0o750)
    assert credentials.read_configuration(ref, "tushare", 1, 1) == {"token": "fixture"}
    assert credentials.root.stat().st_mode & 0o777 == 0o750
