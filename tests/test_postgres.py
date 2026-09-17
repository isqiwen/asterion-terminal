"""Real PostgreSQL concurrency acceptance; requires an isolated test database."""

import os
from concurrent.futures import ThreadPoolExecutor
from uuid import uuid4

import pytest
from sqlalchemy import create_engine

from asterion.platform.store import metadata
from asterion.platform.tasks.service import Tasks


@pytest.mark.skipif(
    not os.getenv("ASTERION_TEST_DATABASE_URL"), reason="PostgreSQL test URL not set"
)
def test_concurrent_workers_claim_each_job_once():
    engine = create_engine(os.environ["ASTERION_TEST_DATABASE_URL"])
    metadata.create_all(engine)
    tasks = Tasks(engine)
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
