"""Import tasks through the Rust executor step: leases, cancellation, rollback."""

import hashlib
import time

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.task_repository import Conflict
from import_support import chart_bars, publish_import
from rules_support import intraday_options
from storage_support import data_store, scheduler

from asterion.data.library import DataLibrary
from asterion.platform.store import jobs, metadata

CSV = """contract,event_time,available_at,trading_day,open,high,low,close,volume
SHFE.rb2610,2026-09-14T13:00:00Z,2026-09-14T13:01:00Z,2026-09-15,3200,3220,3190,3210,100
SHFE.rb2610,2026-09-14T13:01:00Z,2026-09-14T13:02:00Z,2026-09-15,3210,3230,3200,3220,120
"""


@pytest.fixture
def context(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/test.db")
    metadata.create_all(engine)
    yield engine, scheduler(engine), DataLibrary(data_store(engine), tmp_path), tmp_path
    engine.dispose()


def submit(tasks, command="one", source="fixture"):
    payload = {"options": intraday_options(), "csv": CSV, "source": source}
    return tasks.submit(command, "data.import_csv", payload)


def test_import_idempotency_and_fixed_precision(context):
    engine, tasks, _, root = context
    job = submit(tasks)
    assert tasks.submit("one", "data.import_csv", job["payload"])["id"] == job["id"]
    with pytest.raises(Conflict):
        tasks.submit("one", "data.import_csv", {"csv": "changed"})
    claim = tasks.claim("worker")
    snapshot = publish_import(engine, root, claim)
    rows = chart_bars(engine, root, snapshot["id"])
    assert rows[0]["close"] == "3210.00000000"
    assert rows[0]["trading_day"] == "2026-09-15"
    assert tasks.list()[0]["state"] == "SUCCEEDED"


def test_stale_or_cancelled_lease_cannot_publish(context):
    engine, tasks, library, root = context
    submit(tasks)
    old = tasks.claim("old")
    with engine.begin() as conn:
        conn.execute(jobs.update().values(lease_until=time.time() - 1))
    new = tasks.claim("new")
    assert new["attempt"] == 2 and new["token"] != old["token"]
    with pytest.raises(KeyError):
        publish_import(engine, root, old)
    assert library.list(include_archived=True)["total"] == 0
    publish_import(engine, root, new)
    cancelled = submit(tasks, "two")
    claim = tasks.claim("worker")
    tasks.cancel(cancelled["id"])
    with pytest.raises(KeyError):
        publish_import(engine, root, claim)
    assert tasks.claim("worker") is None


def test_failed_file_write_rolls_back_the_publication(context):
    engine, tasks, library, root = context
    submit(tasks, "interrupted")
    claim = tasks.claim("worker")
    (root / "published").write_text("not a directory")
    with pytest.raises(ValueError):
        publish_import(engine, root, claim)
    assert library.list(include_archived=True)["total"] == 0
    assert tasks.list()[0]["state"] == "RUNNING"


def test_file_import_uses_shared_catalog_and_keeps_exact_admitted_csv(context):
    engine, tasks, library, root = context
    for command in ("first", "second"):
        submit(tasks, command)
        publish_import(engine, root, tasks.claim("worker"))
    result = library.list(layer="STANDARD", source="local_file")
    assert result["total"] == 1
    version = result["items"][0]
    assert version["version_count"] == 2
    assert version["manifest"]["type"]["id"] == "futures.bars"
    assert version["manifest"]["type"]["frequency"] == "1m"
    raw = library.preview(version["manifest"]["inputs"][0])
    assert raw["rows"][0]["contract"] == "SHFE.rb2610"
    assert raw["version"]["manifest"]["format"] == "csv"
    assert raw["version"]["manifest"]["checksum"] == hashlib.sha256(CSV.encode()).hexdigest()
    assert len(list((root / "imports").glob("*/source.csv"))) == 2
    # Collection identity is the stable name of the published identity.
    from asterion.data.library import stable_id

    with engine.connect() as conn:
        identity = conn.exec_driver_sql(
            "SELECT identity FROM data_collections WHERE id = ?", (version["dataset_id"],)
        ).scalar_one()
    import json

    assert (
        stable_id(json.loads(identity) if isinstance(identity, str) else identity)
        == version["dataset_id"]
    )


def test_import_without_current_options_fails_at_execution(context):
    engine, tasks, _, root = context
    tasks.submit("unsupported", "data.import_csv", {"source": "fixture", "csv": CSV})
    with pytest.raises(ValueError, match="缺少当前数据规范"):
        publish_import(engine, root, tasks.claim("worker"))
