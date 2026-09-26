import json
from copy import deepcopy
from decimal import Decimal

import pytest
from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.execution import ExecutionFactory
from asterion_bindings.rules import RulePeriod, RuleSpec
from entry_support import entry_lifecycle
from fastapi.testclient import TestClient
from import_identity_support import source_identity
from rules_support import rule_version
from scan_support import unsupported_scan
from storage_support import raw_engine, scheduler
from test_data_sync import MASTER, context, prepared, request  # noqa: F401
from test_research import services as research_services  # noqa: F401
from test_research import version_access

from asterion.api.app import create_app
from asterion.contract_rules.public import RuleAccess
from asterion.contract_rules.service import Rules
from asterion.contract_rules.settlement import (
    SettlementConfirmation,
    SettlementMapping,
    SettlementRequest,
)
from asterion.data.providers.tushare import Tushare
from asterion.data.public import VersionAccess
from asterion.distribution import strategy_catalog
from asterion.distribution_storage import rule_storage
from asterion.platform.config import Settings
from asterion.platform.serialization import canonical
from asterion.research.engine import calculate
from asterion.research.packages import ResearchPackages


def publish(context, monkeypatch, dataset="settlement", **changes):  # noqa: F811
    def enrich(rows):
        rows[0].update(trading_fee_rate="0.050", trading_fee="3", long_margin_rate="12", **changes)
        return rows

    job, content = prepared(context, monkeypatch, request(dataset, symbol="RB2610.SHF"), enrich)
    return context[2].publish(job["id"], job["token"], content)


def confirmation(evidence, **changes):
    return SettlementConfirmation.model_validate(
        {
            "evidence": evidence.model_dump(mode="json"),
            "fee_field": "trading_fee_rate",
            "fee_unit": "percent",
            "margin_unit": "percent",
            "fee_scope": "long_open_and_non_today_close",
            "availability_assumption": "after_source_day",
            "interpretation": "测试：核实费率百分数；次日起研究假设，不证明历史公布时刻",
            "start": "2024-01-03",
            "end": "2024-01-31",
        }
        | changes
    )


def test_sync_retains_raw_values_and_separate_snapshot_versions(context, monkeypatch):  # noqa: F811
    _, _, sync, _ = context
    version = publish(context, monkeypatch)
    assert version["manifest"]["type"]["id"] == "futures.settlement"
    assert version["manifest"]["coverage"] == "RETURNED_ROWS_ONLY"
    result = sync.library.preview(version["id"])
    assert result["snapshot"] is None
    assert result["rows"][0]["trading_fee_rate"] == "0.050"
    assert result["rows"][0]["offset_today_fee"] is None
    raw = sync.library.preview(version["manifest"]["inputs"][0])
    assert raw["rows"][0]["trading_fee_rate"] == "0.050"
    assert "observed_at" in version["manifest"]
    parts = Tushare().plan(request("settlement", symbol="RB2610.SHF", end="2024-03-31"))
    assert all(p.api == "fut_settle" and p.limit == 1600 for p in parts)
    assert len(parts) == 3


@pytest.mark.parametrize(
    "mutation", ["negative", "missing", "duplicate", "exchange", "foreign", "date", "nan"]
)
def test_bad_settlement_is_not_published(context, monkeypatch, mutation):  # noqa: F811
    def corrupt(rows):
        if mutation == "negative":
            rows[0]["trading_fee"] = -1
        if mutation == "missing":
            del rows[0]["trading_fee"]
        if mutation == "duplicate":
            rows *= 2
        if mutation == "exchange":
            rows[0]["exchange"] = "DCE"
        if mutation == "foreign":
            rows[0]["ts_code"] = "CU2610.SHF"
        if mutation == "date":
            rows[0]["trade_date"] = "20240103"
        if mutation == "nan":
            rows[0]["trading_fee_rate"] = "NaN"
        return rows

    job, content = prepared(
        context, monkeypatch, request("settlement", symbol="RB2610.SHF"), corrupt
    )
    if mutation == "missing":
        evidence = json.loads(content)
        del evidence[0]["rows"][0]["trading_fee"]
        content = canonical(evidence)
    with pytest.raises(ValueError):
        context[2].publish(job["id"], job["token"], content)
    assert context[2].library.list(type_id="futures.settlement")["total"] == 0


def test_confirmation_units_time_authenticity_and_reference(context, monkeypatch):  # noqa: F811
    engine, _, _, root = context
    version = publish(context, monkeypatch)
    service = Rules(rule_storage(engine), version_access(engine, root))
    evidence = service.settlement.preview(
        SettlementRequest(
            version_id=version["id"],
            contract_id="SHFE.RB.202610.20230101",
            trading_day="2024-01-02",
        )
    )
    period = service.settlement.confirm(confirmation(evidence))
    assert period.open_fee == period.close_fee == Decimal("0.0005")
    assert period.margin_rate == Decimal("0.12")
    permille = service.settlement.confirm(confirmation(evidence, fee_unit="permille"))
    assert permille.open_fee == Decimal("0.00005")
    per_lot = service.settlement.confirm(
        confirmation(evidence, fee_field="trading_fee", fee_unit="yuan_per_lot")
    )
    assert per_lot.open_fee == 3 and per_lot.fee_mode == "per_lot"
    with pytest.raises(ValueError, match="盘后"):
        service.settlement.confirm(confirmation(evidence, start="2024-01-02"))
    with pytest.raises(ValueError, match="盘后"):
        service.settlement.confirm(confirmation(evidence, start="2024-01-01"))
    with pytest.raises(ValueError):
        confirmation(evidence, fee_unit="yuan_per_lot")
    with pytest.raises(ValueError):
        confirmation(evidence, interpretation="  ")
    with pytest.raises(ValueError):
        confirmation(evidence, margin_unit="ratio")
    spec = rule_version("SHFE.rb2610").spec.model_dump(mode="json")
    spec["periods"] = [period.model_dump(mode="json")]
    spec["contract"] = evidence.contract.model_dump(mode="json")
    saved = service.save(RuleSpec.model_validate(spec))
    assert service.read(saved.id) == saved
    for key, value in [("open_fee", "1"), ("margin_rate", "0.2")]:
        wrong = deepcopy(spec)
        wrong["periods"][0][key] = value
        with pytest.raises(ValueError, match="换算"):
            RuleSpec.model_validate(wrong)
    tampered = confirmation(evidence).model_dump(mode="json")
    tampered["evidence"]["checksum"] = "0" * 64
    with pytest.raises(ValueError, match="固定数据版本"):
        service.settlement.confirm(SettlementConfirmation.model_validate(tampered))
    with TestClient(create_app(Settings(token=MASTER, data_root=root), engine)) as client:
        url = "/api/v1/contract-rules/settlement/preview"
        body = {
            "version_id": version["id"],
            "contract_id": "SHFE.RB.202610.20230101",
            "trading_day": "2024-01-02",
        }
        assert client.post(url, json=body).status_code == 401
        client.headers["Authorization"] = "Bearer " + MASTER
        assert client.post(url, json=body).status_code == 200
        assert (
            client.post(
                "/api/v1/contract-rules/settlement/confirm",
                json=confirmation(evidence).model_dump(mode="json"),
            ).status_code
            == 200
        )
    with entry_lifecycle(str(engine.url), root) as manager:
        assert manager.inspect(version["id"])["references"]["contract_rules"] == 1


def test_null_is_unknown_and_cannot_confirm(context, monkeypatch):  # noqa: F811
    engine, _, _, root = context
    version = publish(context, monkeypatch, short_margin_rate="0.15")
    mapping = SettlementMapping(version_access(engine, root))
    evidence = mapping.preview(
        SettlementRequest(
            version_id=version["id"],
            contract_id="SHFE.RB.202610.20230101",
            trading_day="2024-01-02",
        )
    )
    evidence = evidence.model_copy(
        update={"row": evidence.row.model_copy(update={"long_margin_rate": None})}
    )
    with pytest.raises(ValueError, match="缺失"):
        confirmation(evidence)


def test_frozen_settlement_rules_replay_without_source(research_services):  # noqa: F811
    from asterion_bindings.rules import SettlementEvidence

    research, body, _ = research_services
    req = request("settlement", symbol="RB2405.SHF", start="2023-12-29", end="2023-12-29")
    fields = Tushare().plan(req)[0].fields
    raw = dict.fromkeys(fields) | {
        "ts_code": "RB2405.SHF",
        "exchange": "SHFE",
        "trade_date": "20231229",
        "trading_fee": 2,
        "long_margin_rate": "0.1",
    }
    rows = Tushare().normalize(req, [raw])
    source = {
        "version": {
            "id": "settlement-fixed",
            "manifest": {
                "scope": {"exchange": "SHFE"},
                "contract_identity": source_identity("SHFE.rb2405", "tushare", "RB2405.SHF"),
                "source": "tushare",
                "layer": "STANDARD",
                "type": {"id": "futures.settlement", "schema_version": 1},
                "checksum": "a" * 64,
                "origin": {"connection_id": None},
                "observed_at": "2026-09-20T00:00:00Z",
            },
        },
        "rows": rows,
        "total": 1,
    }
    port = VersionAccess(lambda *a, **kw: source, lambda _: None, unsupported_scan)
    rules = Rules(rule_storage(raw_engine(research.engine)), port)
    evidence = rules.settlement.preview(
        SettlementRequest(
            version_id="settlement-fixed",
            contract_id=body.rules.spec.contract.id,
            trading_day="2023-12-29",
        )
    )
    assert isinstance(evidence, SettlementEvidence)
    period = rules.settlement.confirm(
        confirmation(
            evidence,
            start="2024-01-01",
            end="2024-12-31",
            fee_field="trading_fee",
            fee_unit="yuan_per_lot",
            margin_unit="ratio",
        )
    )
    spec = body.rules.spec.model_dump(mode="json")
    spec["periods"] = [period.model_dump(mode="json")]
    spec["contract"] = evidence.contract.model_dump(mode="json")
    rule = rules.save(RuleSpec.model_validate(spec))
    run = research.submit(body.model_copy(update={"rules": rule}))
    claimed = scheduler(research.engine).claim("settlement")
    # The run pins the settlement evidence of its frozen rules.
    assert "settlement-fixed" in json.dumps(claimed["payload"])
    content = canonical(calculate(claimed["payload"], strategy_catalog(), ExecutionFactory()))
    research.publish(run["id"], claimed["token"], content)
    packages = ResearchPackages(research)
    exported = packages.export(run["id"], True)
    assert (
        exported["content"]["request"]["rules"]["spec"]["periods"][0]["settlement_basis"][
            "evidence"
        ]["version_id"]
        == "settlement-fixed"
    )

    def unavailable(*a, **kw):
        pytest.fail("Replay must use frozen evidence")

    research.versions = VersionAccess(unavailable, unavailable, unavailable)
    research.rules = RuleAccess(unavailable)
    imported = packages.receive("offline", exported)
    replay = packages.replay("offline", imported["id"], "settlement-replay")
    claimed = scheduler(research.engine).claim("offline")
    assert (
        canonical(calculate(claimed["payload"], strategy_catalog(), ExecutionFactory())) == content
    )
    research.publish(replay["id"], claimed["token"], content)
    assert research.get(replay["id"])["result"]["reproduction_matches"]
    # Embedded contracts themselves guard causality even without any catalogue.
    invalid = period.model_dump(mode="json")
    invalid["start"] = "2023-12-29"
    with pytest.raises(ValueError, match="盘后"):
        RulePeriod.model_validate(invalid)


def test_settlement_uses_standard_contract_not_provider_symbol():
    from asterion_bindings.rules import SettlementRow

    row = dict.fromkeys(SettlementRow.model_fields) | {
        "symbol": "opaque:instrument-42",
        "exchange": "SHFE",
        "contract": "SHFE.RB2610",
        "trading_day": "2024-01-02",
        "trading_fee": "3",
        "long_margin_rate": "0.1",
    }
    source = {
        "version": {
            "id": "independent-settlement",
            "manifest": {
                "scope": {"exchange": "SHFE"},
                "contract_identity": source_identity(
                    "SHFE.rb2610", "independent-feed", "opaque:instrument-42"
                ),
                "source": "independent-feed",
                "layer": "STANDARD",
                "type": {"id": "futures.settlement", "schema_version": 1},
                "checksum": "b" * 64,
                "origin": {"connection_id": None},
                "observed_at": "2024-01-03T00:00:00Z",
            },
        },
        "rows": [row],
        "total": 1,
    }
    mapping = SettlementMapping(
        VersionAccess(lambda *a, **kw: source, lambda _: None, unsupported_scan)
    )
    evidence = mapping.preview(
        SettlementRequest(
            version_id="independent-settlement",
            contract_id="SHFE.RB.202610.20240102",
            trading_day="2024-01-02",
        )
    )
    assert evidence.provider == "independent-feed"
    period = mapping.confirm(
        confirmation(
            evidence, fee_field="trading_fee", fee_unit="yuan_per_lot", margin_unit="ratio"
        )
    )
    assert period.open_fee == Decimal(3)
    assert period.margin_rate == Decimal("0.1")


@pytest.mark.parametrize("dataset", ["daily", "settlement"])
def test_contract_backup_requires_identity_without_rewriting_files(context, monkeypatch, dataset):  # noqa: F811
    from dataclasses import replace

    from asterion_bindings.files import read_files
    from credential_helpers import provider_secrets
    from sqlalchemy import select

    from asterion.data.backup import DataBackup, validate_backup
    from asterion.data.library import versions

    version = publish(context, monkeypatch, dataset=dataset)
    service = context[2]
    with service.engine.connect() as conn:
        records = tuple(dict(row) for row in conn.execute(select(versions)).mappings())
    evidence = DataBackup(
        records,
        (),
        (),
        (),
        (),
        (),
        ArtifactStore(service.root, read_only=True),
        read_files(service.root),
        provider_secrets(MASTER, service.root).opens,
    )
    assert validate_backup(evidence)["versions"] == len(records)
    damaged = deepcopy(records)
    for item in damaged:
        if item["id"] == version["id"]:
            item["manifest"].pop("contract_identity")
    files = {p: p.read_bytes() for p in service.root.rglob("*") if p.is_file()}
    with pytest.raises(ValueError, match="合约数据缺少"):
        validate_backup(replace(evidence, versions=damaged))
    assert files == {p: p.read_bytes() for p in service.root.rglob("*") if p.is_file()}


def test_settlement_identity_rejects_different_listing_and_source():
    from asterion_bindings.rules import SettlementRow

    row = dict.fromkeys(SettlementRow.model_fields) | {
        "symbol": "opaque:42",
        "exchange": "SHFE",
        "contract": "SHFE.rb2610",
        "trading_day": "2024-01-02",
        "trading_fee": "3",
        "long_margin_rate": "0.1",
    }
    source = {
        "version": {
            "id": "fixed",
            "manifest": {
                "source": "independent",
                "layer": "STANDARD",
                "type": {"id": "futures.settlement", "schema_version": 1},
                "checksum": "a" * 64,
                "origin": {"connection_id": None},
                "scope": {"exchange": "SHFE"},
                "observed_at": "2024-01-03T00:00:00Z",
                "contract_identity": source_identity("SHFE.rb2610", "independent", "opaque:42"),
            },
        },
        "rows": [row],
        "total": 1,
    }
    mapping = SettlementMapping(
        VersionAccess(lambda *a, **kw: source, lambda _: None, unsupported_scan)
    )
    request = SettlementRequest(
        version_id="fixed", contract_id="SHFE.RB.202610.20240103", trading_day="2024-01-02"
    )
    with pytest.raises(ValueError, match="规范合约身份不一致"):
        mapping.preview(request)
    request.contract_id = "SHFE.RB.202610.20240102"
    assert mapping.preview(request).contract.id == request.contract_id
    source["version"]["manifest"]["source"] = "different-source"
    with pytest.raises(ValueError, match="来源不一致"):
        mapping.preview(request)
