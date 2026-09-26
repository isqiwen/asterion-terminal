"""Diagnostic inputs are explicit test evidence, never production market assertions."""

from copy import deepcopy

import pytest
from test_contract_roles import fixture

from asterion.contract_roles.ranking import RankingRequest, rank_roles


def request():
    base = fixture().spec
    catalog = base.catalog.model_dump(mode="json")
    first = catalog["contracts"][0]
    second = deepcopy(first)
    second.update(id="SHFE.AU.202508.20240617", delivery_month="2025-08")
    catalog["contracts"].append(second)
    return {
        "schema_version": 1,
        "product_id": "SHFE.AU",
        "catalog": catalog,
        "trading_time": base.trading_time.model_dump(mode="json"),
        "candidates": [first["id"], second["id"]],
        "policy": {
            "metric": "open_interest",
            "unit": "contracts",
            "tie_break": "earlier_delivery",
            "missing": "reject",
            "no_trade": "exclude",
            "switch_margin": "0.1",
            "confirmations": 2,
            "allow_backward": False,
            "secondary": "best_remaining",
        },
        "initial_main": first["id"],
        "observations": [
            {
                "trading_day": f"2025-04-{d}",
                "values": [
                    {
                        "contract_id": c["id"],
                        "volume": "50",
                        "open_interest": str(score),
                        "source_version": f"daily-{i}",
                        "source_checksum": "a" * 64,
                        "available_at": f"2025-04-{d}T15:05:00+08:00",
                    }
                    for i, (c, score) in enumerate([(first, 100), (second, 120)])
                ],
            }
            for d in [10, 11]
        ],
        "evidence_scope": "unverified_diagnostic",
        "explanation": "离线算法用例",
    }


def run(value):
    return rank_roles(RankingRequest.model_validate(value))


def test_confirmation_and_friday_night_effective_monday_replay():
    value = request()
    result = run(value)
    assert [d.reason for d in result.decisions] == ["confirming", "switched"]
    assert result.decisions[-1].main == value["candidates"][1]
    assert result.decisions[-1].secondary == value["candidates"][0]
    assert str(result.decisions[-1].effective_day) == "2025-04-14"
    assert result.decisions[-1].effective_start.isoformat() == "2025-04-11T21:00:00+08:00"
    assert result == run(RankingRequest.model_validate(value).model_dump(mode="json"))
    assert not result.publishable and not result.execution_authorized


def test_ties_deterministic_and_do_not_trigger_switch():
    value = request()
    for day in value["observations"]:
        day["values"][1]["open_interest"] = "100"
        day["values"].reverse()
    assert all(d.reason == "retained" for d in run(value).decisions)
    value["initial_main"] = None
    value["policy"]["tie_break"] = "later_delivery"
    assert run(value).decisions[0].main == value["candidates"][1]


def test_threshold_is_strict_and_backward_switch_explicit():
    value = request()
    value["observations"][0]["values"][1]["open_interest"] = "110"
    assert [d.reason for d in run(value).decisions] == ["retained", "confirming"]
    value["initial_main"] = value["candidates"][1]
    for day in value["observations"]:
        day["values"][0]["open_interest"] = "200"
    assert all(d.reason == "retained" for d in run(value).decisions)
    value["policy"]["allow_backward"] = True
    assert run(value).decisions[-1].reason == "switched"


@pytest.mark.parametrize(
    "change",
    [
        "missing",
        "duplicate",
        "premature",
        "gap",
        "idle",
        "nan",
        "expired",
        "unknown_time",
        "overlap",
    ],
)
def test_invalid_or_incomplete_observations_refused(change):
    value = request()
    row = value["observations"][0]["values"][0]
    if change == "missing":
        value["observations"][0]["values"].pop()
    elif change == "duplicate":
        value["observations"][0]["values"].append(deepcopy(row))
    elif change == "premature":
        row["available_at"] = "2025-04-10T14:59:00+08:00"
    elif change == "gap":
        value["observations"][0]["trading_day"] = "2025-04-09"
    elif change == "idle":
        row["volume"] = "0"
    elif change == "nan":
        row["open_interest"] = "NaN"
    elif change == "expired":
        value["catalog"]["contracts"][0]["last_trade_on"] = "2025-04-10"
    elif change == "unknown_time":
        row["available_at"] = None
    elif change == "overlap":
        for row in value["observations"][0]["values"]:
            row["available_at"] = "2025-04-11T16:00:00+08:00"
    with pytest.raises(ValueError):
        run(value)


def test_api_diagnostic_authentication_and_no_publication(tmp_path):
    from asterion_bindings.database import create_engine
    from fastapi.testclient import TestClient

    from asterion.api.app import create_app
    from asterion.platform.config import Settings

    engine = create_engine(f"sqlite:///{tmp_path}/ranking.db")
    settings = Settings(
        token="ranking-test-token-24-characters", data_root=tmp_path, require_account=False
    )
    try:
        with TestClient(create_app(settings, engine)) as client:
            path = "/api/v1/contract-roles/ranking/diagnose"
            assert client.post(path, json=request()).status_code == 401
            client.headers["Authorization"] = "Bearer " + settings.token
            response = client.post(path, json=request())
            assert response.status_code == 200
            assert response.json() == run(request()).model_dump(mode="json")
            assert client.post("/api/v1/contract-roles", json=response.json()).status_code == 422
            invalid = request()
            invalid["observations"][0]["values"].pop()
            assert client.post(path, json=invalid).status_code == 422
            assert client.get("/api/v1/contract-roles").json() == []
    finally:
        engine.dispose()


def test_failed_confirmation_resets_and_volume_metric_is_independent():
    value = request()
    value["observations"][1]["values"][1]["open_interest"] = "100"
    last = run(value).decisions[-1]
    assert last.reason == "retained" and last.confirmation_count == 0 and last.challenger is None
    value["policy"]["metric"] = "volume"
    assert all(d.reason == "retained" for d in run(value).decisions)


def test_threshold_does_not_depend_on_decimal_context_precision():
    from decimal import localcontext

    value = request()
    value["policy"]["switch_margin"] = "0.123456789"
    for day in value["observations"]:
        day["values"][1]["open_interest"] = "112.34567891"
    expected = run(value)
    with localcontext() as context:
        context.prec = 3
        assert run(value) == expected
    assert expected.decisions[-1].reason == "switched"


def test_changing_challenger_restarts_confirmation_and_idle_exclusion():
    value = request()
    third = deepcopy(value["catalog"]["contracts"][1])
    third.update(id="SHFE.AU.202510.20240617", delivery_month="2025-10")
    value["catalog"]["contracts"].append(third)
    value["candidates"].append(third["id"])
    for i, day in enumerate(value["observations"]):
        row = deepcopy(day["values"][1])
        row.update(contract_id=third["id"], open_interest=str(90 if i == 0 else 150))
        day["values"].append(row)
    result = run(value)
    assert [d.confirmation_count for d in result.decisions] == [1, 1]
    assert result.decisions[-1].main == value["initial_main"]
    assert result.decisions[-1].secondary == third["id"]
    for day in value["observations"]:
        day["values"][-1]["volume"] = "0"
    assert all(third["id"] not in d.ranking for d in run(value).decisions)
    value["policy"]["no_trade"] = "reject"
    with pytest.raises(ValueError, match="无成交"):
        run(value)


def test_late_information_waits_for_next_trading_day_opening():
    value = request()
    value["observations"] = value["observations"][:1]
    for row in value["observations"][0]["values"]:
        row["available_at"] = "2025-04-10T21:01:00+08:00"
    decision = run(value).decisions[0]
    assert str(decision.effective_day) == "2025-04-14"
    assert decision.effective_start.isoformat() == "2025-04-11T21:00:00+08:00"


@pytest.mark.parametrize(
    "reason", ["not_listed", "expired", "expires_before_effective", "no_trade"]
)
def test_daily_exclusion_explains_every_unranked_candidate(reason):
    value = request()
    value["observations"] = value["observations"][:1]
    third = deepcopy(value["catalog"]["contracts"][1])
    third.update(id="SHFE.AU.202510.20240617", delivery_month="2025-10")
    if reason == "not_listed":
        third.update(id="SHFE.AU.202510.20250411", listed_on="2025-04-11")
    elif reason == "expired":
        third["last_trade_on"] = "2025-04-09"
    elif reason == "expires_before_effective":
        third["last_trade_on"] = "2025-04-10"
    value["catalog"]["contracts"].append(third)
    value["candidates"].append(third["id"])
    if reason in {"no_trade", "expires_before_effective"}:
        row = deepcopy(value["observations"][0]["values"][0])
        row.update(contract_id=third["id"], volume="0" if reason == "no_trade" else "50")
        value["observations"][0]["values"].append(row)
    decision = run(value).decisions[0]
    assert [(r.contract_id, r.reason) for r in decision.excluded] == [(third["id"], reason)]
    assert third["id"] not in decision.ranking
