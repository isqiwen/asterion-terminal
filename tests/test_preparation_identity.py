from dataclasses import replace
from datetime import UTC, datetime

import pytest
from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.task_repository import Conflict
from sqlalchemy import select
from storage_support import scheduler
from test_preparation import request, sync  # noqa: F401

from asterion.data.library import versions
from asterion.data.preparation import Preparations, batches
from asterion.data.providers.public import SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs


def evidence(job, *, incomplete=False, bad_day=False, last_day="20261015"):
    req = SyncRequest.model_validate(job["payload"]["request"])
    part = Tushare().plan(req)[0]
    if req.dataset == "contracts":
        rows = [
            {
                "ts_code": "RB2610.SHF",
                "exchange": "SHFE",
                "name": "fixture",
                "fut_code": "RB",
                "d_month": None if incomplete else "202610",
                "list_date": "20240101",
                "delist_date": last_day,
                "per_unit": 10,
                "trade_unit": "吨",
                "quote_unit": "元/吨",
            }
        ]
    elif req.dataset == "calendar":
        rows = [
            {
                "exchange": "SHFE",
                "cal_date": f"2024010{day}",
                "is_open": 1,
                "pretrade_date": "20240101",
            }
            for day in (2, 3, 4)
        ]
    else:
        rows = [
            {
                "ts_code": "RB2610.SHF",
                "trade_date": "20230101" if bad_day else f"2024010{day}",
                "open": 10,
                "high": 12,
                "low": 9,
                "close": 11,
                "vol": 100,
            }
            for day in (2, 3, 4)
        ]
    if req.dataset == "daily" and not bad_day:
        rows = [
            row
            for row in rows
            if str(req.start).replace("-", "") <= row["trade_date"] <= str(req.end).replace("-", "")
        ]
    return canonical(
        [
            {
                "partition": part.model_dump(),
                "rows": [{field: row.get(field) for field in part.fields} for row in rows],
                "observed_at": datetime.now(UTC).isoformat(),
            }
        ]
    )


def reference_job(service):
    calendar = scheduler(service.engine).claim("fixture")
    assert calendar["payload"]["type_id"] == "futures.calendar"
    service.publish(calendar["id"], calendar["token"], evidence(calendar))
    return scheduler(service.engine).claim("fixture")


def test_dependency_pins_accepted_configuration_and_publishes_once(sync):  # noqa: F811
    batch = sync.preparations.submit(request())
    with sync.engine.connect() as conn:
        accepted = conn.execute(select(batches.c.daily_payload)).scalar_one()
    sync.sources.apply("tushare", 1, secrets={"token": "changed-fixture-token"})
    source = reference_job(sync)
    content = evidence(source)
    version = sync.publish(source["id"], source["token"], content)
    assert sync.publish(source["id"], source["token"], content)["id"] == version["id"]
    state = Preparations(sync).get(batch.id)
    assert state.daily_state == "SUBMITTED"
    assert state.identity.catalog.inputs[0].version_id == version["id"]
    daily = scheduler(sync.engine).claim("daily")
    assert daily["payload"]["configuration"] == accepted["configuration"]
    assert daily["payload"]["contract_identity"] == state.identity.model_dump(mode="json")
    published = sync.publish(daily["id"], daily["token"], evidence(daily))
    assert published["manifest"]["scope"]["contract_ids"] == ["SHFE.RB.202610.20240101"]
    assert published["manifest"]["contract_identity"] == daily["payload"]["contract_identity"]
    assert len(scheduler(sync.engine).list()) == 3


def test_incomplete_identity_keeps_source_but_never_queues_daily(sync):  # noqa: F811
    batch = sync.preparations.submit(request())
    source = reference_job(sync)
    published = sync.publish(source["id"], source["token"], evidence(source, incomplete=True))
    assert published["manifest"]["state"] == "PUBLISHED"
    state = Preparations(sync).get(batch.id)
    assert state.daily_state == "IDENTITY_REJECTED" and state.identity_error
    assert state.identity is None and len(state.tasks) == 2
    assert scheduler(sync.engine).claim("test") is None


def test_reference_and_consumer_submission_rollback_together(sync, monkeypatch):  # noqa: F811
    batch = sync.preparations.submit(request())
    source = reference_job(sync)
    content = evidence(source)
    original = sync.tasks

    def broken(conn, commands):
        original.submit_batch(conn, commands)
        raise RuntimeError("simulated consumer enqueue failure")

    monkeypatch.setattr(sync, "tasks", replace(original, submit_batch=broken))
    with pytest.raises(RuntimeError):
        sync.publish(source["id"], source["token"], content)
    assert Preparations(sync).get(batch.id).daily_state == "WAITING_REFERENCE"
    with sync.engine.connect() as conn:
        assert not conn.execute(
            select(versions.c.id).where(versions.c.job_id == source["id"])
        ).first()
        assert len(conn.execute(select(jobs.c.id)).all()) == 2
    monkeypatch.setattr(sync, "tasks", original)
    sync.publish(source["id"], source["token"], content)
    assert len(sync.preparations.get(batch.id).tasks) == 3


def test_retry_is_single_branch_and_preserves_batch_snapshot(sync):  # noqa: F811
    sync.preparations.submit(request())
    original = reference_job(sync)
    scheduler(sync.engine).fail(original["id"], original["token"], "fixture failure")
    first = sync.retry(original["id"], "retry", resume=True)
    assert sync.retry(original["id"], "retry", resume=True)["id"] == first["id"]
    assert first["payload"]["configuration"] == original["payload"]["configuration"]
    with pytest.raises(Conflict, match="并行分支"):
        sync.retry(original["id"], "another-retry", resume=True)


def test_reference_outside_request_lifecycle_never_queues_daily(sync):  # noqa: F811
    batch = sync.preparations.submit(request())
    source = reference_job(sync)
    sync.publish(source["id"], source["token"], evidence(source, last_day="20240102"))
    state = sync.preparations.get(batch.id)
    assert state.daily_state == "IDENTITY_REJECTED"
    assert state.identity is None
    assert scheduler(sync.engine).claim("daily") is None


def test_backup_rejects_corrupt_preparation_journal_without_rewrite(sync):  # noqa: F811
    from asterion_bindings.files import read_files
    from storage_support import raw_engine

    from asterion.data.backup import load_evidence, validate_backup

    sync.preparations.submit(request())
    with raw_engine(sync.engine).connect() as conn:
        accepted = load_evidence(
            conn,
            ArtifactStore(sync.root, read_only=True),
            read_files(sync.root),
            lambda value: value,
        )
    validate_backup(accepted)
    damaged = accepted.preparations[0] | {"daily_state": "SUBMITTED"}
    with pytest.raises(ValueError, match="缺少固定来源身份"):
        validate_backup(replace(accepted, preparations=(damaged,)))
    assert sync.preparations.list()[0].daily_state == "WAITING_REFERENCE"


def test_refill_and_retry_keep_published_identity_and_dataset(sync):  # noqa: F811
    import json

    from asterion.data.coverage import CoverageRequest

    sync.preparations.submit(request())
    source = reference_job(sync)
    sync.publish(source["id"], source["token"], evidence(source))
    daily = scheduler(sync.engine).claim("daily")
    partial = json.loads(evidence(daily))
    partial[0]["rows"] = [row for row in partial[0]["rows"] if row["trade_date"] != "20240103"]
    published = sync.publish(daily["id"], daily["token"], canonical(partial))
    report = sync.coverage.check(
        published["id"], CoverageRequest(start="2024-01-02", end="2024-01-04")
    )
    assert report["status"] == "GAPS"
    sync.coverage.refill(report["id"], "refill")
    first = scheduler(sync.engine).claim("refill")
    scheduler(sync.engine).fail(first["id"], first["token"], "fixture failure")
    sync.retry(first["id"], "refill-retry")
    retried = scheduler(sync.engine).claim("retry")
    assert retried["payload"]["contract_identity"] == daily["payload"]["contract_identity"]
    result = sync.publish(retried["id"], retried["token"], evidence(retried))
    assert result["dataset_id"] == published["dataset_id"] and result["rows"] == 3
