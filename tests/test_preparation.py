from dataclasses import replace

from configuration_support import set_token
from credential_helpers import provider_secrets
from storage_support import data_store, domain_tasks, raw_engine, scheduler

"""Research preparation is atomic, reproducible and independent of provider capability IDs."""

import json
import os
from concurrent.futures import ThreadPoolExecutor
from datetime import UTC, datetime
from uuid import uuid4

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.task_repository import Conflict
from fastapi.testclient import TestClient
from sqlalchemy import select, text

from asterion.api.app import create_app
from asterion.data.coverage import CoverageRequest
from asterion.data.preparation import TYPES, batches
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.data.sync import DataSync
from asterion.platform.config import Settings
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs, metadata


@pytest.fixture
def sync(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/prepare.db")
    metadata.create_all(engine)
    service = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path,
        provider_secrets("preparation-test-master-key-long-enough", tmp_path),
    )
    set_token(service, "tushare", "fixture-private-token")
    yield service
    engine.dispose()


def request(**changes):
    return SyncRequest.model_validate(
        {
            "command_id": "research-inputs",
            "provider": "tushare",
            "dataset": "daily",
            "exchange": "SHFE",
            "symbol": "RB2610.SHF",
            "start": "2024-01-02",
            "end": "2024-01-04",
        }
        | changes
    )


def test_batch_freezes_once_and_idempotency_survives_configuration_and_lifecycle_changes(
    sync, monkeypatch
):
    connection = sync.sources.create("tushare", "研究专用")
    sync.sources.apply(connection["id"], 0, secrets={"token": "named-private-token"})
    calls = []
    original = sync.submission_payload_in

    def freeze(conn, request):
        calls.append(request.connection_id)
        return original(conn, request)

    monkeypatch.setattr(sync, "submission_payload_in", freeze)
    body = request(connection_id=connection["id"])
    batch = sync.preparations.submit(body)
    assert batch.connection_name == "研究专用" and len(batch.tasks) == 2
    assert batch.daily_state == "WAITING_REFERENCE" and batch.identity is None
    assert calls == [connection["id"]]
    with sync.engine.connect() as conn:
        payloads = conn.execute(select(jobs.c.payload)).scalars().all()
        payloads.append(conn.execute(select(batches.c.daily_payload)).scalar_one())
    assert {p["type_id"] for p in payloads} == set(TYPES)
    assert len({json.dumps(p["configuration"], sort_keys=True) for p in payloads}) == 1
    assert all(p["request"]["connection_id"] == connection["id"] for p in payloads)
    refs = {p["type_id"]: p["request"] for p in payloads}
    assert refs["futures.calendar"]["start"] == "2024-01-02"
    assert refs["futures.calendar"]["symbol"] == ""
    assert refs["futures.contracts"]["start"] is None
    assert refs["futures.contracts"]["symbol"] == ""
    sync.sources.apply(connection["id"], 1, secrets={"token": "changed-private-token"})
    sync.sources.update(connection["id"], 0, "停用连接", "disabled")
    # An accepted batch is replayed without fixing the configuration again.
    assert sync.preparations.submit(body).id == batch.id and len(calls) == 1
    assert len(scheduler(sync.engine).list()) == 2
    assert (
        "private-token" not in batch.model_dump_json()
        and '"payload"' not in batch.model_dump_json()
    )
    with pytest.raises(Conflict):
        sync.preparations.submit(body.model_copy(update={"symbol": "RB2611.SHF"}))
    with pytest.raises(ProviderError):
        sync.preparations.submit(body.model_copy(update={"command_id": "disabled-new"}))


def test_validation_or_partial_insert_failure_publishes_no_batch_or_jobs(sync, monkeypatch):
    for body in (
        request(exchange="GFEX", symbol="SI2610.GFE"),
        request(symbol="continuous"),
        request(dataset="contracts"),
    ):
        with pytest.raises(ProviderError):
            sync.preparations.submit(body)

    def broken(conn, commands):
        original(conn, commands[:1])
        raise RuntimeError("simulated transaction failure")

    original = sync.tasks.submit_batch
    monkeypatch.setattr(sync, "tasks", replace(sync.tasks, submit_batch=broken))
    with pytest.raises(RuntimeError):
        sync.preparations.submit(request())
    assert scheduler(sync.engine).list() == [] and sync.preparations.list() == []
    monkeypatch.setattr(sync, "tasks", replace(sync.tasks, submit_batch=original))
    assert len(sync.preparations.submit(request()).tasks) == 2


def test_retry_and_restart_keep_batch_and_completed_inputs(sync):
    batch = sync.preparations.submit(request())
    first = scheduler(sync.engine).claim("worker")
    scheduler(sync.engine).fail(first["id"], first["token"], "permission missing")
    retried = sync.retry(first["id"], "retry-one", resume=True)
    restored = DataSync(
        data_store(sync.engine),
        domain_tasks(data_store(sync.engine), "data"),
        sync.root,
        provider_secrets("preparation-test-master-key-long-enough", sync.root),
    )
    current = restored.preparations.get(batch.id)
    assert len(current.tasks) == 2
    assert retried["id"] in {task.job.id for task in current.tasks}
    assert first["id"] not in {task.job.id for task in current.tasks}
    assert restored.preparations.list()[0].id == batch.id
    assert len(scheduler(restored.engine).list()) == 3


def test_atomic_three_inputs_publish_and_check_exact_versions(sync):
    batch = sync.preparations.submit(request())
    while job := scheduler(sync.engine).claim("publish-preparation"):
        req = SyncRequest.model_validate(job["payload"]["request"])
        part = Tushare().plan(req)[0]
        if req.dataset == "contracts":
            rows = [
                {
                    "ts_code": "RB2610.SHF",
                    "exchange": "SHFE",
                    "name": "fixture",
                    "fut_code": "RB",
                    "d_month": "202610",
                    "last_ddate": None,
                    "list_date": "20240101",
                    "delist_date": "20240131",
                    "per_unit": 10,
                    "trade_unit": "吨",
                    "quote_unit": "元/吨",
                }
            ]
        elif req.dataset == "calendar":
            rows = [
                {
                    "exchange": "SHFE",
                    "cal_date": f"2024010{d}",
                    "is_open": 1,
                    "pretrade_date": "20240101",
                }
                for d in (2, 3, 4)
            ]
        else:
            rows = [
                {
                    "ts_code": "RB2610.SHF",
                    "trade_date": f"2024010{d}",
                    "open": 10,
                    "high": 12,
                    "low": 9,
                    "close": 11,
                    "vol": 100,
                }
                for d in (2, 3, 4)
            ]
        evidence = [
            {
                "partition": part.model_dump(),
                "rows": [{field: row.get(field) for field in part.fields} for row in rows],
                "observed_at": datetime.now(UTC).isoformat(),
            }
        ]
        sync.publish(job["id"], job["token"], canonical(evidence))
    current = sync.preparations.get(batch.id)
    assert current.daily_state == "SUBMITTED" and current.identity is not None
    versions = {task.type_id: task.job.result["version_id"] for task in current.tasks}
    checked = sync.coverage.check(
        versions["futures.daily"],
        CoverageRequest(
            start=request().start,
            end=request().end,
            use_latest_daily=False,
            calendar_version_id=versions["futures.calendar"],
            contracts_version_id=versions["futures.contracts"],
        ),
    )
    assert checked["status"] == "COVERED"
    assert checked["counts"]["PRESENT"] == 3
    assert checked["daily_version_id"] == versions["futures.daily"]


def test_api_requires_account_and_returns_safe_current_status(sync):
    settings = Settings(token="preparation-test-master-key-long-enough", data_root=sync.root)
    client = TestClient(create_app(settings, raw_engine(sync.engine)))
    client.headers["Authorization"] = "Bearer preparation-test-master-key-long-enough"
    response = client.post("/api/v1/data/preparations", json=request().model_dump(mode="json"))
    assert response.status_code == 202, response.text
    assert len(response.json()["tasks"]) == 2
    assert client.get("/api/v1/data/preparations/research-inputs").status_code == 200
    assert client.get("/api/v1/data/preparations/missing").status_code == 404
    protected = TestClient(
        create_app(settings.model_copy(update={"require_account": True}), raw_engine(sync.engine))
    )
    protected.headers["Authorization"] = "Bearer preparation-test-master-key-long-enough"
    assert protected.get("/api/v1/data/preparations").status_code == 401
    assert (
        protected.post(
            "/api/v1/data/preparations", json=request().model_dump(mode="json")
        ).status_code
        == 401
    )


@pytest.mark.skipif(not os.getenv("ASTERION_TEST_DATABASE_URL"), reason="PostgreSQL URL required")
def test_postgres_concurrent_same_command_creates_only_reference_jobs(tmp_path):
    url = os.environ["ASTERION_TEST_DATABASE_URL"]
    admin = create_engine(url)
    schema = "preparation_" + uuid4().hex
    with admin.begin() as conn:
        conn.execute(text(f"CREATE SCHEMA {schema}"))
    engine = create_engine(url, connect_args={"options": f"-csearch_path={schema}"})
    try:
        metadata.create_all(engine)
        service = DataSync(
            data_store(engine),
            domain_tasks(data_store(engine), "data"),
            tmp_path,
            provider_secrets("fixture-master", tmp_path),
        )
        set_token(service, "tushare", "fixture-token")
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: service.preparations.submit(request()), range(4)))
        assert len({tuple(sorted(t.job.id for t in r.tasks)) for r in results}) == 1
        assert len(scheduler(service.engine).list()) == 2
        assert len(service.preparations.list()) == 1
        original = scheduler(service.engine).claim("failure")
        scheduler(service.engine).fail(original["id"], original["token"], "test failure")

        def retry_branch(command):
            try:
                return service.retry(original["id"], command)["id"]
            except Conflict:
                return None

        with ThreadPoolExecutor(max_workers=2) as pool:
            branches = list(pool.map(retry_branch, ("branch-one", "branch-two")))
        assert sum(item is not None for item in branches) == 1
        assert len(scheduler(service.engine).list()) == 3
    finally:
        engine.dispose()
        with admin.begin() as conn:
            conn.execute(text(f"DROP SCHEMA {schema} CASCADE"))
        admin.dispose()
