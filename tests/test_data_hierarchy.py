"""Business navigation stays exact across pagination, sources and contract lifetimes."""

import pytest
from asterion_bindings.database import create_engine
from storage_support import data_store

from asterion.data.library import DataLibrary, versions
from asterion.data.version_state import version_states


@pytest.fixture
def library(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/hierarchy.db")
    library = DataLibrary(data_store(engine), tmp_path)
    yield library, engine
    engine.dispose()


def add(library, engine, identifier, contracts, *, source="fixture", archived=False):
    scope = {"contract_ids": contracts}
    with engine.begin() as conn:
        dataset = library.ensure_collection(conn, "futures.daily", source, scope, "STANDARD")
        conn.execute(
            versions.insert().values(
                id=identifier,
                dataset_id=dataset,
                job_id=identifier,
                created_at=1,
                rows=1,
                manifest={
                    "scope": scope,
                    "type": {"id": "futures.daily", "frequency": "1d"},
                    "source": source,
                    "layer": "STANDARD",
                },
            )
        )
        if archived:
            conn.execute(
                version_states.insert().values(
                    version_id=identifier, archived=True, revision=1, updated_at=1
                )
            )
    return dataset


def test_directory_filters_before_pagination_and_deduplicates_multi_contracts(library):
    store, engine = library
    a, b = "CZCE.MA.202601.20250101", "CZCE.MA.203601.20350101"
    add(store, engine, "multi", [a, b])
    add(store, engine, "source-two", [a], source="other")
    add(store, engine, "hidden", [a], source="archived", archived=True)
    # More than one ordinary catalogue page: directory membership never depends on page size.
    for i in range(55):
        add(store, engine, f"unrelated-{i}", ["SHFE.RB.202610.20251001"], source=f"source-{i}")
    tree = {tuple(n["path"]): n["count"] for n in store.hierarchy()}
    assert tree[("CZCE",)] == 2  # multi-contract dataset counted once at the common ancestor
    assert tree[("CZCE", "MA", "contracts", a)] == 2
    assert tree[("CZCE", "MA", "contracts", b)] == 1
    first = store.list(directory=f"CZCE/MA/contracts/{a}", limit=1)
    second = store.list(directory=f"CZCE/MA/contracts/{a}", limit=1, offset=1)
    assert first["total"] == second["total"] == 2
    assert {first["items"][0]["id"], second["items"][0]["id"]} == {"multi", "source-two"}
    assert store.list(directory=f"CZCE/MA/contracts/{b}")["total"] == 1
    assert store.list(directory="CZCE/MA", include_archived=True)["total"] == 3
    assert store.list(directory="CZCE/M")["total"] == 0
    assert store.list(directory="CZCE/MA", source="other")["total"] == 1
