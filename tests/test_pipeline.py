import time

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine

from asterion.api.app import create_app
from asterion.data.public import encode_csv, read_bars
from asterion.data.snapshots import Snapshots
from asterion.platform.config import Settings
from asterion.platform.store import jobs, metadata
from asterion.platform.tasks.service import Conflict, Tasks

CSV = """contract,event_time,available_at,trading_day,open,high,low,close,volume
SHFE.rb2610,2026-09-14T13:00:00Z,2026-09-14T13:01:00Z,2026-09-15,3200,3220,3190,3210,100
SHFE.rb2610,2026-09-14T13:01:00Z,2026-09-14T13:02:00Z,2026-09-15,3210,3230,3200,3220,120
"""


@pytest.fixture
def context(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/test.db")
    metadata.create_all(engine)
    tasks = Tasks(engine)
    return engine, tasks, Snapshots(engine, tasks, tmp_path), tmp_path


def test_import_idempotency_and_fixed_precision(context):
    _, tasks, data, _ = context
    job = tasks.submit("one", "data.import_csv", {"csv": CSV, "source": "test fixture"})
    assert tasks.submit("one", "data.import_csv", job["payload"])["id"] == job["id"]
    with pytest.raises(Conflict):
        tasks.submit("one", "data.import_csv", {"csv": "changed"})
    claim = tasks.claim("worker")
    content, _ = encode_csv(CSV)
    snapshot = data.publish(claim["id"], claim["token"], content)
    assert data.publish(claim["id"], claim["token"], content) == snapshot
    rows = read_bars(data.path(snapshot["id"]))
    assert rows[0]["close"] == "3210.00000000"
    assert rows[0]["trading_day"] == "2026-09-15"
    assert tasks.list()[0]["state"] == "SUCCEEDED"


def test_stale_worker_cannot_publish_or_heartbeat(context):
    engine, tasks, data, _ = context
    tasks.submit("one", "data.import_csv", {"csv": CSV, "source": "fixture"})
    old = tasks.claim("old")
    with engine.begin() as conn:
        conn.execute(jobs.update().values(lease_until=time.time() - 1))
    new = tasks.claim("new")
    assert new["attempt"] == 2 and new["token"] != old["token"]
    with pytest.raises(Conflict):
        tasks.heartbeat(old["id"], old["token"])
    with pytest.raises(Conflict):
        data.publish(old["id"], old["token"], encode_csv(CSV)[0])
    assert data.list() == []
    data.publish(new["id"], new["token"], encode_csv(CSV)[0])


def test_cancel_and_invalid_worker_output(context):
    _, tasks, data, _ = context
    job = tasks.submit("one", "data.import_csv", {"csv": CSV, "source": "fixture"})
    claim = tasks.claim("worker")
    with pytest.raises(ValueError):
        data.publish(job["id"], claim["token"], b"corrupt")
    assert data.list() == []
    tasks.cancel(job["id"])
    with pytest.raises(Conflict):
        data.publish(job["id"], claim["token"], encode_csv(CSV)[0])
    assert tasks.claim("worker") is None


@pytest.mark.parametrize(
    "csv",
    [
        CSV + CSV.splitlines()[1] + "\n",
        CSV.replace("SHFE.rb2610", "RB.CONT"),
        CSV.replace("3220,3190", "3100,3190"),
        CSV.replace("13:01:00Z,2026", "12:00:00Z,2026"),
        CSV.replace("3200,3220", "NaN,3220"),
        CSV.splitlines()[0],
    ],
)
def test_invalid_source_rejected(csv):
    with pytest.raises(ValueError):
        encode_csv(csv)


def test_api_auth_and_pipeline(context):
    engine, _, _, root = context
    settings = Settings(token="test-session-token-long-enough", data_root=root)
    with TestClient(create_app(settings, engine)) as client:
        assert client.get("/api/v1/jobs").status_code == 401
        client.headers["Authorization"] = f"Bearer {settings.token}"
        assert client.get("/api/v1/health").status_code == 200
        response = client.post(
            "/api/v1/imports", json={"command_id": "api", "source": "test", "csv": CSV}
        )
        assert response.status_code == 202
        job = client.post("/api/v1/jobs/claim", json={"worker_id": "api-worker"}).json()
        response = client.post(
            f"/api/v1/jobs/{job['id']}/publish",
            content=encode_csv(CSV)[0],
            headers={"X-Lease-Token": job["token"]},
        )
        assert response.status_code == 200
        assert len(client.get(f"/api/v1/snapshots/{response.json()['id']}/bars").json()) == 2
        assert client.get("/api/v1/jobs").json()[0]["state"] == "SUCCEEDED"


def test_interrupted_file_write_never_publishes_catalog(context, monkeypatch):
    _, tasks, data, _ = context
    tasks.submit("interrupted", "data.import_csv", {"csv": CSV, "source": "fixture"})
    claim = tasks.claim("worker")

    def interrupted(_):
        raise OSError("simulated disk failure")

    monkeypatch.setattr("asterion.data.snapshots.os.fsync", interrupted)
    with pytest.raises(OSError):
        data.publish(claim["id"], claim["token"], encode_csv(CSV)[0])
    assert data.list() == []
    assert tasks.list()[0]["state"] == "RUNNING"


def test_subsecond_bars_rejected_by_v1_display_contract():
    with pytest.raises(ValueError, match="whole-second"):
        encode_csv(CSV.replace("13:00:00Z", "13:00:00.123Z"))


def test_file_import_uses_shared_catalog_and_keeps_exact_admitted_csv(context):
    _, tasks, data, root = context
    for command in ("first", "second"):
        tasks.submit(command, "data.import_csv", {"csv": CSV, "source": "fixture"})
        job = tasks.claim("worker")
        data.publish(job["id"], job["token"], encode_csv(CSV)[0])
    result = data.library.list(layer="STANDARD", source="local_file")
    assert result["total"] == 1
    version = result["items"][0]
    assert version["version_count"] == 2
    assert version["manifest"]["type"]["id"] == "futures.bars"
    assert version["manifest"]["type"]["frequency"] == "unspecified"
    raw = data.library.preview(version["manifest"]["inputs"][0])
    assert raw["rows"][0]["contract"] == "SHFE.rb2610"
    assert raw["version"]["manifest"]["format"] == "csv"
    assert (
        raw["version"]["manifest"]["checksum"]
        == __import__("hashlib").sha256(CSV.encode()).hexdigest()
    )
    assert len(list((root / "imports").glob("*/source.csv"))) == 2
