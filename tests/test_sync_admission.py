import pytest
from sqlalchemy import select
from storage_support import raw_engine, scheduler
from test_preparation import request, sync  # noqa: F401
from test_preparation_identity import evidence

from asterion.platform.store import jobs


def reference(service):
    service.submit(
        request(command_id="reference", dataset="contracts", symbol="", start=None, end=None)
    )
    job = scheduler(service.engine).claim("reference")
    return service.publish(job["id"], job["token"], evidence(job))


def submission(version=None, **changes):
    return request(**changes).model_dump(mode="json") | {"contracts_version_id": version}


def test_standalone_pins_identity_and_publishes_same_canonical_scope(sync):  # noqa: F811
    version = reference(sync)
    body = submission(version["id"])
    accepted = sync.admit(body)
    assert sync.admit(body)["id"] == accepted["id"]
    identity = accepted["payload"]["contract_identity"]
    assert identity["catalog"]["inputs"][0]["version_id"] == version["id"]
    assert "contracts_version_id" not in accepted["payload"]["request"]
    claimed = scheduler(sync.engine).claim("daily")
    result = sync.publish(claimed["id"], claimed["token"], evidence(claimed))
    assert result["manifest"]["scope"]["contract_ids"] == ["SHFE.RB.202610.20240101"]


@pytest.mark.parametrize("changes", [{}, {"symbol": "rb2610.SHF"}, {"start": "2023-01-01"}])
def test_missing_or_wrong_evidence_never_queues_daily(sync, changes):  # noqa: F811
    version = reference(sync)
    with pytest.raises(ValueError):
        sync.admit(submission(version["id"] if changes else None, **changes))
    with sync.engine.connect() as conn:
        assert len(conn.execute(select(jobs.c.id)).all()) == 1


def test_tampered_source_file_is_rejected_before_enqueue(sync):  # noqa: F811
    version = reference(sync)
    (sync.root / version["manifest"]["path"]).write_bytes(b"invalid")
    with pytest.raises(ValueError):
        sync.admit(submission(version["id"]))
    assert len(scheduler(sync.engine).list()) == 1


def test_unrelated_type_rejects_identity_selection(sync):  # noqa: F811
    version = reference(sync)
    with pytest.raises(ValueError, match="不接受"):
        sync.admit(submission(version["id"], dataset="calendar", symbol=""))


def test_wrong_connection_is_rejected_before_enqueue(sync):  # noqa: F811

    version = reference(sync)
    connection = sync.sources.create("tushare", "another")
    with pytest.raises(ValueError, match="连接不一致"):
        sync.admit(submission(version["id"], connection_id=connection["id"]))
    assert len(scheduler(sync.engine).list()) == 1


def test_retry_preserves_identity_and_publication_rechecks_rows(sync):  # noqa: F811
    version = reference(sync)
    accepted = sync.admit(submission(version["id"]))
    claimed = scheduler(sync.engine).claim("daily")
    scheduler(sync.engine).fail(claimed["id"], claimed["token"], "fixture")
    retried = sync.retry(claimed["id"], "retry-identity")
    assert retried["payload"]["contract_identity"] == accepted["payload"]["contract_identity"]
    worker = scheduler(sync.engine).claim("retry")
    with pytest.raises(ValueError, match="返回日期超出请求范围"):
        sync.publish(worker["id"], worker["token"], evidence(worker, bad_day=True))


@pytest.mark.parametrize("dataset", ["daily", "settlement"])
def test_contract_data_requires_identity_at_internal_submission(sync, dataset):  # noqa: F811
    with pytest.raises(ValueError):
        sync.submit(request(dataset=dataset))
    assert scheduler(sync.engine).list() == []


def test_settlement_pins_identity_for_publish_and_retry(sync):  # noqa: F811
    import json

    from asterion.platform.serialization import canonical

    version = reference(sync)
    accepted = sync.admit(submission(version["id"], dataset="settlement"))
    claimed = scheduler(sync.engine).claim("settlement")
    scheduler(sync.engine).fail(claimed["id"], claimed["token"], "fixture")
    retried = sync.retry(claimed["id"], "retry-settlement")
    assert retried["payload"]["contract_identity"] == accepted["payload"]["contract_identity"]
    claimed = scheduler(sync.engine).claim("settlement-retry")
    content = json.loads(evidence(claimed))
    for item in content:
        for row in item["rows"]:
            row["exchange"] = "SHFE"
            row["settle"] = 11
    published = sync.publish(claimed["id"], claimed["token"], canonical(content))
    assert published["manifest"]["scope"]["contract_ids"] == ["SHFE.RB.202610.20240101"]
    assert published["manifest"]["contract_identity"] == accepted["payload"]["contract_identity"]


@pytest.mark.parametrize("dataset", ["daily", "settlement"])
@pytest.mark.parametrize("mutation", ["missing", "type", "symbol"])
def test_worker_and_publication_reject_invalid_contract_task(sync, mutation, dataset):  # noqa: F811
    from copy import deepcopy

    from asterion.data.sync_identity import task_identity

    version = reference(sync)
    accepted = sync.admit(submission(version["id"], dataset=dataset))
    payload = deepcopy(accepted["payload"])
    if mutation == "missing":
        payload.pop("contract_identity")
    elif mutation == "type":
        payload["type_id"] = "futures.calendar"
    else:
        payload["request"]["symbol"] = "CU2610.SHF"
    with pytest.raises(ValueError):
        task_identity(payload)
    with raw_engine(sync.engine).begin() as conn:
        conn.execute(jobs.update().where(jobs.c.id == accepted["id"]).values(payload=payload))
    worker = scheduler(sync.engine).claim("invalid-settlement")
    with pytest.raises(ValueError):
        sync.publish(worker["id"], worker["token"], evidence(worker))
    assert sync.library.list(type_id="futures." + dataset)["total"] == 0


def test_internal_submission_rejects_catalog_rewritten_under_real_input_hash(sync):  # noqa: F811
    from copy import deepcopy

    from asterion_bindings.catalog import ReferenceCatalog, catalog_digest

    version = reference(sync)
    accepted = sync.admit(submission(version["id"], dataset="settlement"))
    identity = deepcopy(accepted["payload"]["contract_identity"])
    identity["catalog"]["contracts"][0]["last_delivery_on"] = "2026-10-25"
    identity["catalog_id"] = catalog_digest(ReferenceCatalog.model_validate(identity["catalog"]))
    with pytest.raises(ValueError, match="资料文件内容不一致"):
        sync.submit(request(command_id="forged", dataset="settlement"), contract_identity=identity)
    assert len(scheduler(sync.engine).list()) == 2


def test_retry_cannot_recreate_daily_task_without_identity(sync):  # noqa: F811
    version = reference(sync)
    accepted = sync.admit(submission(version["id"]))
    worker = scheduler(sync.engine).claim("daily")
    scheduler(sync.engine).fail(worker["id"], worker["token"], "fixture")
    payload = dict(accepted["payload"])
    payload.pop("contract_identity")
    with raw_engine(sync.engine).begin() as conn:
        conn.execute(jobs.update().where(jobs.c.id == accepted["id"]).values(payload=payload))
    with pytest.raises(ValueError):
        sync.retry(accepted["id"], "invalid-retry")
    assert len(scheduler(sync.engine).list()) == 2
