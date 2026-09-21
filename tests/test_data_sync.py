import json
import stat
import time
from datetime import UTC, datetime

import httpx
import pytest
from configuration_support import set_token
from credential_helpers import provider_secrets
from fastapi.testclient import TestClient
from sqlalchemy import create_engine, select
from storage_support import data_store, domain_tasks, raw_engine, scheduler
from sync_identity_support import submit_source

from asterion.api.app import create_app
from asterion.data.ingestion import Observation
from asterion.data.providers import ProviderRegistry
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.data.public import read_bars
from asterion.data.sync import Credentials, DataSync, collect
from asterion.platform.config import Settings
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs, metadata
from asterion.platform.tasks.service import Conflict

MASTER = "test-runtime-token-at-least-24-characters"
SECRET = "synthetic-provider-token-never-log"


@pytest.fixture
def context(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/test.db")
    metadata.create_all(engine)
    tasks = scheduler(engine)
    sync = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path,
        provider_secrets(MASTER),
    )
    set_token(sync, "tushare", SECRET)
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
        "d_month": "202610",
        "last_ddate": "20261020",
        "list_date": "20230101",
        "delist_date": "20261015",
        "per_unit": 10,
        "trade_unit": "吨",
        "quote_unit": "元/吨",
    }
    return {k: values.get(k) for k in partition.fields}


def prepared(context, monkeypatch, req=None, transform=None):
    _, tasks, sync, root = context
    accepted = req or request()
    if accepted.dataset == "settlement":
        from asterion.data.sync_admission import SyncSubmission, admit

        basis_job, basis_content = prepared(
            context,
            monkeypatch,
            request("contracts", command_id=accepted.command_id + "-contracts"),
        )
        basis = sync.publish(basis_job["id"], basis_job["token"], basis_content)
        admit(
            sync,
            SyncSubmission.model_validate(
                accepted.model_dump() | {"contracts_version_id": basis["id"]}
            ),
        )
    else:
        submit_source(sync, accepted)
    job = tasks.claim("worker")

    def fetch(self, partition, credential):
        assert credential == {"token": SECRET}
        rows = [raw(partition)]
        return transform(rows) if transform else rows

    monkeypatch.setattr(Tushare, "fetch", fetch)
    content = collect(
        job["payload"],
        root,
        provider_secrets(MASTER),
        lambda done, total: sync.progress(job["id"], job["token"], done, total),
    )
    return job, content


def test_registry_and_bounded_plan():
    registry = ProviderRegistry((Tushare(),))
    assert registry.get("tushare").manifest.api_version == 2
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
    fixed = sync.configuration.freeze("tushare")
    path = sync.credentials.root / "configurations" / f"{fixed['ref']}.enc"
    assert SECRET.encode() not in path.read_bytes()
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700
    assert sync.configuration.current("tushare")[1] == {"token": SECRET}
    with pytest.raises(ProviderError, match="固定配置"):
        Credentials(root, provider_secrets("different-master-key")).read_configuration(
            fixed["ref"], "tushare", fixed["schema_version"], fixed["revision"]
        )
    assert submit_source(sync, request())["id"] == submit_source(sync, request())["id"]
    with engine.connect() as conn:
        assert SECRET not in str(conn.execute(select(jobs)).all())
    assert SECRET not in str(sync.providers())
    set_token(sync, "tushare", "")
    assert not sync.providers()[0]["configured"]
    with pytest.raises(ProviderError, match="Token"):
        submit_source(sync, request(command_id="missing"))
    with pytest.raises(ValueError):
        sync.credentials.freeze_configuration("../escape", 1, {"token": "invalid"})


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
    assert sync.library.list(type_id="futures.daily")["items"] == []
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
    assert sync.library.list(type_id="futures.daily")["items"] == []
    tasks.cancel(job["id"])
    with pytest.raises(Conflict):
        sync.publish(job["id"], replacement["token"], content)


def test_calendar_gap_and_corrupt_parquet(context, monkeypatch):
    _, _, sync, root = context
    job, content = prepared(context, monkeypatch, request("calendar", end="2024-01-03"))
    with pytest.raises(ProviderError, match="缺少"):
        sync.publish(job["id"], job["token"], content)
    scheduler(sync.engine).cancel(job["id"])
    job, content = prepared(context, monkeypatch, request("calendar", command_id="complete"))
    result = sync.publish(job["id"], job["token"], content)
    (root / "artifacts" / f"{result['manifest']['partitions'][0]['checksum']}.parquet").write_bytes(
        b"corrupt"
    )
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
    assert Tushare().fetch(partition, {"token": SECRET})[0]["close"] == 3210
    mode[0] = "deny"
    with pytest.raises(ProviderError) as error:
        Tushare().fetch(partition, {"token": SECRET})
    assert SECRET not in str(error.value)
    mode[0] = "retry"
    seen.clear()
    assert Tushare().fetch(partition, {"token": SECRET})
    assert len(seen) == 3


def test_api_requires_account_for_provider_configuration(context):
    engine, _, _, root = context
    client = TestClient(
        create_app(Settings(token=MASTER, data_root=root, require_account=True), raw_engine(engine))
    )
    client.headers["Authorization"] = f"Bearer {MASTER}"
    for url, method, body in [
        ("/data/providers", "get", None),
        ("/data/catalog", "get", None),
        ("/data/types", "get", None),
        ("/data/jobs/unknown/observations", "get", None),
        ("/data/jobs/unknown/observations/1/0", "get", None),
        ("/data/versions/unknown", "get", None),
        ("/data/catalog/unknown/versions", "get", None),
        (
            "/data/providers/tushare/configuration",
            "post",
            {"expected_revision": 0, "secrets": {"token": SECRET}},
        ),
        ("/data/sync", "post", request().model_dump(mode="json")),
    ]:
        response = client.request(method, "/api/v1" + url, json=body)
        assert response.status_code == 401


def test_api_sync_roundtrip(context, monkeypatch):
    engine, _, _, root = context
    client = TestClient(create_app(Settings(token=MASTER, data_root=root), raw_engine(engine)))
    client.headers["Authorization"] = f"Bearer {MASTER}"
    assert client.get("/api/v1/data/providers").json()[0]["configured"]
    _, _, service, _ = context
    reference_job, reference_content = prepared(
        context, monkeypatch, request("contracts", command_id="reference")
    )
    reference_version = service.publish(
        reference_job["id"], reference_job["token"], reference_content
    )
    submitted = client.post(
        "/api/v1/data/sync",
        json=request().model_dump(mode="json") | {"contracts_version_id": reference_version["id"]},
    )
    assert submitted.status_code == 202
    job = client.post("/api/v1/jobs/claim", json={"worker_id": "test"}).json()
    monkeypatch.setattr(Tushare, "fetch", lambda self, part, credential: [raw(part)])
    content = collect(job["payload"], root, provider_secrets(MASTER))
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
    sync.credentials = Credentials(root, provider_secrets("rotated-runtime-key-long-enough"))
    assert not sync.providers()[0]["configured"]
    assert sync.providers()[0]["credential_error"]
    set_token(sync, "tushare", SECRET)
    assert sync.providers()[0]["configured"]


@pytest.mark.parametrize("invalid", [False, True])
def test_worker_dispatch_uses_sync_protocol_and_reports_validation(context, monkeypatch, invalid):
    from concurrent.futures import ThreadPoolExecutor

    from asterion.runtime.worker import run_once

    engine, tasks, sync, root = context
    settings = Settings(token=MASTER, data_root=root, api_url="http://worker.test")
    server = TestClient(create_app(settings, raw_engine(engine)))
    submit_source(sync, request())

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
    evidence = sync.evidence.preview(job["id"], 1, 0)
    assert evidence["rows"][0]["high"] == (1 if invalid else 3220)
    assert sync.evidence.list(job["id"])["total"] == 1
    if invalid:
        assert "边界" in job["error"]
        assert not sync.library.list(type_id="futures.daily")["items"]
    else:
        assert job["result"]["completed"] == job["result"]["total"] == 1
        assert sync.library.list(type_id="futures.daily")["items"][0]["rows"] == 1


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
    catalog = sync.library.list(type_id="futures.daily", layer="STANDARD")
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
    assert sync.library.list(domain="reference")["total"] == 2
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
    assert sync.library.list(type_id="futures.daily")["total"] == 0


def test_partial_fetch_failure_retains_prior_and_safe_failure(context, monkeypatch):
    _, tasks, sync, root = context
    submit_source(sync, request(end="2024-02-02"))
    job = tasks.claim("worker")

    def fetch(self, part, credential):
        if part.params["start_date"] == "20240202":
            raise ProviderError("PERMISSION_DENIED：" + SECRET)
        return [raw(part)]

    monkeypatch.setattr(Tushare, "fetch", fetch)
    with pytest.raises(ProviderError, match="PERMISSION_DENIED") as error:
        collect(
            job["payload"],
            root,
            provider_secrets(MASTER),
            checkpoint=lambda index, value: sync.evidence.record(
                job["id"], job["token"], index, Observation.model_validate(value)
            ),
        )
    assert SECRET not in str(error.value)
    tasks.fail(job["id"], job["token"], str(error.value))
    saved = sync.evidence.list(job["id"])
    assert saved["total"] == 2
    assert [item["manifest"]["status"] for item in saved["items"]] == [
        "RECEIVED",
        "PERMISSION_DENIED",
    ]
    assert sync.evidence.list(job["id"], offset=1, limit=1)["items"] == saved["items"][1:]
    assert sync.evidence.preview(job["id"], 1, 0)["rows"][0]["close"] == 3210
    assert sync.evidence.preview(job["id"], 1, 1)["rows"] == []
    assert all(SECRET not in p.read_text() for p in (root / "sources").rglob("*.json"))
    assert sync.library.list(type_id="futures.daily")["total"] == 0


def test_observation_immutable_attempts_cancel_and_corruption(context, monkeypatch):
    engine, tasks, sync, root = context
    job, content = prepared(context, monkeypatch)
    value = Observation.model_validate(json.loads(content)[0])
    saved = sync.evidence.record(job["id"], job["token"], 0, value)
    assert sync.evidence.record(job["id"], job["token"], 0, value) == saved
    with pytest.raises(Conflict, match="changed content"):
        sync.evidence.record(job["id"], job["token"], 0, value.model_copy(update={"rows": []}))
    with engine.begin() as conn:
        conn.execute(jobs.update().values(lease_until=time.time() - 1))
    replacement = tasks.claim("replacement")
    with pytest.raises(Conflict):
        sync.evidence.record(job["id"], job["token"], 0, value)
    sync.evidence.record(job["id"], replacement["token"], 0, value.model_copy(update={"rows": []}))
    assert sync.evidence.preview(job["id"], 2, 0)["manifest"]["status"] == "EMPTY_UNCONFIRMED"
    tasks.cancel(job["id"])
    with pytest.raises(Conflict):
        sync.evidence.record(job["id"], replacement["token"], 0, value)
    assert sync.evidence.list(job["id"])["total"] == 2
    path = root / saved["uri"].removeprefix("asterion://local/")
    path.write_bytes(b"corrupt")
    with pytest.raises(ProviderError, match="校验和"):
        sync.evidence.preview(job["id"], 1, 0)


def test_observation_api_validation_and_io_rollback(context, monkeypatch):
    engine, _, sync, root = context
    job, content = prepared(context, monkeypatch)
    client = TestClient(create_app(Settings(token=MASTER, data_root=root), raw_engine(engine)))
    client.headers["Authorization"] = f"Bearer {MASTER}"
    endpoint = f"/api/v1/jobs/{job['id']}/observations/0"
    evidence = json.loads(content)[0]
    headers = {"X-Lease-Token": job["token"]}
    assert (
        client.post(endpoint, json=evidence, headers={"X-Lease-Token": "stale"}).status_code == 409
    )
    assert (
        client.post(endpoint, json=evidence | {"token": SECRET}, headers=headers).status_code == 422
    )
    assert client.post(endpoint, content=b"x" * 8_000_001, headers=headers).status_code == 413
    assert client.post(endpoint, json=evidence, headers=headers).status_code == 200
    assert client.get(f"/api/v1/data/jobs/{job['id']}/observations/1/0").json()["total"] == 1
    assert client.get("/api/v1/data/jobs/missing/observations").status_code == 404

    submit_source(sync, request(command_id="io-failure"))
    next_job = scheduler(sync.engine).claim("next")

    def fail_write(*args):
        raise OSError("disk full")

    monkeypatch.setattr("asterion.data.ingestion.atomic_write", fail_write)
    value = Observation.model_validate(evidence).model_copy(
        update={"observed_at": datetime.now(UTC)}
    )
    with pytest.raises(OSError):
        sync.evidence.record(next_job["id"], next_job["token"], 0, value)
    assert sync.evidence.list(next_job["id"])["total"] == 0


def test_publication_must_match_retained_observation(context, monkeypatch):
    _, _, sync, _ = context
    job, content = prepared(context, monkeypatch)
    envelope = json.loads(content)
    sync.evidence.record(job["id"], job["token"], 0, Observation.model_validate(envelope[0]))
    envelope[0]["rows"][0]["close"] = 3211
    with pytest.raises(Conflict, match="changed retained"):
        sync.publish(job["id"], job["token"], canonical(envelope))
    assert sync.library.list(type_id="futures.daily")["total"] == 0
    sync.publish(job["id"], job["token"], content)


def test_resume_reuses_valid_partition_preserving_time_and_publishes(context, monkeypatch):
    _, tasks, sync, root = context
    submit_source(sync, request(end="2024-02-02"))
    first = tasks.claim("first")

    def checkpoint(job):
        return lambda index, value: sync.evidence.record(
            job["id"], job["token"], index, Observation.model_validate(value)
        )

    seen = []
    fail = [True]

    def fetch(self, part, credential):
        seen.append(part.params["start_date"])
        if fail[0] and len(seen) == 2:
            raise ProviderError("RATE_LIMITED：synthetic")
        return [raw(part) | {"trade_date": part.params["start_date"]}]

    monkeypatch.setattr(Tushare, "fetch", fetch)
    with pytest.raises(ProviderError):
        collect(first["payload"], root, provider_secrets(MASTER), checkpoint=checkpoint(first))
    original = sync.evidence.preview(first["id"], 1, 0)
    tasks.fail(first["id"], first["token"], "synthetic failure")
    submitted = sync.retry(first["id"], "resume", resume=True)
    assert sync.retry(first["id"], "resume", resume=True)["id"] == submitted["id"]
    with pytest.raises(Conflict):
        sync.retry(first["id"], "resume", resume=False)
    job = tasks.claim("resumer")
    reused = sync.evidence.resume(job["id"], job["token"])
    assert list(reused) == [0]
    assert sync.evidence.resume(job["id"], job["token"]) == reused
    client = TestClient(create_app(Settings(token=MASTER, data_root=root), raw_engine(sync.engine)))
    client.headers["Authorization"] = f"Bearer {MASTER}"
    endpoint = f"/api/v1/jobs/{job['id']}/resume"
    assert client.post(endpoint, headers={"X-Lease-Token": "stale"}).status_code == 409
    response = client.post(endpoint, headers={"X-Lease-Token": job["token"]})
    assert response.status_code == 200
    assert response.json() == {str(k): v for k, v in reused.items()}
    assert reused[0]["observed_at"] == original["manifest"]["observed_at"].replace("+00:00", "Z")
    fail[0] = False
    seen.clear()
    content = collect(
        job["payload"],
        root,
        provider_secrets(MASTER),
        checkpoint=checkpoint(job),
        reused={str(k): v for k, v in reused.items()},
    )
    assert seen == ["20240202"]
    version = sync.publish(job["id"], job["token"], content)
    assert sync.library.preview(version["id"])["total"] == 2
    saved = sync.evidence.preview(job["id"], 1, 0)
    assert saved["manifest"]["reused_from"]["job_id"] == first["id"]
    assert saved["manifest"]["observed_at"] == original["manifest"]["observed_at"]


@pytest.mark.parametrize(
    "invalid", ["empty", "price", "corrupt", "missing", "version", "calendar_gap"]
)
def test_resume_refetches_unusable_partitions(context, monkeypatch, invalid):
    engine, tasks, sync, root = context
    req = request("calendar", end="2024-01-03") if invalid == "calendar_gap" else request()
    job, content = prepared(context, monkeypatch, req)
    value = Observation.model_validate(json.loads(content)[0])
    if invalid == "empty":
        value.rows = []
    if invalid == "price":
        value.rows[0]["high"] = 1
    saved = sync.evidence.record(job["id"], job["token"], 0, value)
    path = root / saved["uri"].removeprefix("asterion://local/")
    if invalid == "corrupt":
        path.write_bytes(b"corrupt")
    if invalid == "missing":
        path.unlink()
    if invalid == "version":
        with engine.begin() as conn:
            conn.execute(
                jobs.update()
                .where(jobs.c.id == job["id"])
                .values(payload=job["payload"] | {"plugin_version": "mismatched-version"})
            )
    tasks.fail(job["id"], job["token"], "synthetic")
    sync.retry(job["id"], "resume", resume=True)
    retry = tasks.claim("resumer")
    assert sync.evidence.resume(retry["id"], retry["token"]) == {}
    assert sync.evidence.list(retry["id"])["total"] == 0


def test_takeover_reuses_prior_attempt_but_rejects_forged_provenance(context, monkeypatch):
    from asterion.data.ingestion import EvidenceSource

    engine, tasks, sync, _ = context
    job, content = prepared(context, monkeypatch)
    value = Observation.model_validate(json.loads(content)[0])
    saved = sync.evidence.record(job["id"], job["token"], 0, value)
    with engine.begin() as conn:
        conn.execute(jobs.update().values(lease_until=time.time() - 1))
    replacement = tasks.claim("replacement")
    with pytest.raises(Conflict):
        sync.evidence.resume(job["id"], job["token"])
    assert 0 in sync.evidence.resume(job["id"], replacement["token"])
    tasks.cancel(job["id"])
    sync.retry(job["id"], "fresh", resume=False)
    fresh = tasks.claim("fresh")
    assert sync.evidence.resume(fresh["id"], fresh["token"]) == {}
    forged = value.model_copy(
        update={
            "reused_from": EvidenceSource(
                job_id=job["id"], attempt=1, partition_index=0, checksum=saved["checksum"]
            )
        }
    )
    with pytest.raises(Conflict):
        sync.evidence.record(fresh["id"], fresh["token"], 0, forged)
    with pytest.raises(Conflict):
        sync.publish(fresh["id"], fresh["token"], canonical([forged.model_dump(mode="json")]))


def test_publication_observer_sees_committing_version_and_rolls_back(context, monkeypatch):
    from asterion.data.public import snapshot_backup_access
    from asterion.platform.files import read_files

    _, tasks, sync, root = context
    job, content = prepared(context, monkeypatch)
    observed = []

    def observer(transaction, identifier):
        row = transaction.execute(select(jobs).where(jobs.c.id == identifier)).mappings().one()
        assert row["state"] == "SUCCEEDED"
        evidence = snapshot_backup_access(transaction, read_files(root))
        assert evidence.read(row["result"]["version_id"], limit=10)["total"] >= 1
        observed.append(identifier)
        raise ValueError("offline observer rollback")

    sync.published = observer
    with pytest.raises(ValueError, match="observer rollback"):
        sync.publish(job["id"], job["token"], content)
    assert tasks.get(job["id"])["state"] == "RUNNING"
    sync.published = lambda transaction, identifier: observed.append(identifier)
    sync.publish(job["id"], job["token"], content)
    sync.publish(job["id"], job["token"], content)
    assert observed == [job["id"], job["id"]]
