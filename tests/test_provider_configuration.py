import copy
import json
import os
import stat
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from datetime import datetime
from threading import Barrier
from uuid import uuid4

import pytest
from configuration_support import set_token
from credential_helpers import provider_secrets
from fastapi.testclient import TestClient
from pydantic import SecretStr, ValidationError
from sqlalchemy import create_engine, select, text
from storage_support import data_store, domain_tasks, raw_engine, scheduler
from synthetic_provider import Synthetic
from test_data_sync import MASTER, SECRET, raw, request

from asterion.api.app import create_app
from asterion.data.configuration import (
    ConfigurationUpdate,
    ProviderConfigurations,
    configurations,
    validate,
)
from asterion.data.ingestion import Observation
from asterion.data.providers.public import ConfigurationField, ConfigurationSpec, ProviderError
from asterion.data.providers.tushare import Tushare
from asterion.data.public import read_bars
from asterion.data.sync import DataSync, collect
from asterion.platform.config import Settings
from asterion.platform.store import jobs, metadata
from asterion.platform.tasks.service import Conflict


@pytest.fixture
def service(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/configuration.db")
    metadata.create_all(engine)
    sync = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path,
        provider_secrets(MASTER),
    )
    yield sync
    engine.dispose()


def update(revision=0, values=None, secrets=None):
    return ConfigurationUpdate(
        expected_revision=revision, values=values or {}, secrets=secrets or {}
    )


def demo_request(command_id="demo"):
    return request(
        command_id=command_id,
        provider="synthetic",
        exchange="SIM",
        symbol="DEMO001.SIM",
        start="2024-01-02",
        end="2024-01-05",
    )


def client_for(service, *, require_account=False):
    return TestClient(
        create_app(
            Settings(token=MASTER, data_root=service.root, require_account=require_account),
            raw_engine(service.engine),
        ),
        headers={"Authorization": f"Bearer {MASTER}"},
    )


def test_schema_rejects_undeclared_fields_wrong_types_and_secret_misplacement(service):
    for values in [{"seed": True}, {"seed": "7"}, {"seed": -1}, {"seed": 10001}, {"extra": 7}]:
        with pytest.raises(ProviderError):
            service.configuration.apply("synthetic", update(values=values))
    for provider, body in [
        ("tushare", update(values={"token": SECRET})),
        ("synthetic", update(secrets={"seed": SecretStr("7")})),
        ("tushare", update(secrets={"unknown": SecretStr(SECRET)})),
        ("tushare", update(secrets={"token": SecretStr("bad token")})),
        ("tushare", update(secrets={"token": SecretStr("x" * 257)})),
    ]:
        with pytest.raises(ProviderError) as error:
            service.configuration.apply(provider, body)
        assert SECRET not in str(error.value)
    assert service.configuration.state("synthetic").revision == 0
    assert service.configuration.state("synthetic").values == {"seed": 7}
    assert not service.configuration.state("tushare").configured
    with pytest.raises(ValidationError):
        ConfigurationField(id="token", label="Token", secret=True, default=SECRET)
    with pytest.raises(ValidationError):
        ConfigurationSpec(fields=[ConfigurationField(id="x", label="X")] * 2)
    spec = ConfigurationSpec(
        fields=[ConfigurationField(id="enabled", label="Enabled", type="boolean")]
    )
    assert validate(spec, {"enabled": False}) == {"enabled": False}
    with pytest.raises(ProviderError):
        validate(spec, {"enabled": 0})


def test_draft_probe_never_changes_saved_state_or_persists_draft_secret(service, monkeypatch):
    set_token(service, "tushare", SECRET)
    calls = []

    def probe(self, configuration):
        calls.append(configuration)
        return "synthetic successful probe"

    monkeypatch.setattr(Tushare, "probe", probe)
    before = sorted(service.credentials.root.rglob("*.enc"))
    checked = service.configuration.check(
        "tushare", update(1, secrets={"token": SecretStr("draft-only-token")})
    )
    assert checked.revision == 1
    assert calls == [{"token": "draft-only-token"}]
    assert service.configuration.current("tushare") == (1, {"token": SECRET})
    assert sorted(service.credentials.root.rglob("*.enc")) == before
    assert "draft-only-token" not in checked.model_dump_json()


def test_probe_reports_conflict_if_saved_configuration_changes_during_check(service, monkeypatch):
    def probe(self, configuration):
        service.configuration.apply("synthetic", update(values={"seed": 8}))
        return "probe complete"

    monkeypatch.setattr(Synthetic, "probe", probe)
    with pytest.raises(Conflict):
        service.configuration.check("synthetic", update(values={"seed": 9}))
    assert service.configuration.state("synthetic").values == {"seed": 8}


def test_saved_configuration_is_revisioned_encrypted_and_never_echoes_secrets(service):
    state = service.configuration.apply("tushare", update(secrets={"token": SecretStr(SECRET)}))
    assert state.revision == 1 and state.configured
    assert state.values == {} and state.secret_fields == ["token"]
    assert SECRET not in state.model_dump_json()
    assert SECRET not in json.dumps(service.providers())
    with pytest.raises(Conflict):
        service.configuration.apply("tushare", update(secrets={"token": SecretStr("stale-token")}))
    kept = service.configuration.apply("tushare", update(1))
    assert kept.revision == 2 and kept.configured
    assert service.configuration.current("tushare")[1] == {"token": SECRET}
    cleared = service.configuration.apply("tushare", update(2, secrets={"token": None}))
    assert cleared.revision == 3 and not cleared.configured and cleared.secret_fields == []
    with raw_engine(service.engine).connect() as conn:
        rows = [dict(row) for row in conn.execute(select(configurations)).mappings()]
    assert SECRET not in json.dumps(rows)
    for path in service.credentials.root.rglob("*.enc"):
        assert SECRET.encode() not in path.read_bytes()
        assert stat.S_IMODE(path.stat().st_mode) == 0o600


def test_account_and_pin_protect_configuration_reads_writes_and_probes(service, identity_instances):
    client = client_for(service, require_account=True)
    endpoints = [
        ("GET", "/configuration", None),
        ("POST", "/configuration", {"expected_revision": 0, "values": {"seed": 8}}),
        ("POST", "/configuration/check", {"expected_revision": 0}),
    ]
    for method, suffix, body in endpoints:
        assert (
            client.request(
                method, "/api/v1/data/providers/synthetic" + suffix, json=body
            ).status_code
            == 401
        )
    identity = identity_instances[-1]
    identity.register(
        "configuration@example.com", "test-password-123", "Config", "Test", pin="246810"
    )
    identity.verify("configuration@example.com", "000000")
    session = identity.login("configuration@example.com", "test-password-123")["session"]
    client.headers["X-Account-Session"] = session
    assert client.get("/api/v1/data/providers/synthetic/configuration").status_code == 200
    identity.pin.state(session, "lock")
    for method, suffix, body in endpoints:
        assert (
            client.request(
                method, "/api/v1/data/providers/synthetic" + suffix, json=body
            ).status_code
            == 423
        )


@pytest.mark.parametrize(
    ("suffix", "body"),
    [
        ("configuration", {"expected_revision": 0, "secrets": {"token": [SECRET]}}),
        ("configuration/check", {"expected_revision": 0, "secrets": {"token": [SECRET]}}),
        ("configuration", {"expected_revision": 0, "secrets": {str(i): SECRET for i in range(31)}}),
    ],
)
def test_api_validation_errors_do_not_echo_unparsed_secrets(service, suffix, body):
    client = client_for(service)
    result = client.post(f"/api/v1/data/providers/tushare/{suffix}", json=body)
    assert result.status_code == 422
    assert SECRET not in result.text


def test_configuration_changes_only_affect_new_jobs_and_command_replay_is_stable(
    service, monkeypatch
):
    first = service.submit(demo_request())
    initial = collect(first["payload"], service.root, provider_secrets(MASTER))
    service.configuration.apply("synthetic", update(values={"seed": 23}))
    second = service.submit(demo_request("different-command"))
    assert first["payload"]["configuration"]["ref"] != second["payload"]["configuration"]["ref"]
    assert (
        json.loads(initial)[0]["rows"]
        == json.loads(collect(first["payload"], service.root, provider_secrets(MASTER)))[0]["rows"]
    )
    assert (
        json.loads(initial)[0]["rows"]
        != json.loads(collect(second["payload"], service.root, provider_secrets(MASTER)))[0]["rows"]
    )
    assert service.submit(demo_request())["id"] == first["id"]
    with pytest.raises(Conflict):
        service.submit(demo_request().model_copy(update={"end": demo_request().start}))
    # Once accepted, replay remains available even if local configuration storage breaks.
    monkeypatch.setattr(
        service.credentials,
        "read_configuration",
        lambda *args: (_ for _ in ()).throw(ProviderError("unreadable")),
    )
    assert service.submit(demo_request())["id"] == first["id"]


def test_duplicate_command_race_returns_winner_even_when_configuration_changed(
    service, monkeypatch
):
    submit = service.tasks.submit
    winner = []

    def racing_submit(command_id, kind, payload):
        monkeypatch.setattr(service, "tasks", replace(service.tasks, submit=submit))
        service.configuration.apply("synthetic", update(values={"seed": 23}))
        winner.append(service.submit(demo_request()))
        return submit(command_id, kind, payload)

    monkeypatch.setattr(service, "tasks", replace(service.tasks, submit=racing_submit))
    assert service.submit(demo_request())["id"] == winner[0]["id"]
    assert len(scheduler(service.engine).list()) == 1


@pytest.mark.parametrize(
    "fixed", [None, {}, {"ref": "not-a-valid-ref", "schema_version": 1, "revision": 0}]
)
def test_malformed_fixed_configuration_is_rejected(service, monkeypatch, fixed):
    set_token(service, "tushare", SECRET)
    payload = service.submission_payload(request("calendar"))
    payload["configuration"] = fixed
    fetched = []
    monkeypatch.setattr(
        Tushare, "fetch", lambda self, part, config: fetched.append(config) or [raw(part)]
    )
    with pytest.raises(ProviderError):
        collect(payload, service.root, provider_secrets(MASTER))
    assert fetched == []


def test_missing_corrupted_and_wrong_schema_snapshots_fail_safely(service):
    with pytest.raises(ProviderError):
        service.submit(request("calendar"))
    set_token(service, "tushare", SECRET)
    payload = service.submission_payload(request("calendar"))
    fixed = payload["configuration"]
    path = service.credentials.root / "configurations" / f"{fixed['ref']}.enc"
    original = path.read_bytes()
    path.unlink()
    with pytest.raises(ProviderError) as missing:
        collect(payload, service.root, provider_secrets(MASTER))
    assert SECRET not in str(missing.value)
    path.write_bytes(b"invalid ciphertext")
    with pytest.raises(ProviderError):
        collect(payload, service.root, provider_secrets(MASTER))
    path.write_bytes(original)
    wrong_schema = copy.deepcopy(payload)
    wrong_schema["configuration"]["schema_version"] += 1
    with pytest.raises(ProviderError):
        collect(wrong_schema, service.root, provider_secrets(MASTER))
    wrong_revision = copy.deepcopy(payload)
    wrong_revision["configuration"]["revision"] += 1
    with pytest.raises(ProviderError):
        collect(wrong_revision, service.root, provider_secrets(MASTER))


@pytest.mark.parametrize("replacement", [SECRET, "replacement-token"])
def test_corrupted_secret_configuration_requires_explicit_replacement_and_keeps_old_job_fixed(
    service, replacement
):
    service.configuration.apply("tushare", update(secrets={"token": SecretStr(SECRET)}))
    original = service.submission_payload(request("calendar"))
    ref = original["configuration"]["ref"]
    path = service.credentials.root / "configurations" / f"{ref}.enc"
    path.write_bytes(b"damaged configuration snapshot")
    state = service.configuration.state("tushare")
    assert state.revision == 1 and not state.configured and state.error
    assert state.secret_fields == []
    assert SECRET not in state.model_dump_json()
    with pytest.raises(ProviderError):
        service.configuration.apply("tushare", update(1))
    restored = service.configuration.apply(
        "tushare", update(1, secrets={"token": SecretStr(replacement)})
    )
    assert restored.revision == 2 and restored.configured and not restored.error
    new_payload = service.submission_payload(request("calendar", command_id="restored"))
    assert new_payload["configuration"]["ref"] != ref
    assert path.read_bytes() == b"damaged configuration snapshot"
    with pytest.raises(ProviderError):
        collect(original, service.root, provider_secrets(MASTER))


def test_unreadable_ordinary_configuration_requires_explicit_values_before_replacing(service):
    service.configuration.apply("synthetic", update(values={"seed": 19}))
    original = service.submission_payload(demo_request())
    ref = original["configuration"]["ref"]
    path = service.credentials.root / "configurations" / f"{ref}.enc"
    path.unlink()
    state = service.configuration.state("synthetic")
    assert state.revision == 1 and not state.configured and state.error
    assert state.values == {"seed": 7}
    with pytest.raises(ProviderError):
        service.configuration.apply("synthetic", update(1))
    restored = service.configuration.apply("synthetic", update(1, values={"seed": 19}))
    assert restored.revision == 2 and restored.configured and restored.values == {"seed": 19}
    assert not path.exists()


def test_missing_configuration_is_rejected_before_fetch(service, monkeypatch):
    set_token(service, "tushare", SECRET)
    payload = service.submission_payload(request("calendar"))
    payload.pop("configuration")
    monkeypatch.setattr(Tushare, "fetch", lambda *args: pytest.fail("Unfixed job fetched data"))
    with pytest.raises(ProviderError, match="固定配置"):
        collect(payload, service.root, provider_secrets(MASTER))
    assert "configuration" not in payload


def test_resume_reuses_only_evidence_from_identical_frozen_configuration(service):
    original = service.submit(demo_request())
    claimed = scheduler(service.engine).claim("configuration-test")
    collect(
        claimed["payload"],
        service.root,
        provider_secrets(MASTER),
        checkpoint=lambda index, evidence: service.evidence.record(
            claimed["id"], claimed["token"], index, Observation.model_validate(evidence)
        ),
    )
    scheduler(service.engine).fail(claimed["id"], claimed["token"], "interrupted after observation")
    service.retry(original["id"], "same-config", resume=True)
    same = scheduler(service.engine).claim("same-config-worker")
    assert set(service.evidence.resume(same["id"], same["token"])) == {0}
    scheduler(service.engine).cancel(same["id"])
    service.configuration.apply("synthetic", update(values={"seed": 23}))
    service.retry(original["id"], "changed-config", resume=True)
    changed = scheduler(service.engine).claim("changed-config-worker")
    assert service.evidence.resume(changed["id"], changed["token"]) == {}


def test_synthetic_source_runs_configuration_collect_publish_catalog_and_chart(service):
    client = client_for(service)
    config = client.get("/api/v1/data/providers/synthetic/configuration").json()
    assert config["configured"] and config["values"] == {"seed": 7}
    assert (
        client.post(
            "/api/v1/data/providers/synthetic/configuration/check", json={"expected_revision": 0}
        ).status_code
        == 200
    )
    submitted = client.post("/api/v1/data/sync", json=demo_request().model_dump(mode="json"))
    assert submitted.status_code == 202  # Opt-in test-only data type, not futures identity data.
    claimed = client.post("/api/v1/jobs/claim", json={"worker_id": "demo"}).json()
    content = collect(claimed["payload"], service.root, provider_secrets(MASTER))
    published = client.post(
        f"/api/v1/jobs/{claimed['id']}/publish-data",
        content=content,
        headers={"X-Lease-Token": claimed["token"]},
    )
    assert published.status_code == 200, published.text
    version = published.json()
    assert version["manifest"]["demo"]
    preview = client.get(f"/api/v1/data/versions/{version['id']}").json()
    assert preview["total"] == 4
    snapshot = client.get("/api/v1/snapshots").json()[0]
    assert snapshot["manifest"]["demo"]
    assert "非真实行情" in snapshot["manifest"]["source"]
    bars = client.get(f"/api/v1/snapshots/{snapshot['id']}/bars").json()
    assert len(bars) == 4 and all(bar["contract"] == "SIM.DEMO001" for bar in bars)
    stored = read_bars(service.root / "published" / f"{snapshot['id']}.parquet")
    assert len(stored) == len(bars)
    for original, displayed in zip(stored, bars, strict=True):
        for name in ["event_time", "available_at"]:
            assert datetime.fromisoformat(original[name]) == datetime.fromisoformat(displayed[name])
        assert {k: v for k, v in original.items() if k not in {"event_time", "available_at"}} == {
            k: v for k, v in displayed.items() if k not in {"event_time", "available_at"}
        }


def test_claim_does_not_rewrite_unsupported_queued_payload(service):
    set_token(service, "tushare", SECRET)
    payload = service.submission_payload(request("calendar"))
    payload.pop("configuration")
    queued = service.tasks.submit("unsupported-command", "data.sync", payload)
    claimed = client_for(service).post("/api/v1/jobs/claim", json={"worker_id": "worker"}).json()
    assert claimed["id"] == queued["id"] and claimed["payload"] == payload
    with pytest.raises(ProviderError, match="固定配置"):
        collect(claimed["payload"], service.root, provider_secrets(MASTER))
    scheduler(service.engine).fail(claimed["id"], claimed["token"], "固定配置缺失")
    assert scheduler(service.engine).claim("worker") is None


@pytest.mark.skipif(
    not os.getenv("ASTERION_TEST_DATABASE_URL"), reason="PostgreSQL test URL not set"
)
def test_postgres_configuration_cas_has_one_winner_and_submission_is_idempotent(
    tmp_path, monkeypatch, development_providers
):
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
            provider_secrets(MASTER),
        )
        candidate = ProviderConfigurations.candidate
        for revision in [0, 1]:
            barrier = Barrier(6)

            def synchronized_candidate(self, provider, body, barrier=barrier):
                values = candidate(self, provider, body)
                barrier.wait(timeout=10)
                return values

            monkeypatch.setattr(ProviderConfigurations, "candidate", synchronized_candidate)

            def apply(index, revision=revision):
                try:
                    return service.configuration.apply(
                        "synthetic", update(revision, {"seed": index})
                    )
                except Conflict:
                    return None

            with ThreadPoolExecutor(max_workers=6) as pool:
                results = list(pool.map(apply, range(6)))
            winners = [result for result in results if result is not None]
            assert len(winners) == 1
            assert winners[0] == service.configuration.state("synthetic")
            assert winners[0].revision == revision + 1
        monkeypatch.setattr(ProviderConfigurations, "candidate", candidate)
        with ThreadPoolExecutor(max_workers=6) as pool:
            tasks = list(pool.map(lambda _: service.submit(demo_request()), range(6)))
        assert len({task["id"] for task in tasks}) == 1
        with engine.connect() as conn:
            assert len(conn.execute(select(jobs)).all()) == 1
    finally:
        engine.dispose()
        with admin.begin() as conn:
            conn.execute(text(f"DROP SCHEMA {schema} CASCADE"))
        admin.dispose()


pytestmark = pytest.mark.usefixtures("development_providers")


def test_publication_rejects_a_task_without_fixed_configuration(service):
    queued = service.submit(demo_request())
    claimed = scheduler(service.engine).claim("worker")
    content = collect(claimed["payload"], service.root, provider_secrets(MASTER))
    unsupported = dict(claimed["payload"])
    unsupported.pop("configuration")
    with raw_engine(service.engine).begin() as conn:
        conn.execute(jobs.update().where(jobs.c.id == queued["id"]).values(payload=unsupported))
    with pytest.raises(ProviderError, match="固定配置"):
        service.publish(queued["id"], claimed["token"], content)
    assert service.library.list()["total"] == 0
