from storage_support import data_store, raw_engine, research_store, scheduler

from asterion.distribution import strategy_catalog
from asterion.platform.tasks.execution import ExecutionContext
from asterion.research.strategies import STRATEGY_RESOURCE

"""Archiving changes visibility, never immutable inputs or replay availability."""

import copy
from pathlib import Path

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import select
from test_research import services  # noqa: F401

from asterion.api.app import create_app
from asterion.data.library import DataLibrary, versions
from asterion.data.lifecycle import ArchiveRequest, VersionLifecycle
from asterion.platform.config import Settings
from asterion.platform.tasks.service import Conflict
from asterion.research.packages import ResearchPackages
from asterion.research.public import version_references
from asterion.research.worker import execute
from asterion.research.workspace import DocumentUpdate, ResearchWorkspace


@pytest.fixture
def lifecycle(request):
    research, body, root = request.getfixturevalue("services")
    workspace = ResearchWorkspace(research_store(research.engine))
    packages = ResearchPackages(research)
    library = DataLibrary(data_store(research.engine), root / "data")

    def references(transaction, version_id):
        with research_store(research.engine).borrow(transaction) as reader:
            return version_references(reader, version_id)

    manager = VersionLifecycle(data_store(research.engine), (references,))
    return research, body, workspace, packages, library, manager


def test_archive_preserves_frozen_inputs_templates_packages_and_original_files(lifecycle):
    research, body, workspace, packages, library, manager = lifecycle
    before = library.preview(body.version_id)
    workspace.save(
        "private@example.com",
        "draft",
        DocumentUpdate(expected_revision=0, content={"config": {"version_id": body.version_id}}),
    )
    submitted = research.submit(body)
    assert manager.inspect(body.version_id)["references"]["research_runs"] == 1
    claimed = scheduler(research.engine).claim("worker")
    research.publish(
        submitted["id"],
        claimed["token"],
        execute(
            ExecutionContext((STRATEGY_RESOURCE,), {STRATEGY_RESOURCE: strategy_catalog()}),
            claimed["payload"],
        )[0],
    )
    package = packages.export(submitted["id"])
    packages.receive("private@example.com", package)
    state = manager.inspect(body.version_id)
    assert state["references"] == {
        "coverage_reports": 1,
        "research_runs": 1,
        "research_documents": 1,
        "research_packages": 1,
    }
    assert "private@example.com" not in str(state)
    assert state["protected"] and not state["can_delete"]
    request = ArchiveRequest(archived=True, expected_revision=0)
    archived = manager.archive(body.version_id, request)
    assert archived["archived"] and archived["revision"] == 1
    assert manager.archive(body.version_id, request) == archived
    assert library.list(type_id="futures.daily", layer="STANDARD")["total"] == 0
    assert library.list(type_id="futures.daily", layer="STANDARD", include_archived=True)["items"][
        0
    ]["archived"]
    assert library.history(before["version"]["dataset_id"])["items"][0]["archived"]
    assert library.preview(body.version_id) == before
    assert research.rerun(submitted["id"], "archived-rerun")["state"] == "QUEUED"
    assert packages.receive("other-account", package)["can_replay"]
    restored = manager.archive(body.version_id, ArchiveRequest(archived=False, expected_revision=1))
    assert not restored["archived"] and restored["revision"] == 2
    with pytest.raises(Conflict):
        manager.archive(body.version_id, request)
    assert library.list(type_id="futures.daily", layer="STANDARD")["total"] == 1


def test_latest_archive_does_not_silently_select_older_version_and_raw_lineage(lifecycle):
    research, body, _, _, library, manager = lifecycle
    original = library.preview(body.version_id)["version"]
    raw_id = original["manifest"]["inputs"][0]
    assert manager.inspect(raw_id)["references"]["data_lineage"] == 1
    with raw_engine(research.engine).begin() as conn:
        row = dict(
            conn.execute(select(versions).where(versions.c.id == body.version_id)).mappings().one()
        )
        old = copy.deepcopy(row)
        old.update(id="old-version", created_at=row["created_at"] - 10)
        conn.execute(versions.insert().values(**old))
    manager.archive(body.version_id, ArchiveRequest(archived=True, expected_revision=0))
    assert library.list(type_id="futures.daily", layer="STANDARD")["items"] == []
    assert {r["id"] for r in library.history(original["dataset_id"])["items"]} == {
        body.version_id,
        "old-version",
    }
    with raw_engine(research.engine).begin() as conn:
        fresh = copy.deepcopy(row)
        fresh.update(id="new-version", created_at=row["created_at"] + 10)
        conn.execute(versions.insert().values(**fresh))
    assert (
        library.list(type_id="futures.daily", layer="STANDARD")["items"][0]["id"] == "new-version"
    )
    assert manager.inspect(body.version_id)["archived"]
    with pytest.raises(KeyError):
        manager.archive("missing", ArchiveRequest(archived=True, expected_revision=0))


def test_archive_api_auth_cas_and_no_deletion(lifecycle):
    research, body, _, _, library, _ = lifecycle
    settings = Settings(
        database_url=str(raw_engine(research.engine).url),
        data_root=Path(library.root),
        token="test-lifecycle-token-at-least-24",
        require_account=False,
    )
    client = TestClient(create_app(settings))
    path = f"/api/v1/data/versions/{body.version_id}"
    assert client.get(path + "/lifecycle").status_code == 401
    client.headers["Authorization"] = "Bearer test-lifecycle-token-at-least-24"
    assert client.get(path + "/lifecycle").json()["revision"] == 0
    assert (
        client.post(path + "/archive", json={"archived": True, "expected_revision": 0}).status_code
        == 200
    )
    assert (
        client.post(path + "/archive", json={"archived": False, "expected_revision": 0}).status_code
        == 409
    )
    assert (
        client.get("/api/v1/data/catalog?type_id=futures.daily&layer=STANDARD").json()["total"] == 0
    )
    assert (
        client.get(
            "/api/v1/data/catalog?type_id=futures.daily&layer=STANDARD&include_archived=true"
        ).json()["total"]
        == 1
    )
    assert client.delete(path).status_code == 405


def test_postgres_archive_compare_and_swap(tmp_path):
    import os
    from concurrent.futures import ThreadPoolExecutor
    from uuid import uuid4

    from sqlalchemy import create_engine

    from asterion.data.library import collections
    from asterion.data.version_state import version_states
    from asterion.platform.store import metadata

    url = os.getenv("ASTERION_TEST_DATABASE_URL")
    if not url:
        pytest.skip("PostgreSQL test URL required")
    engine = create_engine(url)
    metadata.create_all(engine)
    library = DataLibrary(data_store(engine), tmp_path)
    ident = str(uuid4())
    with engine.begin() as conn:
        dataset = library.ensure_collection(
            conn, "futures.daily", "local_file", {"source_id": ident}, "STANDARD"
        )
        conn.execute(
            versions.insert().values(
                id=ident,
                dataset_id=dataset,
                job_id=ident,
                created_at=1,
                rows=0,
                manifest={"inputs": []},
            )
        )
    manager = VersionLifecycle(data_store(engine))

    def update(archived):
        try:
            return manager.archive(ident, ArchiveRequest(archived=archived, expected_revision=0))[
                "revision"
            ]
        except Conflict:
            return "conflict"

    try:
        with ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = list(pool.map(update, [True, False]))
        assert sorted(map(str, outcomes)) == ["1", "conflict"]
        assert manager.inspect(ident)["revision"] == 1
    finally:
        with engine.begin() as conn:
            conn.execute(version_states.delete().where(version_states.c.version_id == ident))
            conn.execute(versions.delete().where(versions.c.id == ident))
            conn.execute(collections.delete().where(collections.c.id == dataset))
        engine.dispose()
