import copy
import json
import os
from concurrent.futures import ThreadPoolExecutor
from uuid import uuid4

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.task_repository import Conflict
from configuration_support import set_token
from credential_helpers import provider_secrets
from fastapi.testclient import TestClient
from sqlalchemy import select, text
from storage_support import data_store, domain_tasks, raw_engine, scheduler
from test_data_sync import MASTER, SECRET, evidence_content, request

from asterion.api.app import create_app
from asterion.data.providers.public import ProviderError
from asterion.data.providers.tushare import Tushare
from asterion.data.sync import DataSync, fixed_configuration
from asterion.platform.config import Settings
from asterion.platform.store import jobs, metadata


@pytest.fixture
def service(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/configuration.db")
    metadata.create_all(engine)
    sync = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path,
        provider_secrets(MASTER, tmp_path),
    )
    yield sync
    engine.dispose()


def fixed_values(service, payload):
    """The runnable configuration a task is fixed to, as collection and publication read it."""
    return fixed_configuration(service.credentials, payload, Tushare())


def calendar(command_id="calendar"):
    return request("calendar", command_id=command_id, end="2024-01-05")


def client_for(service, *, require_account=False):
    return TestClient(
        create_app(
            Settings(token=MASTER, data_root=service.root, require_account=require_account),
            raw_engine(service.engine),
        ),
        headers={"Authorization": f"Bearer {MASTER}"},
    )


def test_configuration_changes_only_affect_new_jobs_and_command_replay_is_stable(
    service, monkeypatch
):
    set_token(service, "tushare", SECRET)
    first = service.submit(calendar())
    set_token(service, "tushare", "a-replacement-provider-token")
    second = service.submit(calendar("different-command"))
    assert first["payload"]["configuration"]["ref"] != second["payload"]["configuration"]["ref"]
    seen = [fixed_values(service, job["payload"])["token"] for job in (first, second, first)]
    assert seen == [SECRET, "a-replacement-provider-token", SECRET]
    assert service.submit(calendar())["id"] == first["id"]
    with pytest.raises(Conflict):
        service.submit(calendar().model_copy(update={"end": calendar().start}))
    # Once accepted, replay remains available even if local configuration storage breaks.
    service.credentials = provider_secrets("a-different-runtime-key", service.root)
    assert service.submit(calendar())["id"] == first["id"]


@pytest.mark.parametrize(
    "fixed", [None, {}, {"ref": "not-a-valid-ref", "schema_version": 1, "revision": 0}]
)
def test_malformed_fixed_configuration_is_rejected(service, monkeypatch, fixed):
    set_token(service, "tushare", SECRET)
    payload = service.submission_payload(request("calendar"))
    payload["configuration"] = fixed
    with pytest.raises(ProviderError):
        fixed_values(service, payload)


def test_missing_corrupted_and_wrong_schema_snapshots_fail_safely(service):
    with pytest.raises(ProviderError):
        service.submit(request("calendar"))
    set_token(service, "tushare", SECRET)
    payload = service.submission_payload(request("calendar"))
    fixed = payload["configuration"]
    path = service.root / ".credentials" / "configurations" / f"{fixed['ref']}.enc"
    original = path.read_bytes()
    path.unlink()
    with pytest.raises(ProviderError) as missing:
        fixed_values(service, payload)
    assert SECRET not in str(missing.value)
    path.write_bytes(b"invalid ciphertext")
    with pytest.raises(ProviderError):
        fixed_values(service, payload)
    path.write_bytes(original)
    wrong_schema = copy.deepcopy(payload)
    wrong_schema["configuration"]["schema_version"] += 1
    with pytest.raises(ProviderError):
        fixed_values(service, wrong_schema)
    wrong_revision = copy.deepcopy(payload)
    wrong_revision["configuration"]["revision"] += 1
    with pytest.raises(ProviderError):
        fixed_values(service, wrong_revision)


@pytest.mark.parametrize("replacement", [SECRET, "replacement-token"])
def test_corrupted_secret_configuration_requires_explicit_replacement_and_keeps_old_job_fixed(
    service, replacement
):
    service.sources.apply("tushare", 0, secrets={"token": SECRET})
    original = service.submission_payload(request("calendar"))
    ref = original["configuration"]["ref"]
    path = service.root / ".credentials" / "configurations" / f"{ref}.enc"
    path.write_bytes(b"damaged configuration snapshot")
    state = service.sources.state("tushare")
    assert state["revision"] == 1 and not state["configured"] and state["error"]
    assert state["secret_fields"] == []
    assert SECRET not in json.dumps(state)
    with pytest.raises(ProviderError):
        service.sources.apply("tushare", 1)
    restored = service.sources.apply("tushare", 1, secrets={"token": replacement})
    assert restored["revision"] == 2 and restored["configured"] and not restored["error"]
    new_payload = service.submission_payload(request("calendar", command_id="restored"))
    assert new_payload["configuration"]["ref"] != ref
    assert path.read_bytes() == b"damaged configuration snapshot"
    with pytest.raises(ProviderError):
        fixed_values(service, original)


def test_missing_configuration_is_rejected_before_fetch(service, monkeypatch):
    set_token(service, "tushare", SECRET)
    payload = service.submission_payload(request("calendar"))
    payload.pop("configuration")
    with pytest.raises(ProviderError, match="固定配置"):
        fixed_values(service, payload)
    assert "configuration" not in payload


def test_resume_reuses_only_evidence_from_identical_frozen_configuration(service, monkeypatch):
    set_token(service, "tushare", SECRET)
    original = service.submit(request("calendar"))
    claimed = scheduler(service.engine).claim("configuration-test")
    evidence_content(
        claimed,
        record=lambda index, evidence: service.evidence.record(
            claimed["id"], claimed["token"], index, evidence
        ),
    )
    scheduler(service.engine).fail(claimed["id"], claimed["token"], "interrupted after observation")
    service.retry(original["id"], "same-config", resume=True)
    same = scheduler(service.engine).claim("same-config-worker")
    assert set(service.evidence.resume(same["id"], same["token"])) == {0}
    scheduler(service.engine).cancel(same["id"])
    set_token(service, "tushare", "a-changed-provider-token")
    service.retry(original["id"], "changed-config", resume=True)
    changed = scheduler(service.engine).claim("changed-config-worker")
    assert service.evidence.resume(changed["id"], changed["token"]) == {}


def test_claim_does_not_rewrite_unsupported_queued_payload(service):
    set_token(service, "tushare", SECRET)
    payload = service.submission_payload(request("calendar"))
    payload.pop("configuration")
    queued = service.tasks.submit("unsupported-command", "data.sync", payload)
    claimed = scheduler(service.engine).claim("worker")
    assert claimed["id"] == queued["id"] and claimed["payload"] == payload
    with pytest.raises(ProviderError, match="固定配置"):
        fixed_values(service, claimed["payload"])
    scheduler(service.engine).fail(claimed["id"], claimed["token"], "固定配置缺失")
    assert scheduler(service.engine).claim("worker") is None


@pytest.mark.skipif(
    not os.getenv("ASTERION_TEST_DATABASE_URL"), reason="PostgreSQL test URL not set"
)
def test_postgres_concurrent_submission_is_idempotent(tmp_path, monkeypatch):
    url = os.environ["ASTERION_TEST_DATABASE_URL"]
    admin = create_engine(url)
    schema = "configuration_" + uuid4().hex
    with admin.begin() as conn:
        conn.execute(text(f"CREATE SCHEMA {schema}"))
    engine = create_engine(url, connect_args={"options": f"-csearch_path={schema}"})
    try:
        metadata.create_all(engine)
        service = DataSync(
            data_store(engine),
            domain_tasks(data_store(engine), "data"),
            tmp_path,
            provider_secrets(MASTER, tmp_path),
        )
        set_token(service, "tushare", SECRET)
        with ThreadPoolExecutor(max_workers=6) as pool:
            tasks = list(pool.map(lambda _: service.submit(calendar()), range(6)))
        assert len({task["id"] for task in tasks}) == 1
        with engine.connect() as conn:
            assert len(conn.execute(select(jobs)).all()) == 1
    finally:
        engine.dispose()
        with admin.begin() as conn:
            conn.execute(text(f"DROP SCHEMA {schema} CASCADE"))
        admin.dispose()


def test_publication_rejects_a_task_without_fixed_configuration(service, monkeypatch):
    set_token(service, "tushare", SECRET)
    queued = service.submit(calendar())
    claimed = scheduler(service.engine).claim("worker")
    content = evidence_content(claimed)
    unsupported = dict(claimed["payload"])
    unsupported.pop("configuration")
    with raw_engine(service.engine).begin() as conn:
        conn.execute(jobs.update().where(jobs.c.id == queued["id"]).values(payload=unsupported))
    with pytest.raises(ProviderError, match="固定配置"):
        service.publish(queued["id"], claimed["token"], content)
    assert service.library.list()["total"] == 0
