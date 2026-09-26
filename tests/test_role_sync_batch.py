"""Role batch orchestration uses declared ports and one transaction."""

import pytest
from asterion_bindings.task_models import Job
from asterion_bindings.task_repository import task_port
from role_source_support import port
from sqlalchemy import select
from test_role_tasks import setup as role_setup

from asterion.contract_roles.sync_batch import BatchContinuation, submit
from asterion.contract_roles.sync_workflow import SyncWorkflow, workflows
from asterion.data.public import SyncAccess, SyncBatchAccess
from asterion.data.sync_dependencies import access
from asterion.distribution_storage import data_storage
from asterion.platform.store import jobs

setup = role_setup


def ports(setup, fail=False):
    roles, tasks, storage, values, _, body = setup
    data = data_storage(tasks.engine)
    projection = access(data, None)
    workflow = SyncWorkflow(
        storage, roles.tasks, SyncAccess(projection.inspect, lambda conn: port(values)), roles
    )

    def create(transaction, request):
        with data.join(transaction) as conn:
            commands = [
                (
                    f"{request.command_prefix}:{index}",
                    "data.sync",
                    {
                        "type_id": "futures.daily",
                        "request": {
                            "provider": request.provider,
                            "connection_id": request.connection_id,
                            "exchange": request.exchange,
                            "symbol": symbol,
                            "start": request.trading_day.isoformat(),
                            "end": request.trading_day.isoformat(),
                        },
                        "contract_identity": {
                            "catalog": {"inputs": [{"version_id": request.contracts_version_id}]}
                        },
                    },
                )
                for index, symbol in enumerate(request.symbols)
            ]
            result = task_port(tasks, frozenset({"data.sync"})).submit_batch(conn, commands)
            if fail:
                raise ValueError("offline batch failure after insert")
            return tuple(Job.model_validate(row) for row in result)

    request = BatchContinuation(
        command_id="batch-flow",
        previous_version_id=body.continuation.previous_version_id,
        trading_day="2025-04-11",
        connection_id=None,
        explanation="offline batch workflow",
    )
    return workflow, SyncBatchAccess(create), request, data


def test_batch_receipt_is_idempotent_and_rejects_changed_command(setup):
    workflow, batch, request, data = ports(setup)
    row = submit(workflow, batch, request)
    assert len(row["request"]["sync_job_ids"]) == 2
    assert submit(workflow, batch, request) == row
    with pytest.raises(ValueError):
        submit(workflow, batch, request.model_copy(update={"connection_id": "different"}))
    with pytest.raises(ValueError):
        submit(workflow, batch, request.model_copy(update={"explanation": "different"}))
    with data.connect() as conn:
        assert len(conn.execute(select(jobs)).all()) == 2
    data.close()


def test_batch_failure_rolls_back_jobs_and_registration(setup):
    workflow, batch, request, data = ports(setup, fail=True)
    with pytest.raises(ValueError, match="batch failure"):
        submit(workflow, batch, request)
    with data.connect() as conn:
        assert conn.execute(select(jobs)).first() is None
    with workflow.storage.connect() as conn:
        assert conn.execute(select(workflows)).first() is None
    data.close()


def test_next_day_required_before_batch_is_called(setup):
    workflow, batch, request, data = ports(setup)
    with pytest.raises(ValueError, match="下一个"):
        submit(
            workflow,
            batch,
            request.model_copy(update={"trading_day": request.trading_day.replace(day=14)}),
        )
    with data.connect() as conn:
        assert conn.execute(select(jobs)).first() is None
    data.close()


def test_batch_flows_through_completion_to_role_publication(setup):
    from test_role_sync_workflow import finish

    from asterion.contract_roles.tasks import KIND, execute

    workflow, batch, request, data = ports(setup)
    roles, tasks, _, _, _, _ = setup
    receipt = submit(workflow, batch, request)
    finish(workflow, tasks, data, 0)
    assert workflow.get(receipt["id"])["job_id"] is None
    finish(workflow, tasks, data, 1)
    queued = tasks.claim("offline-role-worker")
    assert queued["kind"] == KIND
    content, _ = execute(None, queued["payload"])
    result = roles.publish(queued["id"], queued["token"], content)
    assert result["previous_version_id"] == request.previous_version_id
    assert tasks.get(queued["id"])["state"] == "SUCCEEDED"
    assert submit(workflow, batch, request)["job_id"] == queued["id"]
    data.close()


def test_batch_endpoint_authentication_and_receipt(setup, tmp_path, monkeypatch):
    from fastapi.testclient import TestClient

    from asterion.api.app import create_app
    from asterion.contract_roles.computed import ComputedSources
    from asterion.platform.config import Settings

    _, batch, request, data = ports(setup)
    _, tasks, _, values, _, _ = setup
    monkeypatch.setattr(
        ComputedSources, "__init__", lambda self, versions: setattr(self, "versions", port(values))
    )
    monkeypatch.setattr(
        "asterion.data.plugin.submit_batch", lambda sync, conn, body: batch.submit(conn, body)
    )
    settings = Settings(
        token="batch-api-test-token-at-least-24", data_root=tmp_path, require_account=False
    )
    with TestClient(create_app(settings, tasks.engine)) as client:
        path = "/api/v1/contract-roles/computed/sync-batches"
        payload = request.model_dump(mode="json")
        assert client.post(path, json=payload).status_code == 401
        plan_path = f"/api/v1/contract-roles/computed/{request.previous_version_id}/sync-plan"
        list_path = f"/api/v1/contract-roles/computed/{request.previous_version_id}/sync-workflows"
        assert client.get(plan_path).status_code == 401
        assert client.get(list_path).status_code == 401
        client.headers["Authorization"] = "Bearer " + settings.token
        plan = client.get(plan_path).json()
        assert plan["trading_day"] == request.trading_day.isoformat()
        assert len(plan["symbols"]) == 2
        assert client.get(list_path).json() == []
        with data.connect() as conn:
            assert conn.execute(select(jobs)).first() is None
        response = client.post(path, json=payload)
        assert response.status_code == 202, response.text
        assert client.post(path, json=payload).json() == response.json()
        assert client.get(list_path).json() == [response.json()]
    data.close()
