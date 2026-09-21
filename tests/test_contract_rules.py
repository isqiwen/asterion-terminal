from copy import deepcopy
from datetime import date
from decimal import Decimal

import pytest
from fastapi.testclient import TestClient
from rules_support import manual_versions, rule_version
from sqlalchemy import create_engine
from test_research import payload, services  # noqa: F401

from asterion.api.app import create_app
from asterion.contract_rules.public import RuleSpec, RuleVersion, rule_id
from asterion.contract_rules.service import Rules
from asterion.distribution import builtin_plugins, strategy_catalog
from asterion.distribution_storage import rule_storage
from asterion.platform.config import Settings
from asterion.platform.plugins import PluginHost
from asterion.research.engine import BacktestRequest, calculate


def frozen(spec):
    spec = RuleSpec.model_validate(spec)
    return RuleVersion(id=rule_id(spec), spec=spec).model_dump(mode="json")


def test_rule_date_fee_and_margin_calculation():
    value = payload()
    spec = deepcopy(value["request"]["rules"]["spec"])
    spec["periods"] = [
        {
            "start": "2024-01-01",
            "end": "2024-01-03",
            "fee_mode": "notional",
            "open_fee": "0.01",
            "close_fee": "0.02",
            "settlement_basis": None,
            "margin_rate": "0.1",
        },
        {
            "start": "2024-01-04",
            "end": "2024-12-31",
            "fee_mode": "per_lot",
            "open_fee": "3",
            "close_fee": "5",
            "settlement_basis": None,
            "margin_rate": "0.2",
        },
    ]
    value["request"]["rules"] = frozen(spec)
    result = calculate(value, strategy_catalog())
    # Open 21 * 10 * .01 = 2.1; close under Jan 4 rules = 5.
    assert [Decimal(f["fee"]) for f in result["fills"]] == [Decimal("2.1"), Decimal(5)]
    assert Decimal(result["summary"]["final_equity"]) == Decimal("942.9")
    assert result["fills"][1]["rule_start"] == "2024-01-04"
    assert Decimal(result["curve"][3]["margin"]) == Decimal(36)
    assert result["curve"][3]["margin_rate"] == "0.2"
    assert result["fills"][0]["rule_version"] == value["request"]["rules"]["id"]


@pytest.mark.parametrize("change", ["gap", "overlap", "order", "invalid_rate", "nan", "source"])
def test_invalid_rule_contracts(change):
    spec = rule_version().spec.model_dump(mode="json")
    first = spec["periods"][0]
    if change in {"gap", "overlap", "order"}:
        first["end"] = "2024-01-10"
        spec["periods"].append(
            {
                **first,
                "start": {"gap": "2024-01-12", "overlap": "2024-01-10", "order": "2024-01-01"}[
                    change
                ],
                "end": "2024-01-20",
            }
        )
    elif change == "invalid_rate":
        first.update(fee_mode="notional", open_fee="1.1")
    elif change == "nan":
        spec["tick_size"] = "NaN"
    else:
        spec["source"] = " "
    with pytest.raises(ValueError):
        RuleSpec.model_validate(spec)


def test_rules_missing_mismatched_or_out_of_range_are_blocked(services):  # noqa: F811
    service, request, _ = services
    dumped = request.model_dump(mode="json")
    dumped.pop("rules")
    with pytest.raises(ValueError):
        BacktestRequest.model_validate(dumped)
    other = rule_version("DCE.m2405")
    from storage_support import raw_engine

    Rules(rule_storage(raw_engine(service.engine)), manual_versions()).save(other.spec)
    with pytest.raises(ValueError, match="合约.*不一致"):
        service.submit(request.model_copy(update={"rules": other}))
    spec = request.rules.spec.model_dump(mode="json")
    spec["source"] = "未登记的新版本"
    with pytest.raises(ValueError, match="不存在"):
        service.submit(
            request.model_copy(update={"rules": RuleVersion.model_validate(frozen(spec))})
        )
    with pytest.raises(ValueError, match="日期范围不一致"):
        service.submit(request.model_copy(update={"end": date(2025, 1, 1)}))
    spec = request.rules.model_dump(mode="json")
    spec["spec"]["multiplier"] = "99"
    with pytest.raises(ValueError, match="指纹"):
        RuleVersion.model_validate(spec)


def test_saved_rule_versions_are_immutable_and_rerun_uses_snapshot(services):  # noqa: F811
    service, request, _ = services
    run = service.submit(request)
    from storage_support import raw_engine

    store = Rules(rule_storage(raw_engine(service.engine)), manual_versions())
    changed = request.rules.spec.model_dump(mode="json")
    changed["periods"][0]["open_fee"] = "100"
    revision = store.save(RuleSpec.model_validate(changed))
    assert revision.id != request.rules.id
    assert store.save(revision.spec) == revision
    assert len(store.list()) == 2
    assert store.read(request.rules.id) == request.rules
    # Rerun requires no mutable catalogue read and preserves the entire snapshot.
    service.rules = type(service.rules)(lambda _: pytest.fail("rerun must use frozen rules"))
    again = service.rerun(run["id"], "rule-rerun")
    assert again["payload"] == run["payload"]


def test_rule_api_account_gate_and_immutable_registration(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/rules.db")
    settings = Settings(
        token="rules-test-token-at-least-24-characters", data_root=tmp_path, require_account=False
    )
    with TestClient(create_app(settings, engine)) as client:
        assert client.get("/api/v1/contract-rules").status_code == 401
        client.headers["Authorization"] = f"Bearer {settings.token}"
        assert client.get("/api/v1/contract-rules").json() == []
        spec = rule_version().spec.model_dump(mode="json")
        first = client.post("/api/v1/contract-rules", json=spec)
        assert first.status_code == 200
        assert first.json() == rule_version().model_dump(mode="json")
        assert client.post("/api/v1/contract-rules", json=spec).json() == first.json()
        assert len(client.get("/api/v1/contract-rules").json()) == 1
    with TestClient(
        create_app(settings.model_copy(update={"require_account": True}), engine),
        headers={"Authorization": f"Bearer {settings.token}"},
    ) as client:
        assert client.get("/api/v1/contract-rules").status_code in (401, 403, 423)
    engine.dispose()


def test_research_cannot_activate_without_rules_plugin():
    with pytest.raises(ValueError, match="Missing required plugin"):
        PluginHost(tuple(p for p in builtin_plugins() if p.id != "asterion.contract_rules"))


def test_rules_bind_lifecycle_not_reused_market_code():
    spec = rule_version().spec
    spec.cover("SHFE.RB.202405.20230516", date(2024, 1, 2), date(2024, 1, 4))
    with pytest.raises(ValueError, match="合约不一致"):
        spec.cover("SHFE.RB.212405.21230516", date(2124, 1, 2), date(2124, 1, 4))
    with pytest.raises(ValueError, match="生命周期"):
        spec.cover(spec.contract.id, date(2024, 5, 16), date(2024, 5, 17))


def test_worker_rejects_rule_from_another_listing_with_same_market_code():
    value = payload()
    spec = deepcopy(value["request"]["rules"]["spec"])
    spec["contract"].update(id="SHFE.RB.202405.20230517", listed_on="2023-05-17")
    value["request"]["rules"] = frozen(spec)
    with pytest.raises(ValueError, match="合约不一致"):
        calculate(value, strategy_catalog())


def test_rule_identity_requires_complete_contract_and_matching_time_product():
    for mutation in ("missing_month", "wrong_product", "invalid_id"):
        spec = rule_version().spec.model_dump(mode="json")
        if mutation == "missing_month":
            spec["contract"].pop("delivery_month")
        elif mutation == "invalid_id":
            spec["contract"]["id"] = "SHFE.RB.202405.20230517"
        else:
            spec["trading_time"] = rule_version("DCE.m2405").spec.trading_time.model_dump(
                mode="json"
            )
        with pytest.raises(ValueError):
            RuleSpec.model_validate(spec)


def test_manual_rule_identity_source_is_referenced():
    from asterion.research.public import _input_versions

    spec = rule_version().spec.model_dump(mode="json")
    spec["contract"]["provenance"]["source_version"] = "fixed-identity-observation"
    assert "fixed-identity-observation" in _input_versions({"request": {"rules": frozen(spec)}})


def test_rule_backup_rejects_invalid_identity_without_mutation():
    from asterion.contract_rules.backup import RuleBackup, validate

    value = rule_version().model_dump(mode="json")
    assert validate(RuleBackup(lambda: iter([value]))) == {"contract_rule_versions": 1}
    value["spec"]["contract"].pop("delivery_month")
    before = deepcopy(value)
    with pytest.raises(ValueError):
        validate(RuleBackup(lambda: iter([value])))
    assert value == before
