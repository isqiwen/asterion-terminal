"""Durable publication barrier with real task transactions and offline source evidence."""

import pytest
from role_source_support import port
from sqlalchemy import select
from test_role_tasks import setup as role_setup

setup = role_setup

from asterion_bindings.task_repository import task_port

from asterion.contract_roles.sync_workflow import SyncContinuation, SyncWorkflow
from asterion.contract_roles.tasks import KIND
from asterion.data.public import SyncAccess
from asterion.data.sync_dependencies import access
from asterion.distribution_storage import data_storage
from asterion.platform.store import jobs


def workflow(setup):
    roles, tasks, storage, values, _, submission = setup
    data = data_storage(tasks.engine)
    public = access(data, None)
    identifiers = []
    for index, symbol in enumerate(("opaque-au", "opaque-au-next")):
        record = tasks.submit(
            f"daily-{index}",
            "data.sync",
            {
                "type_id": "futures.daily",
                "request": {
                    "provider": "offline-feed",
                    "symbol": symbol,
                    "exchange": "SHFE",
                    "start": "2025-04-11",
                    "end": "2025-04-11",
                },
                "contract_identity": {"catalog": {"inputs": [{"version_id": "contracts-1"}]}},
            },
        )
        identifiers.append(record["id"])
    service = SyncWorkflow(
        storage, roles.tasks, SyncAccess(public.inspect, lambda conn: port(values)), roles
    )
    request = SyncContinuation(
        command_id="daily-continuation",
        previous_version_id=submission.continuation.previous_version_id,
        trading_day="2025-04-11",
        sync_job_ids=tuple(identifiers),
        explanation="offline publication barrier",
    )
    return service, request, data


def finish(service, tasks, data, index, *, rollback=False):
    job = tasks.claim("data-worker")
    with data.begin() as conn:
        task_port(tasks, frozenset({"data.sync"})).complete(
            conn, job["id"], job["token"], {"version_id": f"daily-11-{index}"}
        )
        service.published(conn, job["id"])
        if rollback:
            raise RuntimeError("explicit transaction rollback")
    return job


def test_last_publication_queues_once_and_survives_service_recreation(setup):
    service, body, data = workflow(setup)
    _, tasks, storage, _, _, _ = setup
    enrolled = service.submit(body)
    assert enrolled["job_id"] is None
    first = finish(service, tasks, data, 0)
    assert service.get(enrolled["id"])["job_id"] is None
    restored = SyncWorkflow(storage, service.tasks, service.data, service.roles)
    last = finish(restored, tasks, data, 1)
    result = restored.get(enrolled["id"])
    assert result["job_id"]
    with data.begin() as conn:
        restored.published(conn, first["id"])
        restored.published(conn, last["id"])
    assert restored.submit(body)["job_id"] == result["job_id"]
    with storage.connect() as conn:
        assert len(conn.execute(select(jobs)).all()) == 1
    queued = tasks.claim("role-worker")
    assert queued["id"] == result["job_id"] and queued["kind"] == KIND
    data.close()


def test_subscribe_after_completion_also_advances(setup):
    service, body, data = workflow(setup)
    _, tasks, _, _, _, _ = setup
    finish(service, tasks, data, 0)
    finish(service, tasks, data, 1)
    assert service.submit(body)["job_id"]
    data.close()


def test_publication_and_followup_roll_back_together(setup):
    service, body, data = workflow(setup)
    _, tasks, storage, _, _, _ = setup
    enrolled = service.submit(body)
    finish(service, tasks, data, 0)
    with pytest.raises(RuntimeError):
        finish(service, tasks, data, 1, rollback=True)
    assert service.get(enrolled["id"])["job_id"] is None
    with storage.connect() as conn:
        assert len(conn.execute(select(jobs)).all()) == 0
    data.close()


@pytest.mark.parametrize("change", ["missing", "wrong_day", "duplicate", "failed", "corrupt"])
def test_incomplete_or_invalid_dependency_never_queues(setup, change):
    service, body, data = workflow(setup)
    _, tasks, storage, values, _, _ = setup
    if change in {"missing", "duplicate", "wrong_day"}:
        payload = body.model_dump()
        if change == "missing":
            payload["sync_job_ids"] = ("missing", body.sync_job_ids[0])
        elif change == "duplicate":
            payload["sync_job_ids"] = (body.sync_job_ids[0], body.sync_job_ids[0])
        else:
            payload["trading_day"] = "2025-04-14"
        with pytest.raises(ValueError):
            service.submit(SyncContinuation.model_validate(payload))
    else:
        enrolled = service.submit(body)
        if change == "failed":
            job = tasks.claim("data-worker")
            tasks.fail(job["id"], job["token"], "offline failure")
        else:
            finish(service, tasks, data, 0)
            values["daily-11-0"]["rows"][0]["oi"] = None
        finish(service, tasks, data, 1)
        state = service.get(enrolled["id"])
        assert state["job_id"] is None and state["error"]
    with storage.connect() as conn:
        assert len(conn.execute(select(jobs)).all()) == 0
    data.close()


def test_workflow_api_auth_and_backup_journal(setup, tmp_path, monkeypatch):
    from copy import deepcopy

    from fastapi.testclient import TestClient

    from asterion.api.app import create_app
    from asterion.contract_roles.computed import ComputedSources
    from asterion.contract_roles.plugin import RoleBackup, computed_versions, validate
    from asterion.platform.config import Settings

    _, body, data = workflow(setup)
    _, tasks, storage, values, _, _ = setup
    monkeypatch.setattr(
        ComputedSources, "__init__", lambda self, versions: setattr(self, "versions", port(values))
    )
    settings = Settings(token="auto-roles-test-token-24", data_root=tmp_path, require_account=False)
    with TestClient(create_app(settings, tasks.engine)) as client:
        path = "/api/v1/contract-roles/computed/sync-workflows"
        assert client.post(path, json=body.model_dump(mode="json")).status_code == 401
        client.headers["Authorization"] = "Bearer " + settings.token
        response = client.post(path, json=body.model_dump(mode="json"))
        assert response.status_code == 202, response.text
        journal = response.json()
        assert client.post(path, json=body.model_dump(mode="json")).json() == journal
        status = client.get(path + "/" + journal["id"]).json()
        assert len(status["dependencies"]) == 2 and status["job_id"] is None
        assert all("token" not in row and "payload" not in row for row in status["dependencies"])
        with storage.connect() as conn:
            previous = dict(conn.execute(select(computed_versions)).mappings().one())
        assert (
            validate(RoleBackup((), port(values), (previous,), (journal,)))[
                "computed_role_versions"
            ]
            == 1
        )
        invalid = deepcopy(journal)
        invalid["request"]["previous_version_id"] = "f" * 64
        with pytest.raises(ValueError):
            validate(RoleBackup((), port(values), (previous,), (invalid,)))
    data.close()
