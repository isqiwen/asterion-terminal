from asterion_bindings.execution import ExecutionFactory
from asterion_bindings.tasks import ExecutionContext
from storage_support import data_store, raw_engine, research_store, scheduler

from asterion.distribution import strategy_catalog
from asterion.research.execution import EXECUTION
from asterion.research.strategies import STRATEGY_RESOURCE

"""Archiving changes visibility, never immutable inputs or replay availability."""

import copy

import pytest
from asterion_bindings.task_repository import Conflict
from entry_support import entry_lifecycle
from sqlalchemy import select
from test_research import services  # noqa: F401

from asterion.data.library import DataLibrary, versions
from asterion.research.packages import ResearchPackages
from asterion.research.worker import execute
from asterion.research.workspace import DocumentUpdate, ResearchWorkspace


@pytest.fixture
def lifecycle(request):
    research, body, root = request.getfixturevalue("services")
    workspace = ResearchWorkspace(research_store(research.engine))
    packages = ResearchPackages(research)
    library = DataLibrary(data_store(research.engine), root / "data")
    url = str(raw_engine(research.engine).url)
    with entry_lifecycle(url, root / "data") as manager:
        yield research, body, workspace, packages, library, manager


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
            ExecutionContext(
                (
                    STRATEGY_RESOURCE,
                    EXECUTION,
                ),
                {STRATEGY_RESOURCE: strategy_catalog(), EXECUTION: ExecutionFactory()},
            ),
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
        "reference_catalogs": 0,
        "contract_rules": 0,
        "contract_roles": 0,
    }
    assert "private@example.com" not in str(state)
    assert state["protected"] and not state["can_delete"]
    archived = manager.archive(body.version_id, archived=True, expected_revision=0)
    assert archived["archived"] and archived["revision"] == 1
    assert manager.archive(body.version_id, archived=True, expected_revision=0) == archived
    assert library.list(type_id="futures.daily", layer="STANDARD")["total"] == 0
    assert library.list(type_id="futures.daily", layer="STANDARD", include_archived=True)["items"][
        0
    ]["archived"]
    assert library.history(before["version"]["dataset_id"])["items"][0]["archived"]
    assert library.preview(body.version_id) == before
    assert research.rerun(submitted["id"], "archived-rerun")["state"] == "QUEUED"
    assert packages.receive("other-account", package)["can_replay"]
    restored = manager.archive(body.version_id, archived=False, expected_revision=1)
    assert not restored["archived"] and restored["revision"] == 2
    with pytest.raises(Conflict):
        manager.archive(body.version_id, archived=True, expected_revision=0)
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
    manager.archive(body.version_id, archived=True, expected_revision=0)
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
        manager.archive("missing", archived=True, expected_revision=0)


def test_archive_api_auth_cas_and_no_deletion(lifecycle):
    _, body, _, _, library, manager = lifecycle
    client = manager.client
    path = f"/data/versions/{body.version_id}"
    anonymous = client.get(path + "/lifecycle", headers={"Authorization": ""})
    assert anonymous.status_code == 401
    assert client.get(path + "/lifecycle").json()["revision"] == 0
    archive = {"archived": True, "expected_revision": 0}
    assert client.post(path + "/archive", json=archive).status_code == 200
    stale = {"archived": False, "expected_revision": 0}
    assert client.post(path + "/archive", json=stale).status_code == 409
    for invalid in ({"archived": True}, {**archive, "expected_revision": -1}, {**archive, "x": 1}):
        assert client.post(path + "/archive", json=invalid).status_code == 422
    assert library.list(type_id="futures.daily", layer="STANDARD")["total"] == 0
    assert (
        library.list(type_id="futures.daily", layer="STANDARD", include_archived=True)["total"] == 1
    )
    # Versions have no deletion operation.
    assert client.delete(path).status_code == 405


def test_postgres_archive_compare_and_swap(tmp_path):
    import os
    from concurrent.futures import ThreadPoolExecutor
    from uuid import uuid4

    from asterion_bindings.database import create_engine

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

    def update(archived):
        try:
            return manager.archive(ident, archived=archived, expected_revision=0)["revision"]
        except Conflict:
            return "conflict"

    try:
        with entry_lifecycle(url, tmp_path) as manager:
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
