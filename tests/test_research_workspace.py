from uuid import uuid4

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.task_repository import Conflict
from fastapi.testclient import TestClient
from storage_support import raw_engine, research_store

from asterion.api.app import create_app
from asterion.platform.config import Settings
from asterion.research.workspace import DocumentUpdate, ResearchWorkspace


@pytest.fixture
def workspace(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/workspace.db")
    service = ResearchWorkspace(research_store(engine))
    yield service
    engine.dispose()


def update(revision=0, **config):
    return DocumentUpdate(expected_revision=revision, name="均线研究", content={"config": config})


def test_partial_draft_survives_new_service_and_is_account_scoped(workspace):
    first = workspace.save("alice", "draft", update(capital="", rules=None, version_id="fixed-old"))
    assert first["revision"] == 1
    assert ResearchWorkspace(research_store(workspace.engine)).read("alice")["draft"] == first
    assert workspace.read("bob") == {"draft": None, "templates": []}
    with pytest.raises(Conflict):
        workspace.save("bob", "draft", update(1))


def test_cas_conflict_and_response_lost_retry(workspace):
    original = workspace.save("alice", "draft", update(parameters={"fast": 5}))
    assert workspace.save("alice", "draft", update(parameters={"fast": 5})) == original
    with pytest.raises(Conflict):
        workspace.save("alice", "draft", update(parameters={"fast": 8}))
    second = workspace.save("alice", "draft", update(1, parameters={"fast": 8}))
    assert second["revision"] == 2
    with pytest.raises(Conflict):
        workspace.save("alice", "draft", update(parameters={"fast": 5}))
    assert workspace.read("alice")["draft"] == second


def test_template_rename_delete_and_stale_recreation_are_fenced(workspace):
    ident = str(uuid4())
    first = workspace.save(
        "alice", ident, update(version_id="immutable", coverage_report_id="report-old")
    )
    renamed = workspace.save(
        "alice", ident, DocumentUpdate(expected_revision=1, name="新名称", content=first["content"])
    )
    assert renamed["revision"] == 2
    with pytest.raises(Conflict):
        workspace.delete("alice", ident, 1)
    workspace.delete("alice", ident, 2)
    assert workspace.delete("alice", ident, 2) == {"status": "deleted"}
    assert not workspace.read("alice")["templates"]
    with pytest.raises(Conflict):
        workspace.save("alice", ident, update())
    assert workspace.save("bob", ident, update())["revision"] == 1


def test_draft_cannot_persist_acknowledgement_or_arbitrary_payload():
    with pytest.raises(ValueError):
        DocumentUpdate.model_validate(
            {"expected_revision": 0, "content": {"config": {}, "ack": True}}
        )
    with pytest.raises(ValueError):
        update(token="secret")


def test_workspace_api_auth_and_owner_is_server_derived(tmp_path, accounts):
    engine = create_engine(f"sqlite:///{tmp_path}/api.db")
    app = create_app(
        Settings(token="test-workspace-runtime-token", data_root=tmp_path, require_account=True),
        raw_engine(engine),
    )
    for name in ("alice", "bob"):
        accounts[name] = ("unlocked", f"{name}@example.com")
    client = TestClient(app)
    assert client.get("/api/v1/research/workspace").status_code == 401
    client.headers.update(
        {"Authorization": "Bearer test-workspace-runtime-token", "X-Account-Session": "alice"}
    )
    body = update().model_dump(mode="json")
    assert client.post("/api/v1/research/workspace/draft", json=body).status_code == 200
    assert client.get("/api/v1/research/workspace").json()["draft"]["revision"] == 1
    client.headers["X-Account-Session"] = "bob"
    assert client.get("/api/v1/research/workspace").json()["draft"] is None
    assert (
        client.post(
            "/api/v1/research/workspace/draft?expected_account=alice@example.com", json=body
        ).status_code
        == 409
    )
    accounts["bob"] = ("locked", "bob@example.com")
    assert client.post("/api/v1/research/workspace/draft", json=body).status_code == 423
    engine.dispose()
