import json
import stat
import time
from datetime import UTC, datetime

import httpx
import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine, select

from asterion.api.app import create_app
from asterion.data.providers import ProviderRegistry
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.data.public import read_bars
from asterion.data.sync import Credentials, DataSync, canonical, collect
from asterion.platform.config import Settings
from asterion.platform.store import jobs, metadata
from asterion.platform.tasks.service import Conflict, Tasks

MASTER = "test-runtime-token-at-least-24-characters"
SECRET = "synthetic-provider-token-never-log"


@pytest.fixture
def context(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/test.db")
    metadata.create_all(engine)
    tasks = Tasks(engine)
    sync = DataSync(engine, tasks, tmp_path, MASTER)
    sync.credentials.save("tushare", SECRET)
    return engine, tasks, sync, tmp_path


def request(dataset="daily", **changes):
    values = {
        "command_id": "sync",
        "provider": "tushare",
        "dataset": dataset,
        "exchange": "SHFE",
        "symbol": "RB2610.SHF" if dataset == "daily" else "",
        "start": "2024-01-02" if dataset != "contracts" else None,
        "end": "2024-01-02" if dataset != "contracts" else None,
    }
    return SyncRequest.model_validate(values | changes)


def raw(partition):
    values = {
        "ts_code": "RB2610.SHF",
        "trade_date": "20240102",
        "open": 3200,
        "high": 3220,
        "low": 3190,
        "close": 3210,
        "vol": 100,
        "amount": 321,
        "oi": 200,
        "settle": 3205,
        "pre_settle": 3200,
        "pre_close": 3201,
        "oi_chg": 1,
        "exchange": "SHFE",
        "cal_date": "20240102",
        "is_open": 1,
        "pretrade_date": "20231229",
        "symbol": "RB2610",
        "name": "测试螺纹",
        "fut_code": "RB",
        "list_date": "20230101",
        "delist_date": "20261015",
        "per_unit": 10,
        "trade_unit": "吨",
        "quote_unit": "元/吨",
    }
    return {k: values.get(k) for k in partition.fields}


def prepared(context, monkeypatch, req=None, transform=None):
    _, tasks, sync, root = context
    sync.submit(req or request())
    job = tasks.claim("worker")

    def fetch(self, partition, credential):
        assert credential == SECRET
        rows = [raw(partition)]
        return transform(rows) if transform else rows

    monkeypatch.setattr(Tushare, "fetch", fetch)
    content = collect(
        job["payload"],
        root,
        MASTER,
        lambda done, total: sync.progress(job["id"], job["token"], done, total),
    )
    return job, content


def test_registry_and_bounded_plan():
    registry = ProviderRegistry((Tushare(),))
    assert registry.get("tushare").manifest.api_version == 1
    with pytest.raises(ValueError):
        registry.register(Tushare())
    with pytest.raises(ProviderError):
        registry.get("unknown")
    plan = Tushare().plan(request(start="2024-01-01", end="2024-03-15"))
    assert len(plan) == 3
    assert plan[0].params["end_date"] == "20240131"
    assert plan[1].params["start_date"] == "20240201"
    assert plan[-1].params["end_date"] == "20240315"
    for req in [
        request(symbol="RB.SHF"),
        request(symbol="RB2610.DCE"),
        request("calendar", exchange="GFEX"),
    ]:
        with pytest.raises(ProviderError):
            Tushare().plan(req)


def test_encrypted_credentials_and_no_secret_in_job(context):
    engine, _tasks, sync, root = context
    path = sync.credentials.path("tushare")
    assert SECRET.encode() not in path.read_bytes()
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700
    assert sync.credentials.read("tushare") == SECRET
    with pytest.raises(ProviderError, match="解密"):
        Credentials(root, "different-master-key").read("tushare")
    assert sync.submit(request())["id"] == sync.submit(request())["id"]
    with engine.connect() as conn:
        assert SECRET not in str(conn.execute(select(jobs)).all())
    assert SECRET not in str(sync.providers())
    sync.credentials.save("tushare", "")
    assert not sync.providers()[0]["configured"]
    with pytest.raises(ProviderError, match="Token"):
        sync.submit(request(command_id="missing"))
    with pytest.raises(ValueError):
        sync.credentials.save("../escape", "invalid")


@pytest.mark.parametrize("dataset", ["daily", "contracts", "calendar"])
def test_publish_retains_evidence_and_daily_chart(context, monkeypatch, dataset):
    _, tasks, sync, root = context
    job, content = prepared(context, monkeypatch, request(dataset))
    published = sync.publish(job["id"], job["token"], content)
    assert sync.publish(job["id"], job["token"], content) == published
    assert tasks.list()[0]["state"] == "SUCCEEDED"
    assert sync.library.preview(published["id"])["total"] == 1
    assert (root / "datasets" / job["id"] / "evidence.json").read_bytes() == content
    assert sync.library.preview(published["id"], 100)["rows"] == []
    with pytest.raises(Conflict):
        sync.publish(job["id"], job["token"], content + b" ")
    if dataset == "daily":
        bars = read_bars(root / "published" / f"{published['manifest']['snapshot_id']}.parquet")
        assert bars[0]["trading_day"] == "2024-01-02"
        assert bars[0]["event_time"].startswith("2024-01-02 00:00:00")
        assert bars[0]["available_at"].startswith(str(datetime.now(UTC).date()))
        assert sync.library.preview(published["id"])["rows"][0]["settle"] == "3205"
    if dataset == "contracts":
        assert sync.library.preview(published["id"])["rows"][0]["rules_status"] == "INCOMPLETE"


@pytest.mark.parametrize(
    "mutation",
    ["empty", "duplicate", "foreign", "bad_price", "truncated", "plan", "time", "partition_date"],
)
def test_invalid_evidence_cannot_publish(context, monkeypatch, mutation):
    _, _, sync, root = context
    req = request(end="2024-02-02") if mutation == "partition_date" else request()
    job, content = prepared(context, monkeypatch, req)
    evidence = json.loads(content)
    if mutation == "empty":
        evidence[0]["rows"] = []
    elif mutation == "duplicate":
        evidence[0]["rows"] *= 2
    elif mutation == "foreign":
        evidence[0]["rows"][0]["ts_code"] = "CU2610.SHF"
    elif mutation == "bad_price":
        evidence[0]["rows"][0]["high"] = 2
    elif mutation == "truncated":
        evidence[0]["rows"] *= 2000
    elif mutation == "plan":
        evidence[0]["partition"]["api"] = "other_api"
    elif mutation == "time":
        evidence[0]["observed_at"] = "2024-01-02T00:00:00+00:00"
    elif mutation == "partition_date":
        evidence[0]["rows"][0]["trade_date"] = "20240202"
    with pytest.raises(ValueError):
        sync.publish(job["id"], job["token"], canonical(evidence))
    assert sync.library.list()["items"] == []
    assert not list((root / "published").glob("*.parquet"))


def test_cancel_expired_lease_and_file_failure(context, monkeypatch):
    engine, tasks, sync, _ = context
    job, content = prepared(context, monkeypatch)
    with engine.begin() as conn:
        conn.execute(jobs.update().values(lease_until=time.time() - 1))
    replacement = tasks.claim("new-worker")
    with pytest.raises(Conflict):
        sync.publish(job["id"], job["token"], content)
    with pytest.raises(Conflict):
        sync.progress(job["id"], job["token"], 1, 1)

    def fail_write(*args):
        raise OSError("disk full")

    monkeypatch.setattr("asterion.data.sync.atomic_write", fail_write)
    with pytest.raises(OSError):
        sync.publish(job["id"], replacement["token"], content)
    assert sync.library.list()["items"] == []
    tasks.cancel(job["id"])
    with pytest.raises(Conflict):
        sync.publish(job["id"], replacement["token"], content)


def test_calendar_gap_and_corrupt_parquet(context, monkeypatch):
    _, _, sync, root = context
    job, content = prepared(context, monkeypatch, request("calendar", end="2024-01-03"))
    with pytest.raises(ProviderError, match="缺少"):
        sync.publish(job["id"], job["token"], content)
    sync.tasks.cancel(job["id"])
    job, content = prepared(context, monkeypatch, request("calendar", command_id="complete"))
    result = sync.publish(job["id"], job["token"], content)
    (root / "datasets" / job["id"] / "data.parquet").write_bytes(b"corrupt")
    with pytest.raises(ProviderError, match="校验和"):
        sync.library.preview(result["id"])


def test_https_transport_and_sanitized_failures(monkeypatch):
    partition = Tushare().plan(request())[0]
    seen = []
    mode = ["success"]

    def handle(req):
        seen.append(req)
        assert req.url == "https://api.tushare.pro"
        assert json.loads(req.content)["token"] == SECRET
        if mode[0] == "deny":
            return httpx.Response(200, json={"code": -1, "msg": f"token {SECRET} 无效"})
        if mode[0] == "retry" and len(seen) < 3:
            return httpx.Response(503)
        return httpx.Response(
            200,
            json={
                "code": 0,
                "data": {"fields": partition.fields, "items": [list(raw(partition).values())]},
            },
        )

    client_type = httpx.Client
    monkeypatch.setattr(
        httpx,
        "Client",
        lambda **kwargs: client_type(transport=httpx.MockTransport(handle), **kwargs),
    )
    monkeypatch.setattr("asterion.data.providers.tushare.time.sleep", lambda _: None)
    assert Tushare().fetch(partition, SECRET)[0]["close"] == 3210
    mode[0] = "deny"
    with pytest.raises(ProviderError) as error:
        Tushare().fetch(partition, SECRET)
    assert SECRET not in str(error.value)
    mode[0] = "retry"
    seen.clear()
    assert Tushare().fetch(partition, SECRET)
    assert len(seen) == 3


def test_api_requires_account_for_provider_configuration(context):
    engine, _, _, root = context
    client = TestClient(
        create_app(Settings(token=MASTER, data_root=root, require_account=True), engine)
    )
    client.headers["Authorization"] = f"Bearer {MASTER}"
    for url, method, body in [
        ("/data/providers", "get", None),
        ("/data/catalog", "get", None),
        ("/data/types", "get", None),
        ("/data/versions/unknown", "get", None),
        ("/data/catalog/unknown/versions", "get", None),
        ("/data/providers/tushare/credential", "post", {"token": SECRET}),
        ("/data/sync", "post", request().model_dump(mode="json")),
    ]:
        response = client.request(method, "/api/v1" + url, json=body)
        assert response.status_code == 401


def test_api_sync_roundtrip(context, monkeypatch):
    engine, _, _, root = context
    client = TestClient(create_app(Settings(token=MASTER, data_root=root), engine))
    client.headers["Authorization"] = f"Bearer {MASTER}"
    assert client.get("/api/v1/data/providers").json()[0]["configured"]
    submitted = client.post("/api/v1/data/sync", json=request().model_dump(mode="json"))
    assert submitted.status_code == 202
    job = client.post("/api/v1/jobs/claim", json={"worker_id": "test"}).json()
    monkeypatch.setattr(Tushare, "fetch", lambda self, part, credential: [raw(part)])
    content = collect(job["payload"], root, MASTER)
    published = client.post(
        f"/api/v1/jobs/{job['id']}/publish-data",
        content=content,
        headers={"X-Lease-Token": job["token"]},
    )
    assert published.status_code == 200, published.text
    assert client.get("/api/v1/data/catalog").json()["items"][0]["rows"] == 1
    assert client.get(f"/api/v1/data/versions/{published.json()['id']}").json()["total"] == 1
    assert client.get("/api/v1/snapshots").json()[0]["manifest"]["frequency"] == "1d"
    assert SECRET not in client.get("/api/v1/jobs").text


def test_retry_preserves_request_and_recovery_from_unreadable_credentials(context, monkeypatch):
    _, tasks, sync, root = context
    job, _ = prepared(context, monkeypatch)
    tasks.fail(job["id"], job["token"], "synthetic failure")
    retried = sync.retry(job["id"], "retry-command")
    assert retried["id"] != job["id"]
    assert retried["payload"]["request"]["symbol"] == "RB2610.SHF"
    assert sync.retry(job["id"], "retry-command")["id"] == retried["id"]
    with pytest.raises(Conflict):
        sync.retry(retried["id"], "not-failed")
    sync.credentials = Credentials(root, "rotated-runtime-key-long-enough")
    assert not sync.providers()[0]["configured"]
    assert sync.providers()[0]["credential_error"]
    sync.credentials.save("tushare", SECRET)
    assert sync.providers()[0]["configured"]


@pytest.mark.parametrize("invalid", [False, True])
def test_worker_dispatch_uses_sync_protocol_and_reports_validation(context, monkeypatch, invalid):
    from concurrent.futures import ThreadPoolExecutor

    from asterion.runtime.worker import run_once

    engine, tasks, sync, root = context
    settings = Settings(token=MASTER, data_root=root, api_url="http://worker.test")
    server = TestClient(create_app(settings, engine))
    sync.submit(request())

    def fetch(self, partition, credential):
        row = raw(partition)
        if invalid:
            row["high"] = 1
        return [row]

    monkeypatch.setattr(Tushare, "fetch", fetch)
    monkeypatch.setattr(
        "asterion.runtime.worker.ProcessPoolExecutor",
        lambda **kwargs: ThreadPoolExecutor(max_workers=1),
    )
    client_type = httpx.Client

    def transport(req):
        return server.request(
            req.method, req.url.path, content=req.content, headers=dict(req.headers)
        )

    monkeypatch.setattr(
        httpx,
        "Client",
        lambda **kwargs: client_type(transport=httpx.MockTransport(transport), **kwargs),
    )
    assert run_once(settings, "worker-integration")
    job = tasks.list()[0]
    assert job["state"] == ("FAILED" if invalid else "SUCCEEDED")
    if invalid:
        assert "边界" in job["error"]
        assert not sync.library.list()["items"]
    else:
        assert job["result"]["completed"] == job["result"]["total"] == 1
        assert sync.library.list()["items"][0]["rows"] == 1


def test_catalog_groups_versions_and_preserves_input_lineage(context, monkeypatch):
    _, _, sync, root = context
    job, content = prepared(context, monkeypatch)
    first = sync.publish(job["id"], job["token"], content)
    job2, content2 = prepared(
        context, monkeypatch, request(command_id="second"), lambda rows: [rows[0] | {"close": 3211}]
    )
    second = sync.publish(job2["id"], job2["token"], content2)
    assert first["id"] != second["id"]
    assert first["dataset_id"] == second["dataset_id"]
    catalog = sync.library.list(layer="STANDARD")
    assert catalog["total"] == 1
    assert catalog["items"][0]["version_count"] == 2
    assert "path" not in catalog["items"][0]["manifest"]
    assert sync.library.history(first["dataset_id"], limit=1)["total"] == 2
    assert sync.library.history(first["dataset_id"], offset=1)["items"][0]["id"] == first["id"]
    assert sync.library.preview(first["id"])["rows"][0]["close"] == "3210"
    assert sync.library.preview(second["id"])["rows"][0]["close"] == "3211"
    raw_id = second["manifest"]["inputs"][0]
    original = sync.library.preview(raw_id)
    assert original["version"]["manifest"]["layer"] == "RAW"
    assert original["rows"][0]["close"] == 3211
    assert original["snapshot"] is None
    assert sync.library.preview(second["id"])["snapshot"]["id"] == second["manifest"]["snapshot_id"]
    assert sync.library.list(domain="reference")["total"] == 0
    assert sync.library.list(source="local_file")["total"] == 0
    assert sync.library.list(search="RB2610", layer="RAW")["total"] == 1
    assert sync.library.list(search="%", layer="RAW")["total"] == 0
    assert sync.library.list(layer="DERIVED")["total"] == 0
    (root / "datasets" / job2["id"] / "evidence.json").write_bytes(b"corrupt")
    with pytest.raises(ProviderError, match="校验和"):
        sync.library.preview(raw_id)


def test_type_contract_is_independent_of_provider_mapping(context, monkeypatch):
    from asterion.data.types import builtin_types
    from asterion.data.types.public import DataType, TypeRegistry

    types = builtin_types()
    registry = TypeRegistry(tuple(types.all()))
    with pytest.raises(ValueError):
        registry.register(types.get("futures.daily"))
    with pytest.raises(ProviderError):
        registry.get("missing")
    custom = DataType(
        types.get("futures.contracts").manifest.model_copy(update={"id": "other.reference"}),
        lambda rows: None,
    )
    registry.register(custom)
    assert registry.get("other.reference") is custom
    # Even if the adapter supplies invalid standardized rows, the type blocks publication.
    _, _, sync, _ = context
    job, content = prepared(context, monkeypatch)
    original = Tushare.normalize

    def invalid(self, req, rows):
        return [r | {"vol": "1.5"} for r in original(self, req, rows)]

    monkeypatch.setattr(Tushare, "normalize", invalid)
    with pytest.raises(ProviderError, match="数据类型校验"):
        sync.publish(job["id"], job["token"], content)
    assert sync.library.list()["total"] == 0
