"""Candidate coverage is a source claim, never an exchange completeness attestation."""

from copy import deepcopy

import pytest
from role_source_support import port
from test_computed_roles import evidence

from asterion.contract_roles.candidates import CandidateRequest, CandidateScope, candidate_evidence
from asterion.contract_roles.computed import ComputedSources
from asterion.contract_roles.computed_public import ComputedRequest


def selection(scope=None):
    return CandidateRequest(
        contracts_version_id="contracts-1",
        product_id="SHFE.AU",
        scope=scope or CandidateScope(mode="product_catalog", symbols=()),
        start="2025-04-10",
        end="2025-04-11",
    )


def add_contract(values, **fields):
    contracts = values["contracts-1"]
    row = deepcopy(contracts["rows"][0])
    row.update(fields)
    contracts["rows"].append(row)
    contracts["total"] += 1


def test_product_scope_preserves_lifecycle_exclusions_and_source_boundary():
    values, _ = evidence()
    add_contract(
        values,
        symbol="expired",
        contract="SHFE.au2502",
        delivery_month="2025-02",
        delisted="2025-02-17",
    )
    add_contract(
        values,
        symbol="future",
        contract="SHFE.au2606",
        delivery_month="2026-06",
        listed="2025-06-17",
        delisted="2026-06-15",
    )
    result = candidate_evidence(port(values).read, selection())
    assert len(result.catalog.contracts) == 4
    assert len(result.included) == 2
    assert {(e.symbol, e.reason) for e in result.excluded} == {
        ("expired", "expired_before_window"),
        ("future", "listed_after_window"),
    }
    assert result.coverage == "fixed_source_product"
    assert result.exchange_completeness_attested is False


def test_product_scope_cannot_hide_missing_active_candidate():
    values, body = evidence()
    add_contract(
        values,
        symbol="third",
        contract="SHFE.au2510",
        delivery_month="2025-10",
        delisted="2025-10-15",
    )
    explicit = ComputedSources(port(values)).build(body)
    assert explicit.candidates.coverage == "explicit_subset"
    request = ComputedRequest.model_validate(
        body.model_dump() | {"candidate_scope": {"mode": "product_catalog", "symbols": []}}
    )
    with pytest.raises(ValueError, match="全部存续"):
        ComputedSources(port(values)).build(request)


@pytest.mark.parametrize(
    "change", ["truncated", "unknown_product", "missing_lifecycle", "wrong_version"]
)
def test_invalid_catalog_refused(change):
    values, _ = evidence()
    body = selection()
    if change == "truncated":
        values["contracts-1"]["total"] += 1
    elif change == "unknown_product":
        body = body.model_copy(update={"product_id": "SHFE.RB"})
    elif change == "missing_lifecycle":
        values["contracts-1"]["rows"][0]["delisted"] = None
    else:
        values["contracts-1"]["version"]["id"] = "another-version"
    with pytest.raises(ValueError):
        candidate_evidence(port(values).read, body)


@pytest.mark.parametrize(
    "scope",
    [
        {"mode": "explicit", "symbols": ["one"]},
        {"mode": "explicit", "symbols": ["one", "one"]},
        {"mode": "product_catalog", "symbols": ["one", "two"]},
    ],
)
def test_scope_cannot_be_mislabelled(scope):
    with pytest.raises(ValueError):
        CandidateScope.model_validate(scope)


def test_saved_candidate_evidence_reverified_against_source():
    values, body = evidence()
    service = ComputedSources(port(values))
    spec = service.build(body)
    payload = spec.model_dump(mode="json")
    payload["candidates"]["included"] = payload["candidates"]["included"][:1]
    with pytest.raises(ValueError, match="来源证据"):
        service.verify(type(spec).model_validate(payload))


def test_consecutive_source_windows_publish_before_each_effective_opening():
    from asterion.contract_roles.computed_public import ComputedVersion, digest, resolve_computed
    from asterion.contract_roles.public import RoleQuery

    values, body = evidence()
    payload = body.model_dump(mode="json")
    payload["candidate_scope"] = {"mode": "product_catalog", "symbols": []}
    full = ComputedRequest.model_validate(payload)
    first_payload = deepcopy(payload)
    first_payload["daily_inputs"] = [
        r for r in payload["daily_inputs"] if r["trading_day"] == "2025-04-10"
    ]
    service = ComputedSources(port(values))
    records = []
    for request, day in [(ComputedRequest.model_validate(first_payload), "10"), (full, "11")]:
        spec = service.build(request)
        version = ComputedVersion(
            id=digest(spec.model_dump(mode="json")),
            spec=spec,
            published_at=f"2025-04-{day}T16:00:00+08:00",
        )
        decision = spec.result.decisions[-1]
        resolved = resolve_computed(
            version,
            RoleQuery(
                version_id=version.id,
                role="main",
                timestamp=decision.effective_start,
                information_at=version.published_at,
                mode="as_known",
                explanation="offline consecutive source publication",
            ),
        )
        assert resolved.contract.id == decision.main
        service.verify(version.spec)
        records.append(version)
    assert [r.spec.result.decisions[-1].reason for r in records] == ["confirming", "switched"]
    assert records[0].id != records[1].id
    # Accumulated evidence published on Friday must not backfill Thursday's opening.
    first_opening = records[1].spec.result.decisions[0].effective_start
    with pytest.raises(ValueError, match="发布晚于"):
        resolve_computed(
            records[1],
            RoleQuery(
                version_id=records[1].id,
                role="main",
                timestamp=first_opening,
                information_at=first_opening,
                mode="as_known",
                explanation="",
            ),
        )
