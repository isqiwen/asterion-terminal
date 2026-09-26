"""Role worker publication, fencing and exact-input retry with offline evidence."""

from datetime import datetime
from importlib import import_module

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.task_repository import Tasks, task_port
from sqlalchemy import select
from test_role_sequence import campaign

from asterion.contract_roles.plugin import computed_versions
from asterion.contract_roles.tasks import KIND, RoleTasks, Submission, execute
from asterion.distribution_storage import role_storage
from asterion.platform.communication.schema import initialize_core


@pytest.fixture
def setup(tmp_path, monkeypatch):
    values, sources, first, second, body = campaign()
    engine = create_engine(f"sqlite:///{tmp_path}/tasks.db")
    initialize_core(engine)
    storage = role_storage(engine)
    storage.initialize(computed_versions)
    with storage.begin() as conn:
        conn.execute(
            computed_versions.insert().values(
                id=first.id,
                spec=first.spec.model_dump(mode="json"),
                published_at=first.published_at.isoformat(),
            )
        )
    tasks = Tasks(engine)
    service = RoleTasks(
        storage,
        task_port(tasks, frozenset({KIND})),
        sources,
        lambda identifier: first if identifier == first.id else None,
        computed_versions,
    )

    class Clock(datetime):
        @classmethod
        def now(cls, tz=None):
            return second.published_at

    monkeypatch.setattr(import_module("asterion.contract_roles.tasks"), "datetime", Clock)
    yield (
        service,
        tasks,
        storage,
        values,
        second,
        Submission(command_id="continue-day", continuation=body),
    )
    storage.close()
    engine.dispose()


def test_idempotent_submission_worker_and_atomic_publication(setup):
    service, tasks, storage, _, second, body = setup
    queued = service.submit(body)
    assert service.submit(body).id == queued.id
    assert "token" not in queued.model_dump() and "payload" not in queued.model_dump()
    claimed = tasks.claim("offline-worker")
    content, headers = execute(None, claimed["payload"])
    assert headers == {}
    result = service.publish(claimed["id"], claimed["token"], content)
    assert result["computed_version_id"] == second.id
    assert tasks.get(queued.id)["state"] == "SUCCEEDED"
    assert service.publish(claimed["id"], claimed["token"], content) == result
    with pytest.raises(ValueError):
        service.publish(claimed["id"], "wrong-token", content)
    with pytest.raises(ValueError):
        service.publish(claimed["id"], claimed["token"], b"{}")
    with storage.connect() as conn:
        assert len(conn.execute(select(computed_versions)).all()) == 2


@pytest.mark.parametrize("failure", ["source", "lease", "late", "content"])
def test_failed_publication_leaves_no_new_version(setup, failure, monkeypatch):
    service, tasks, storage, values, second, body = setup
    service.submit(body)
    claimed = tasks.claim("offline-worker")
    content, _ = execute(None, claimed["payload"])
    token = claimed["token"]
    if failure == "source":
        values["daily-11-0"]["rows"][0]["oi"] = "99"
    elif failure == "lease":
        token = "expired-token"
    elif failure == "late":

        class LateClock(datetime):
            @classmethod
            def now(cls, tz=None):
                return second.published_at.replace(hour=22)

        monkeypatch.setattr(import_module("asterion.contract_roles.tasks"), "datetime", LateClock)
    else:
        content = b"{}"
    with pytest.raises(ValueError):
        service.publish(claimed["id"], token, content)
    assert tasks.get(claimed["id"])["state"] == "RUNNING"
    with storage.connect() as conn:
        assert len(conn.execute(select(computed_versions)).all()) == 1


def test_retry_preserves_payload_and_has_only_one_successor(setup):
    service, tasks, _, _, _, body = setup
    original = service.submit(body)
    claimed = tasks.claim("offline-worker")
    with pytest.raises(ValueError):
        service.retry(original.id)
    tasks.fail(claimed["id"], claimed["token"], "offline temporary failure")
    retry = service.retry(original.id)
    assert retry.id != original.id and service.retry(original.id).id == retry.id
    second = tasks.claim("replacement-worker")
    assert second["payload"] == claimed["payload"]
    content, _ = execute(None, second["payload"])
    service.publish(second["id"], second["token"], content)
    assert service.retry(original.id).id == retry.id


def test_task_api_authentication_and_pending_source_references(setup, tmp_path, monkeypatch):
    from fastapi.testclient import TestClient
    from role_source_support import port

    from asterion.api.app import create_app
    from asterion.contract_roles.computed import ComputedSources
    from asterion.platform.config import Settings

    _, tasks, _, values, second, body = setup
    monkeypatch.setattr(
        ComputedSources, "__init__", lambda self, versions: setattr(self, "versions", port(values))
    )
    settings = Settings(
        token="role-task-api-token-24-characters", data_root=tmp_path, require_account=False
    )
    with TestClient(create_app(settings, tasks.engine)) as client:
        path = "/api/v1/contract-roles/computed/tasks"
        assert client.post(path, json=body.model_dump(mode="json")).status_code == 401
        client.headers["Authorization"] = "Bearer " + settings.token
        response = client.post(path, json=body.model_dump(mode="json"))
        assert response.status_code == 202
        queued = response.json()
        assert "payload" not in queued and "token" not in queued
        assert client.post(path, json=body.model_dump(mode="json")).json()["id"] == queued["id"]
        claimed = tasks.claim("api-worker")
        content, _ = execute(None, claimed["payload"])
        result = client.post(
            f"/api/v1/jobs/{queued['id']}/publish-roles",
            content=content,
            headers={"x-lease-token": claimed["token"]},
        )
        assert result.status_code == 200, result.text
        assert result.json()["computed_version_id"] == second.id
        assert client.get(path + "/" + queued["id"]).json()["state"] == "SUCCEEDED"
