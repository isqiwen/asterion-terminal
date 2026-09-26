from configuration_support import set_token
from credential_helpers import provider_secrets
from storage_support import data_store, domain_tasks, scheduler
from sync_identity_support import submit_source

"""Real PostgreSQL concurrency acceptance; requires an isolated test database."""

import os
from concurrent.futures import ThreadPoolExecutor
from datetime import UTC, date, datetime
from uuid import uuid4

import pytest
from asterion_bindings.database import create_engine

from asterion.platform.store import metadata


@pytest.mark.skipif(
    not os.getenv("ASTERION_TEST_DATABASE_URL"), reason="PostgreSQL test URL not set"
)
def test_concurrent_workers_claim_each_job_once():
    engine = create_engine(os.environ["ASTERION_TEST_DATABASE_URL"])
    metadata.create_all(engine)
    tasks = scheduler(engine)
    submitted = {
        tasks.submit(str(uuid4()), "data.import_csv", {"csv": "test"})["id"] for _ in range(16)
    }
    with ThreadPoolExecutor(max_workers=8) as pool:
        claims = list(pool.map(lambda n: tasks.claim(f"worker-{n}"), range(16)))
    ids = [job["id"] for job in claims if job]
    assert len(ids) == len(set(ids)) == 16
    assert set(ids) == submitted
    for job in claims:
        tasks.cancel(job["id"])
    engine.dispose()


@pytest.mark.skipif(
    not os.getenv("ASTERION_TEST_DATABASE_URL"), reason="PostgreSQL test URL not set"
)
def test_concurrent_observation_retries_are_immutable(tmp_path):
    from asterion_bindings.task_repository import Conflict

    from asterion.data.providers.public import SyncRequest
    from asterion.data.sync import DataSync

    engine = create_engine(os.environ["ASTERION_TEST_DATABASE_URL"])
    metadata.create_all(engine)
    tasks = scheduler(engine)
    sync = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path,
        provider_secrets("synthetic-test-master-key", tmp_path),
    )
    set_token(sync, "tushare", "synthetic-test-credential")
    request = SyncRequest(
        command_id=str(uuid4()),
        provider="tushare",
        dataset="calendar",
        exchange="SHFE",
        start=date(2024, 1, 2),
        end=date(2024, 1, 2),
    )
    submit_source(sync, request)
    job = tasks.claim("evidence-worker")
    value = {
        "partition": sync.registry.get("tushare").plan(request)[0].model_dump(),
        "observed_at": datetime.now(UTC).isoformat(),
        "rows": [
            {"exchange": "SHFE", "cal_date": "20240102", "is_open": 1, "pretrade_date": "20231229"}
        ],
    }
    try:
        with ThreadPoolExecutor(max_workers=8) as pool:
            saved = list(
                pool.map(
                    lambda _: sync.evidence.record(job["id"], job["token"], 0, value), range(16)
                )
            )
        assert all(record == saved[0] for record in saved)
        assert sync.evidence.list(job["id"])["total"] == 1
        with pytest.raises(Conflict):
            sync.evidence.record(job["id"], job["token"], 0, value | {"rows": []})
        tasks.cancel(job["id"])
        with pytest.raises(Conflict):
            sync.evidence.record(job["id"], job["token"], 0, value)
        assert sync.evidence.preview(job["id"], 1, 0)["rows"] == value["rows"]
        sync.retry(job["id"], str(uuid4()), resume=True)
        resumed = tasks.claim("resume-worker")
        with ThreadPoolExecutor(max_workers=8) as pool:
            restored = list(
                pool.map(lambda _: sync.evidence.resume(resumed["id"], resumed["token"]), range(8))
            )
        assert all(result == restored[0] for result in restored)
        assert list(restored[0]) == [0]
        assert sync.evidence.list(resumed["id"])["total"] == 1
        tasks.cancel(resumed["id"])
    finally:
        engine.dispose()
