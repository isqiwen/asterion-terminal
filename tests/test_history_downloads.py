"""History admission, durable recovery and coverage use actual sync publication."""

from datetime import UTC, date, datetime, timedelta
from uuid import uuid4

import pytest
from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.task_repository import Conflict
from sqlalchemy import func, select
from storage_support import raw_engine, scheduler
from test_coverage import calendar
from test_coverage import sync as sync  # noqa: PLC0414

from asterion.data.history import HistoryDownloads, HistoryRequest
from asterion.data.providers.public import SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs


def references(sync):
    request = SyncRequest(
        command_id=str(uuid4()), provider="tushare", dataset="contracts", exchange="SHFE"
    )
    sync.submit(request)
    job = scheduler(sync.engine).claim("reference")
    part = Tushare().plan(request)[0]
    rows = []
    for code in ("RB", "HC"):
        row = {
            "ts_code": f"{code}2405.SHF",
            "exchange": "SHFE",
            "name": "合成合约",
            "fut_code": code,
            "d_month": "202405",
            "last_ddate": None,
            "list_date": "20240102",
            "delist_date": "20240515",
            "per_unit": 10,
            "trade_unit": "吨",
            "quote_unit": "元/吨",
        }
        rows.append({f: row.get(f) for f in part.fields})
    contracts = sync.publish(
        job["id"],
        job["token"],
        canonical(
            [
                {
                    "partition": part.model_dump(),
                    "rows": rows,
                    "observed_at": datetime.now(UTC).isoformat(),
                }
            ]
        ),
    )
    days = {str(date(2024, 1, 1) + timedelta(days=i)): 1 for i in range(34)}
    cal = calendar(sync, days)
    return HistoryRequest(
        command_id=uuid4(),
        provider="tushare",
        exchange="SHFE",
        contracts_version_id=contracts["id"],
        calendar_version_id=cal["id"],
        symbols=("RB2405.SHF", "HC2405.SHF"),
        start="2024-01-01",
        end="2024-02-02",
    )


def publish_job(sync, job, *, omit=None):
    request = SyncRequest.model_validate(job["payload"]["request"])
    evidence = []
    for part in Tushare().plan(request):
        first = date.fromisoformat(part.params["start_date"])
        last = date.fromisoformat(part.params["end_date"])
        rows = []
        for i in range((last - first).days + 1):
            day = first + timedelta(days=i)
            if (request.symbol, str(day)) == omit:
                continue
            value = {
                "ts_code": request.symbol,
                "trade_date": day.strftime("%Y%m%d"),
                "open": 100,
                "high": 110,
                "low": 90,
                "close": 100,
                "settle": 100,
                "vol": 100,
            }
            rows.append({f: value.get(f) for f in part.fields})
        evidence.append(
            {
                "partition": part.model_dump(),
                "rows": rows,
                "observed_at": datetime.now(UTC).isoformat(),
            }
        )
    return sync.publish(job["id"], job["token"], canonical(evidence))


def test_monthly_atomic_submission_replay_and_fixed_coverage(sync):
    request = references(sync)
    service = HistoryDownloads(sync)
    plan = service.plan(request)
    assert len(plan.slices) == 4
    assert plan.expected_days == 64  # Two declared contracts, Jan 2 through Feb 2.
    assert {s.request.start for s in plan.slices} == {date(2024, 1, 2), date(2024, 2, 1)}
    batch = service.submit(request)
    assert len(batch.tasks) == 4
    assert service.submit(request) == batch
    assert len(HistoryDownloads(sync).get(request.command_id).tasks) == 4
    assert not service.coverage(request.command_id).complete
    changed = request.model_copy(update={"end": date(2024, 2, 1)})
    with pytest.raises(Conflict):
        service.submit(changed)
    while job := scheduler(sync.engine).claim("history-test"):
        publish_job(sync, job, omit=("RB2405.SHF", "2024-01-03"))
    result = service.coverage(request.command_id)
    assert not result.complete
    assert {i.symbol: i.report.status for i in result.items} == {
        "HC2405.SHF": "COVERED",
        "RB2405.SHF": "GAPS",
    }
    assert result.counts["GAP"] == 1
    assert result.counts["PRESENT"] == 63
    # Later unrelated publication does not silently change this batch's fixed output versions.
    reports = {i.report.id for i in result.items}
    assert {i.report.id for i in service.coverage(request.command_id).items} == reports


def test_bad_evidence_and_admission_failure_leave_no_partial_jobs(sync, monkeypatch):
    body = references(sync)
    service = HistoryDownloads(sync)
    missing = body.model_copy(update={"end": date(2024, 2, 4)})
    with pytest.raises(ValueError, match="日历"):
        service.submit(missing)
    bad = body.model_copy(update={"symbols": ("RB2405.SHF", "UNKNOWN")})
    with pytest.raises(ValueError, match="来源代码"):
        service.submit(bad)
    original = sync.validate_identity
    calls = 0

    def fail(conn, payload):
        nonlocal calls
        calls += 1
        if calls == 3:
            raise ValueError("synthetic admission failure")
        return original(conn, payload)

    monkeypatch.setattr(sync, "validate_identity", fail)
    with pytest.raises(ValueError, match="synthetic"):
        service.submit(body)
    with sync.engine.connect() as conn:
        assert (
            conn.execute(
                select(func.count()).select_from(jobs).where(jobs.c.command_id.like("history:%"))
            ).scalar_one()
            == 0
        )


def test_retry_remains_in_history_and_can_complete_coverage(sync):
    body = references(sync)
    service = HistoryDownloads(sync)
    service.submit(body)
    failed = scheduler(sync.engine).claim("history-test")
    scheduler(sync.engine).fail(failed["id"], failed["token"], "synthetic interruption")
    retried = sync.retry(failed["id"], str(uuid4()))
    assert retried["id"] in {j.id for j in service.get(body.command_id).tasks}
    while job := scheduler(sync.engine).claim("history-test"):
        publish_job(sync, job)
    assert service.coverage(body.command_id).complete
    assert len(service.get(body.command_id).tasks) == 5


def test_scoped_api_submission_and_restart_readback(sync):
    from fastapi.testclient import TestClient
    from storage_support import raw_engine

    from asterion.api.app import create_app
    from asterion.platform.config import Settings

    body = references(sync)
    settings = Settings(
        token="synthetic-coverage-master-key", data_root=sync.root, require_account=False
    )
    with TestClient(create_app(settings, raw_engine(sync.engine))) as client:
        path = "/api/v1/data/history"
        payload = body.model_dump(mode="json")
        assert client.post(path + "/plan", json=payload).status_code == 401
        assert client.get(path).status_code == 401
        client.headers["Authorization"] = "Bearer " + client.scope("research")
        assert client.post(path, json=payload).status_code == 401
        client.headers["Authorization"] = "Bearer " + client.scope("data")
        preview = client.post(path + "/plan", json=payload)
        assert preview.status_code == 200, preview.text
        assert len(preview.json()["slices"]) == 4
        created = client.post(path, json=payload)
        assert created.status_code == 202, created.text
        summaries = client.get(path).json()
        assert len(summaries) == 1
        assert summaries[0]["request"]["command_id"] == str(body.command_id)
        assert summaries[0]["task_count"] == 4
        assert "configuration" not in summaries[0]
        assert client.get(path + f"/{body.command_id}").json() == created.json()
        assert client.post(path, json=payload).json() == created.json()
        result = client.post(path + f"/{body.command_id}/coverage")
        assert result.status_code == 200
        assert result.json()["complete"] is False
        assert client.get(path + f"/{uuid4()}").status_code == 404


def test_response_loss_replay_preserves_original_configuration(sync, monkeypatch):
    body = references(sync)
    service = HistoryDownloads(sync)
    original = service.submit(body)
    monkeypatch.setattr(
        sync, "submission_payload", lambda _: pytest.fail("Must retain accepted configuration")
    )
    assert service.submit(body) == original
    with sync.engine.connect() as conn:
        payloads = list(
            conn.execute(
                select(jobs.c.payload).where(jobs.c.command_id.like(f"history:{body.command_id}:%"))
            ).scalars()
        )
    assert all(p["configuration"] == payloads[0]["configuration"] for p in payloads)
    assert "history_plan" in payloads[0] or any("history_plan" in p for p in payloads)


def test_history_references_survive_backup_validation(sync):
    from dataclasses import replace

    from asterion_bindings.files import read_files
    from credential_helpers import provider_secrets
    from entry_support import entry_lifecycle

    from asterion.data.backup import load_evidence, validate_backup

    body = references(sync)
    HistoryDownloads(sync).submit(body)
    with sync.engine.connect() as conn:
        evidence = load_evidence(
            conn,
            ArtifactStore(sync.root, read_only=True),
            read_files(sync.root),
            provider_secrets("synthetic-coverage-master-key", sync.root).opens,
        )
    validate_backup(evidence)
    assert len(evidence.history_plans) == 1
    with entry_lifecycle(str(raw_engine(sync.engine).url), sync.root) as manager:
        refs = manager.inspect(body.calendar_version_id)
    assert refs["references"]["history_plans"] == 1
    damaged = tuple(r for r in evidence.versions if r["id"] != body.calendar_version_id)
    with pytest.raises(ValueError, match="历史计划固定依据"):
        validate_backup(replace(evidence, versions=damaged))


def test_concurrent_response_loss_retry_inserts_one_batch(sync, monkeypatch):
    from concurrent.futures import ThreadPoolExecutor
    from threading import Barrier

    body = references(sync)
    service = HistoryDownloads(sync)
    original = service.plan
    ready = Barrier(2)

    def together(value):
        plan = original(value)
        ready.wait(timeout=10)
        return plan

    monkeypatch.setattr(service, "plan", together)
    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(service.submit, [body, body]))
    assert {j.id for j in results[0].tasks} == {j.id for j in results[1].tasks}
    assert len(service.get(body.command_id).tasks) == 4
