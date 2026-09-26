import hashlib
import json
import stat
import time
from datetime import UTC, datetime

import pytest
from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.database import create_engine
from asterion_bindings.task_repository import Conflict
from configuration_support import saved_values, set_token
from credential_helpers import provider_secrets
from entry_support import running_entry
from fastapi.testclient import TestClient
from import_support import chart_bars
from sqlalchemy import select
from storage_support import data_store, domain_tasks, raw_engine, scheduler
from sync_identity_support import submit_source

from asterion.api.app import create_app
from asterion.data.events import VERSION_PUBLISHED
from asterion.data.providers import ProviderRegistry
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.data.sync import DataSync
from asterion.platform.communication.events import EventJournal
from asterion.platform.config import Settings
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs, metadata

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
        provider_secrets(MASTER, tmp_path),
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


def evidence_content(job, rows=lambda partition: [raw(partition)], record=None) -> bytes:
    """The evidence a collection of `job` would produce from `rows` per partition."""
    request = SyncRequest.model_validate(job["payload"]["request"])
    evidence = []
    for index, partition in enumerate(Tushare().plan(request)):
        value = {
            "partition": partition.model_dump(),
            "rows": [{f: row.get(f) for f in partition.fields} for row in rows(partition)],
            "observed_at": datetime.now(UTC).isoformat(),
        }
        if record:
            record(index, value)
        evidence.append(value)
    return canonical(evidence)


def prepared(context, monkeypatch, req=None, transform=None):
    _, tasks, sync, root = context
    accepted = req or request()
    if accepted.dataset == "settlement":
        basis_job, basis_content = prepared(
            context,
            monkeypatch,
            request("contracts", command_id=accepted.command_id + "-contracts"),
        )
        basis = sync.publish(basis_job["id"], basis_job["token"], basis_content)
        sync.admit(accepted.model_dump() | {"contracts_version_id": basis["id"]})
    else:
        submit_source(sync, accepted)
    job = tasks.claim("worker")

    def rows(partition):
        values = [raw(partition)]
        return transform(values) if transform else values

    return job, evidence_content(job, rows)


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
    fixed = submit_source(sync, request(command_id="fixed"))["payload"]["configuration"]
    path = root / ".credentials" / "configurations" / f"{fixed['ref']}.enc"
    assert SECRET.encode() not in path.read_bytes()
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700
    assert saved_values(sync, "tushare") == {"token": SECRET}
    spec = Tushare.manifest.configuration.model_dump(mode="json")
    with pytest.raises(ValueError, match="固定配置"):
        provider_secrets("different-master-key", root).resolve(fixed, "tushare", spec)
    assert submit_source(sync, request())["id"] == submit_source(sync, request())["id"]
    with engine.connect() as conn:
        assert SECRET not in str(conn.execute(select(jobs)).all())
    assert SECRET not in str(sync.providers())
    set_token(sync, "tushare", "")
    assert not sync.providers()[0]["configured"]
    with pytest.raises(ProviderError, match="Token"):
        submit_source(sync, request(command_id="missing"))


@pytest.mark.parametrize("dataset", ["daily", "contracts", "calendar"])
def test_publish_retains_evidence_and_daily_chart(context, monkeypatch, dataset):
    _, tasks, sync, root = context
    job, content = prepared(context, monkeypatch, request(dataset))
    published = sync.publish(job["id"], job["token"], content)
    assert sync.publish(job["id"], job["token"], content) == published
    assert tasks.list()[0]["state"] == "SUCCEEDED"
    assert sync.library.preview(published["id"])["total"] == 1
    evidence = f"datasets/{job['id']}/evidence/{hashlib.sha256(content).hexdigest()}.json"
    assert (root / evidence).read_bytes() == content
    assert sync.library.preview(published["id"], 100)["rows"] == []
    with pytest.raises(Conflict):
        sync.publish(job["id"], job["token"], content + b" ")
    if dataset == "daily":
        bars = chart_bars(sync.engine, root, published["manifest"]["snapshot_id"])
        assert bars[0]["trading_day"] == "2024-01-02"
        assert bars[0]["event_time"] == "2024-01-02T00:00:00Z"
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

    def fail_write(*args):
        raise OSError("disk full")

    monkeypatch.setattr(sync.library.artifacts, "put_addressed", fail_write)
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


def test_api_sync_roundtrip(context, monkeypatch):
    engine, _, sync, root = context
    client = TestClient(create_app(Settings(token=MASTER, data_root=root), raw_engine(engine)))
    client.headers["Authorization"] = f"Bearer {MASTER}"
    assert sync.providers()[0]["configured"]
    _, _, service, _ = context
    reference_job, reference_content = prepared(
        context, monkeypatch, request("contracts", command_id="reference")
    )
    reference_version = service.publish(
        reference_job["id"], reference_job["token"], reference_content
    )
    sync.admit(
        request().model_dump(mode="json") | {"contracts_version_id": reference_version["id"]}
    )
    job = scheduler(engine).claim("test")
    content = evidence_content(job)
    published = client.post(
        f"/api/v1/jobs/{job['id']}/publish-data",
        content=content,
        headers={"X-Lease-Token": job["token"]},
    )
    assert published.status_code == 200, published.text
    event_page = EventJournal(engine, (VERSION_PUBLISHED,)).read(VERSION_PUBLISHED.id)
    assert event_page["items"][-1]["payload"]["version_id"] == published.json()["id"]
    assert (
        event_page["items"][-1]["payload"]["checksum"] == published.json()["manifest"]["checksum"]
    )
    assert SECRET not in json.dumps(event_page)

    assert sync.library.list()["items"][0]["rows"] == 1
    assert sync.library.preview(published.json()["id"])["total"] == 1
    with engine.connect() as conn:
        stored = conn.exec_driver_sql("SELECT manifest FROM snapshots").scalar_one()
    assert (json.loads(stored) if isinstance(stored, str) else stored)["frequency"] == "1d"
    assert SECRET not in json.dumps(scheduler(engine).list())


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
    sync.credentials = provider_secrets("rotated-runtime-key-long-enough", root)
    sync.sources.credentials = sync.credentials
    assert not sync.providers()[0]["configured"]
    assert sync.providers()[0]["credential_error"]
    set_token(sync, "tushare", SECRET)
    assert sync.providers()[0]["configured"]


def test_queued_sync_is_collected_by_the_entry_not_the_worker(context):
    from asterion.runtime.worker import run_once

    engine, tasks, sync, root = context
    settings = Settings(token=MASTER, data_root=root, database_url=str(engine.url))
    app = create_app(settings, raw_engine(engine))
    submit_source(sync, request())
    with running_entry(app, settings) as url:
        # The internal worker never claims sync tasks; the entry does, and in
        # tests its source is unreachable, so it retains a failure observation.
        assert not run_once(settings.model_copy(update={"api_url": url}), "worker-integration")
        deadline = time.monotonic() + 30
        while tasks.list()[0]["state"] != "FAILED":
            assert time.monotonic() < deadline, tasks.list()[0]
            time.sleep(0.2)
    job = tasks.list()[0]
    assert job["error"].startswith("FETCH_FAILED：")
    saved = sync.evidence.list(job["id"])
    assert [item["manifest"]["status"] for item in saved["items"]] == ["FETCH_FAILED"]


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
    (evidence,) = (root / "datasets" / job2["id"] / "evidence").glob("*.json")
    evidence.write_bytes(b"corrupt")
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


def test_observation_immutable_attempts_cancel_and_corruption(context, monkeypatch):
    engine, tasks, sync, root = context
    job, content = prepared(context, monkeypatch)
    value = json.loads(content)[0]
    saved = sync.evidence.record(job["id"], job["token"], 0, value)
    assert sync.evidence.record(job["id"], job["token"], 0, value) == saved
    with pytest.raises(Conflict, match="changed content"):
        sync.evidence.record(job["id"], job["token"], 0, value | {"rows": []})
    with engine.begin() as conn:
        conn.execute(jobs.update().values(lease_until=time.time() - 1))
    replacement = tasks.claim("replacement")
    with pytest.raises(Conflict):
        sync.evidence.record(job["id"], job["token"], 0, value)
    sync.evidence.record(job["id"], replacement["token"], 0, value | {"rows": []})
    assert sync.evidence.preview(job["id"], 2, 0)["manifest"]["status"] == "EMPTY_UNCONFIRMED"
    tasks.cancel(job["id"])
    with pytest.raises(Conflict):
        sync.evidence.record(job["id"], replacement["token"], 0, value)
    assert sync.evidence.list(job["id"])["total"] == 2
    path = root / saved["uri"].removeprefix("asterion://local/")
    path.write_bytes(b"corrupt")
    with pytest.raises(ProviderError, match="校验和"):
        sync.evidence.preview(job["id"], 1, 0)


def test_observation_io_failure_records_nothing(context, monkeypatch):
    _, _, sync, root = context
    job, content = prepared(context, monkeypatch)
    evidence = json.loads(content)[0]
    sync.evidence.record(job["id"], job["token"], 0, evidence)
    assert sync.evidence.preview(job["id"], 1, 0)["total"] == 1
    with pytest.raises(KeyError):
        sync.evidence.list("missing")

    submit_source(sync, request(command_id="io-failure"))
    next_job = scheduler(sync.engine).claim("next")

    # Evidence that cannot be written is refused and nothing is recorded.
    blocked = root / "sources" / next_job["id"]
    blocked.parent.mkdir(exist_ok=True)
    blocked.write_bytes(b"not a directory")
    value = evidence | {"observed_at": datetime.now(UTC).isoformat()}
    with pytest.raises(ProviderError, match="无法保存"):
        sync.evidence.record(next_job["id"], next_job["token"], 0, value)
    assert sync.evidence.list(next_job["id"])["total"] == 0


def test_publication_must_match_retained_observation(context, monkeypatch):
    _, _, sync, _ = context
    job, content = prepared(context, monkeypatch)
    envelope = json.loads(content)
    sync.evidence.record(job["id"], job["token"], 0, envelope[0])
    envelope[0]["rows"][0]["close"] = 3211
    with pytest.raises(Conflict, match="changed retained"):
        sync.publish(job["id"], job["token"], canonical(envelope))
    assert sync.library.list(type_id="futures.daily")["total"] == 0
    sync.publish(job["id"], job["token"], content)


def test_resume_reuses_valid_partition_preserving_time_and_publishes(context):
    _, tasks, sync, _ = context
    submit_source(sync, request(end="2024-02-02"))
    first = tasks.claim("first")

    def rows(partition):
        return [raw(partition) | {"trade_date": partition.params["start_date"]}]

    # The first attempt retained only its first partition before failing.
    envelope = json.loads(evidence_content(first, rows))
    sync.evidence.record(first["id"], first["token"], 0, envelope[0])
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
    with pytest.raises(Conflict):
        sync.evidence.resume(job["id"], "stale")
    assert reused[0]["observed_at"] == original["manifest"]["observed_at"].replace("+00:00", "Z")
    fresh = json.loads(evidence_content(job, rows))[1]
    sync.evidence.record(job["id"], job["token"], 1, fresh)
    version = sync.publish(job["id"], job["token"], canonical([reused[0], fresh]))
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
    value = json.loads(content)[0]
    if invalid == "empty":
        value["rows"] = []
    if invalid == "price":
        value["rows"][0]["high"] = 1
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

    engine, tasks, sync, _ = context
    job, content = prepared(context, monkeypatch)
    value = json.loads(content)[0]
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
    forged = value | {
        "reused_from": {
            "job_id": job["id"],
            "attempt": 1,
            "partition_index": 0,
            "checksum": saved["checksum"],
        }
    }
    with pytest.raises(Conflict):
        sync.evidence.record(fresh["id"], fresh["token"], 0, forged)
    with pytest.raises(Conflict):
        sync.publish(fresh["id"], fresh["token"], canonical([forged]))


def test_publication_observer_sees_committing_version_and_rolls_back(context, monkeypatch):

    from asterion.data.public import snapshot_backup_access

    _, tasks, sync, root = context
    job, content = prepared(context, monkeypatch)
    observed = []

    def observer(transaction, identifier):
        row = transaction.execute(select(jobs).where(jobs.c.id == identifier)).mappings().one()
        assert row["state"] == "SUCCEEDED"
        evidence = snapshot_backup_access(transaction, ArtifactStore(root, read_only=True))
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
