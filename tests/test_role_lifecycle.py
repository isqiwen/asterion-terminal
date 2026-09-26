"""Fixed catalog lifecycle changes must survive consecutive role publication."""

from copy import deepcopy

import pytest
from asterion_bindings.catalog import catalog_digest
from asterion_bindings.task_repository import task_port
from role_source_support import port
from test_computed_roles import evidence
from test_role_candidates import add_contract
from test_role_sequence import check, publish
from test_role_sync_batch import ports
from test_role_tasks import setup as role_setup

from asterion.contract_roles.computed import ComputedSources, replay_computed
from asterion.contract_roles.computed_public import ComputedRequest
from asterion.contract_roles.plugin import RoleBackup, computed_versions, validate
from asterion.contract_roles.sequence import ContinuationRequest, continuation
from asterion.contract_roles.sync_batch import preview, submit
from asterion.contract_roles.tasks import KIND, execute
from asterion.data.public import SourceIdentity, source_contract_catalog

setup = role_setup


def lifecycle_evidence(mode="product_catalog"):
    values, original = evidence()
    add_contract(
        values,
        symbol="new-listing",
        contract="SHFE.au2604",
        delivery_month="2026-04",
        listed="2025-04-11",
        delisted="2026-04-15",
    )
    add_contract(
        values,
        symbol="expiring",
        contract="SHFE.au2504",
        delivery_month="2025-04",
        listed="2024-04-16",
        delisted="2025-04-10",
    )
    refs = [r.model_dump(mode="json") for r in original.daily_inputs]
    for symbol, day, score in [("new-listing", 11, "1000"), ("expiring", 10, "1")]:
        catalog = source_contract_catalog(port(values).read, "contracts-1", [symbol])
        source = next(r for r in values["contracts-1"]["rows"] if r["symbol"] == symbol)
        identity = SourceIdentity(
            catalog_id=catalog_digest(catalog),
            catalog=catalog,
            source="offline-feed",
            symbol=symbol,
            information_at="2025-04-01T00:00:00Z",
        )
        identifier = f"daily-{day}-{symbol}"
        sample = deepcopy(values[f"daily-{day}-0"])
        sample["version"]["id"] = identifier
        sample["version"]["manifest"]["contract_identity"] = identity.model_dump(mode="json")
        sample["rows"][0].update(contract=source["contract"], symbol=symbol, oi=score)
        values[identifier] = sample
        refs.append({"trading_day": f"2025-04-{day}", "version_id": identifier})
    scope = {
        "mode": mode,
        "symbols": [r["symbol"] for r in values["contracts-1"]["rows"]]
        if mode == "explicit"
        else [],
    }
    request = ComputedRequest.model_validate(
        original.model_dump() | {"candidate_scope": scope, "daily_inputs": refs}
    )
    return values, request


def lifecycle_campaign(mode="product_catalog"):
    values, request = lifecycle_evidence(mode)
    sources = ComputedSources(port(values))
    first_refs = tuple(r for r in request.daily_inputs if r.trading_day.day == 10)
    next_refs = tuple(r for r in request.daily_inputs if r.trading_day.day == 11)
    first = publish(
        sources.build(request.model_copy(update={"daily_inputs": first_refs})),
        "2025-04-10T16:00:00+08:00",
    )
    body = ContinuationRequest(
        previous_version_id=first.id,
        daily_inputs=next_refs,
        explanation="Offline new listing and expiry in a fixed source catalog",
    )
    second = publish(continuation(sources, first, body), "2025-04-11T16:00:00+08:00")
    return values, sources, first, second, body


@pytest.fixture(autouse=True)
def use_lifecycle_campaign(monkeypatch):
    monkeypatch.setattr("test_role_tasks.campaign", lifecycle_campaign)


@pytest.mark.parametrize("mode", ["explicit", "product_catalog"])
def test_listing_and_expiry_preserve_prior_decisions_and_confirmation(mode):
    values, sources, first, second, _ = lifecycle_campaign(mode)
    catalog = {s.symbol: s.contract_id for s in first.spec.input.catalog.symbols}
    new, expiring = catalog["new-listing"], catalog["expiring"]
    assert first.spec.input.candidates == second.spec.input.candidates
    before, after = second.spec.result.decisions
    assert before == first.spec.result.decisions[0]
    assert (new, "not_listed") in {(e.contract_id, e.reason) for e in before.excluded}
    assert (expiring, "expires_before_effective") in {
        (e.contract_id, e.reason) for e in before.excluded
    }
    assert (expiring, "expired") in {(e.contract_id, e.reason) for e in after.excluded}
    assert after.ranking[0] == after.challenger == new
    assert after.main == before.main
    assert after.confirmation_count == 1  # A different challenger cannot inherit confirmations.
    assert check(sources, first, second, minimum=0).switches == 0
    assert replay_computed(second.spec) == second.spec.result
    assert (
        validate(
            RoleBackup(
                (), port(values), tuple(r.model_dump(mode="json") for r in (first, second)), ()
            )
        )["computed_role_versions"]
        == 2
    )


def test_newly_listed_candidate_cannot_be_omitted_from_next_day():
    _, sources, first, _, body = lifecycle_campaign()
    missing = body.model_copy(
        update={
            "daily_inputs": tuple(r for r in body.daily_inputs if "new-listing" not in r.version_id)
        }
    )
    with pytest.raises(ValueError, match="全部存续"):
        continuation(sources, first, missing)


def test_batch_and_worker_publish_full_lifecycle_chain(setup):
    from sqlalchemy import select

    workflow, batch, request, data = ports(setup)
    roles, tasks, storage, values, expected, _ = setup
    plan = preview(workflow, request.previous_version_id)
    assert set(plan.symbols) == {"opaque-au", "opaque-au-next", "new-listing"}
    receipt = submit(workflow, batch, request)
    assert len(receipt["request"]["sync_job_ids"]) == 3
    fixed = {
        "opaque-au": "daily-11-0",
        "opaque-au-next": "daily-11-1",
        "new-listing": "daily-11-new-listing",
    }
    for index in range(3):
        claimed = tasks.claim("offline-daily-worker")
        assert claimed["kind"] == "data.sync"
        symbol = claimed["payload"]["request"]["symbol"]
        with data.begin() as conn:
            task_port(tasks, frozenset({"data.sync"})).complete(
                conn, claimed["id"], claimed["token"], {"version_id": fixed[symbol]}
            )
            workflow.published(conn, claimed["id"])
        if index < 2:
            assert workflow.get(receipt["id"])["job_id"] is None
    claimed = tasks.claim("offline-role-worker")
    assert claimed["kind"] == KIND
    content, _ = execute(None, claimed["payload"])
    result = roles.publish(claimed["id"], claimed["token"], content)
    with storage.connect() as conn:
        stored = list(conn.execute(select(computed_versions)).mappings())
    assert len(stored) == 2
    assert tasks.get(claimed["id"])["state"] == "SUCCEEDED"
    assert result["previous_version_id"] == request.previous_version_id
    # The batch has its own explanation, but must produce the same daily decisions.
    record = next(r for r in stored if r["id"] == result["computed_version_id"])
    assert (
        record["spec"]["result"]["decisions"]
        == expected.spec.result.model_dump(mode="json")["decisions"]
    )
    assert validate(RoleBackup((), port(values), tuple(dict(r) for r in stored), ()))
    data.close()


def test_workflow_refuses_dependencies_that_omit_new_listing(setup):
    from sqlalchemy import select

    from asterion.contract_roles.sync_workflow import SyncContinuation, workflows

    workflow, batch, request, data = ports(setup)
    receipt = submit(workflow, batch, request)
    dependencies = tuple(
        row["id"]
        for row in workflow.get(receipt["id"])["dependencies"]
        if row["symbol"] != "new-listing"
    )
    incomplete = SyncContinuation.model_validate(
        receipt["request"] | {"command_id": "missing-new-listing", "sync_job_ids": dependencies}
    )
    with pytest.raises(ValueError, match="全部存续"):
        workflow.submit(incomplete)
    with workflow.storage.connect() as conn:
        assert len(conn.execute(select(workflows)).all()) == 1
    data.close()


def test_continuation_rejects_changed_fixed_catalog():
    values, sources, first, _, body = lifecycle_campaign()
    values["contracts-1"]["rows"][-2]["listed"] = "2025-04-12"
    with pytest.raises(ValueError):
        continuation(sources, first, body)
