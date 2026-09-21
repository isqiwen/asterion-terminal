"""Acceptance tools exercise real publication machinery with explicit offline transport fixtures."""

import importlib.util
import json
import sys
from datetime import date, datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

import pytest

from asterion.contract_roles.public import NextOpening, next_opening
from asterion.data.providers.tushare import Tushare
from asterion.trading_time.public import TimeSpec, TimeVersion, time_id

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / (name + ".py"))
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


acceptance = load("verify_tushare")
computed = load("verify_tushare_computed_roles")
mapping = load("verify_tushare_roles")


def case():
    now = datetime.now(ZoneInfo("Asia/Shanghai"))
    day = now.date() - timedelta(days=1)
    calendar_start = day - timedelta(days=2)
    time = TimeSpec.model_validate(
        {
            "schema_version": 1,
            "exchange": "SHFE",
            "product": "RB",
            "title": "Explicit offline test calendar",
            "timezone": "Asia/Shanghai",
            "calendar_source": "offline fixture, all dates open",
            "night_source": "offline fixture, no night",
            "calendar": [
                {
                    "date": (calendar_start + timedelta(days=i)).isoformat(),
                    "is_open": True,
                    "night_open": False,
                }
                for i in range(7)
            ],
            "periods": [
                {
                    "start": (day - timedelta(days=1)).isoformat(),
                    "end": (day + timedelta(days=4)).isoformat(),
                    "source": "offline fixture",
                    "day": [
                        {
                            "start": "09:00:00",
                            "end": "15:00:00",
                            "end_offset": 0,
                            "phase": "continuous",
                        }
                    ],
                    "night": [],
                }
            ],
            "exceptions": [],
        }
    )
    fixed = TimeVersion(id=time_id(time), spec=time)
    observation_end = datetime.combine(day, datetime.min.time().replace(hour=15), now.tzinfo)
    opening = next_opening(
        NextOpening(
            product_id="SHFE.RB",
            trading_time=fixed,
            observation_end=observation_end,
            available_at=now,
        )
    )
    year = now.year + 2
    return computed.ComputationCase(
        schema_version=1,
        exchange="SHFE",
        product_id="SHFE.RB",
        candidate_scope={
            "mode": "explicit",
            "symbols": (f"RB{year % 100:02}05.SHF", f"RB{year % 100:02}10.SHF"),
        },
        observation_day=day,
        initial_main=None,
        policy={
            "metric": "open_interest",
            "unit": "contracts",
            "tie_break": "earlier_delivery",
            "missing": "reject",
            "no_trade": "reject",
            "switch_margin": "0.1",
            "confirmations": 2,
            "allow_backward": False,
            "secondary": "best_remaining",
        },
        trading_time=fixed,
        expected_effective_start=opening.start,
    )


def rows(partition, example):
    if partition.api == "fut_basic":
        values = []
        for symbol in example.candidate_scope.symbols:
            year = example.observation_day.year + 2
            month = symbol.split(".")[0][-2:]
            values.append(
                {
                    "ts_code": symbol,
                    "symbol": symbol.split(".")[0],
                    "exchange": "SHFE",
                    "name": "offline fixture",
                    "fut_code": "RB",
                    "d_month": f"{year}{month}",
                    "list_date": f"{year - 3}0101",
                    "delist_date": f"{year}{month}15",
                    "last_ddate": f"{year}{month}20",
                    "per_unit": 10,
                    "trade_unit": "吨",
                    "quote_unit": "人民币元/吨",
                    "quote_unit_desc": "1人民币元/吨",
                }
            )
    elif partition.api == "fut_trade_cal":
        start, end = date.fromisoformat(partition.start), date.fromisoformat(partition.end)
        assert end <= example.observation_day
        values = [
            {
                "exchange": "SHFE",
                "cal_date": (start + timedelta(days=i)).strftime("%Y%m%d"),
                "is_open": 1,
                "pretrade_date": (start + timedelta(days=i - 1)).strftime("%Y%m%d"),
            }
            for i in range((end - start).days + 1)
        ]
    elif partition.api == "fut_daily":
        symbol = partition.params["ts_code"]
        values = [
            {
                "ts_code": symbol,
                "trade_date": example.observation_day.strftime("%Y%m%d"),
                "open": 100,
                "close": 100,
                "high": 100,
                "low": 100,
                "vol": 100,
                "oi": 200 if symbol == example.candidate_scope.symbols[0] else 300,
            }
        ]
    else:
        raise AssertionError("unexpected API")
    return [{k: row.get(k) for k in partition.fields} for row in values]


def test_real_sync_contract_computed_publication_and_backup(tmp_path, monkeypatch):
    example = case()
    monkeypatch.setattr(
        Tushare, "fetch", lambda self, partition, configuration: rows(partition, example)
    )
    report = acceptance.Report(tmp_path / "acceptance", example)
    computed.suite(report, example, {"token": "fixture-computed-secret"})
    value = json.loads((report.root / "report.json").read_bytes())
    assert value["status"] == "PASSED" and value["exact_reproduction"]
    assert not value["live_session_observed"] and not value["historical_knowledge_attested"]
    assert not value["whole_market_dominance_verified"]
    assert value["decision"]["reason"] == "initial"
    assert value["steps"][-1]["name"] == "backup_sources"
    for path in report.root.rglob("*"):
        if path.is_file():
            assert b"fixture-computed-secret" not in path.read_bytes()


def test_failure_keeps_evidence_and_redacts_transport_exception(tmp_path, monkeypatch):
    example = case()

    def fetch(self, partition, configuration):
        if partition.api == "fut_daily":
            raise RuntimeError("sensitive-transport-fixture")
        return rows(partition, example)

    monkeypatch.setattr(Tushare, "fetch", fetch)
    report = acceptance.Report(tmp_path / "failed", example)
    with pytest.raises(RuntimeError):
        computed.suite(report, example, {"token": "fixture-computed-secret"})
    value = json.loads((report.root / "report.json").read_bytes())
    assert value["status"] == "FAILED"
    assert set(value["versions"]) == {"contracts", "calendar"}
    assert value["steps"][-1]["error_type"] == "RuntimeError"
    assert "sensitive-transport-fixture" not in json.dumps(value)


def test_mapping_acceptance_requires_real_transition_and_complete_days():
    from test_contract_roles import fixture

    version = fixture()
    example = mapping.MappingCase(
        schema_version=1,
        exchange="SHFE",
        symbol="AU.SHF",
        start="2025-04-14",
        end="2025-04-14",
        trading_time=version.spec.trading_time,
        minimum_transitions=1,
    )
    with pytest.raises(ValueError, match="变化次数不足"):
        mapping.verify_transitions(version, example)
    assert (
        mapping.verify_transitions(version, example.model_copy(update={"minimum_transitions": 0}))
        == []
    )
    with pytest.raises(ValueError, match="未完整覆盖"):
        mapping.verify_transitions(version, example.model_copy(update={"start": date(2025, 4, 11)}))


@pytest.mark.parametrize("missing", [False, True])
def test_product_scope_discovers_every_candidate_and_rejects_missing_daily(
    tmp_path, monkeypatch, missing
):
    from copy import deepcopy

    base = case()
    example = computed.ComputationCase.model_validate(
        base.model_dump() | {"candidate_scope": {"mode": "product_catalog", "symbols": []}}
    )
    third = base.candidate_scope.symbols[0].replace("05.SHF", "11.SHF")
    fetched = []

    def fetch(self, partition, configuration):
        result = rows(partition, base)
        if partition.api == "fut_basic":
            extra = deepcopy(result[0])
            extra.update(
                ts_code=third,
                symbol=third.split(".")[0],
                d_month=f"{base.observation_day.year + 2}11",
                delist_date=f"{base.observation_day.year + 2}1115",
                last_ddate=f"{base.observation_day.year + 2}1120",
            )
            result.append(extra)
        elif partition.api == "fut_daily":
            fetched.append(partition.params["ts_code"])
            if missing and partition.params["ts_code"] == third:
                return []
        return result

    monkeypatch.setattr(Tushare, "fetch", fetch)
    report = acceptance.Report(tmp_path / "product", example)
    if missing:
        with pytest.raises(ValueError):
            computed.suite(report, example, {"token": "fixture-product-secret"})
        assert not (report.root / "computed-roles.json").exists()
    else:
        computed.suite(report, example, {"token": "fixture-product-secret"})
        saved = json.loads((report.root / "computed-roles.json").read_bytes())
        assert saved["spec"]["candidates"]["coverage"] == "fixed_source_product"
        assert len(saved["spec"]["candidates"]["included"]) == 3
    assert set(fetched) == {*base.candidate_scope.symbols, third}
    evidence = json.loads((report.root / "candidates.json").read_bytes())
    assert len(evidence["included"]) == 3
    assert not evidence["exchange_completeness_attested"]
