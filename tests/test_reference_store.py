from copy import deepcopy

import pytest
from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.catalog import ReferenceCatalog
from asterion_bindings.database import create_engine
from asterion_bindings.files import read_files
from fastapi.testclient import TestClient
from sqlalchemy import select
from storage_support import data_store, raw_engine
from test_reference import fixture, query

from asterion.api.app import create_app
from asterion.data.backup import load_evidence, validate_backup
from asterion.data.library import versions
from asterion.data.reference_store import ReferenceStore, releases
from asterion.platform.config import Settings


def test_publication_is_immutable_idempotent_and_survives_reopen(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/reference.db")
    store = ReferenceStore(data_store(engine))
    original = store.publish(ReferenceCatalog.model_validate(fixture()))
    assert store.publish(original.catalog) == original
    changed = fixture()
    changed["contracts"][0]["last_delivery_on"] = "2026-10-20"
    revised = store.publish(ReferenceCatalog.model_validate(changed))
    assert revised.id != original.id
    reopened = ReferenceStore(data_store(engine))
    assert reopened.get(original.id) == original
    assert len(reopened.list()) == 2
    assert len(reopened.list(1, 1)) == 1


def test_reference_api_auth_validation_and_version_lookup(tmp_path):
    settings = Settings(token="reference-test-secret-123456789", data_root=tmp_path)
    app = create_app(settings, raw_engine(create_engine(f"sqlite:///{tmp_path}/api.db")))
    client = TestClient(app)
    endpoint = "/api/v1/reference/releases"
    assert client.get(endpoint).status_code == 401
    client.headers["Authorization"] = "Bearer reference-test-secret-123456789"
    assert client.get(endpoint).json() == []
    bad = fixture()
    bad["contracts"][0]["product_id"] = "DCE.i"
    assert client.post(endpoint, json=bad).status_code == 422
    result = client.post(endpoint, json=fixture())
    assert result.status_code == 200
    release = result.json()
    assert client.post(endpoint, json=fixture()).json()["id"] == release["id"]
    assert client.get(f"{endpoint}/{release['id']}").json() == release
    resolution = client.post(
        f"{endpoint}/{release['id']}/resolve", json=query().model_dump(mode="json")
    )
    assert resolution.status_code == 200
    assert resolution.json()["contract"]["id"] == "SHFE.RB.202610.20251001"
    assert (
        client.post(
            f"{endpoint}/{release['id']}/resolve",
            json=query(day="2027-01-01").model_dump(mode="json"),
        ).status_code
        == 422
    )
    assert client.get(f"{endpoint}/missing").status_code == 404
    assert client.get(endpoint + "?limit=101").status_code == 422
    assert len(client.get(endpoint).json()) == 1


def test_native_reference_requires_account(tmp_path):
    app = create_app(
        Settings(token="test-token-12345678901234567890", require_account=True, data_root=tmp_path),
        raw_engine(create_engine(f"sqlite:///{tmp_path}/protected.db")),
    )
    client = TestClient(app, headers={"Authorization": "Bearer test-token-12345678901234567890"})
    assert client.get("/api/v1/reference/releases").status_code == 401
    assert client.post("/api/v1/reference/releases", json=fixture()).status_code == 401
    assert (
        client.post(
            "/api/v1/reference/releases/missing/resolve", json=query().model_dump(mode="json")
        ).status_code
        == 401
    )


@pytest.mark.parametrize("damage", ["content", "missing", "unknown"])
def test_damaged_catalog_rejected_on_read_list_and_restore_without_rewrite(tmp_path, damage):
    storage = data_store(create_engine(f"sqlite:///{tmp_path}/catalog.db"))
    store = ReferenceStore(storage)
    release = store.publish(ReferenceCatalog.model_validate(fixture()))
    from asterion.data.catalog import snapshots
    from asterion.platform.store import jobs

    jobs.create(raw_engine(storage), checkfirst=True)
    storage.initialize(versions, snapshots)
    from asterion.data.coverage import reports
    from asterion.data.preparation import batches

    storage.initialize(batches)
    storage.initialize(reports)
    with raw_engine(storage).connect() as conn:
        evidence = load_evidence(
            conn,
            ArtifactStore(tmp_path, read_only=True),
            read_files(tmp_path),
            lambda content: content,
        )
    assert validate_backup(evidence)["reference_releases"] == 1
    payload = deepcopy(release.catalog.model_dump(mode="json"))
    if damage == "content":
        payload["contracts"][0]["last_delivery_on"] = "2026-10-20"
    elif damage == "missing":
        payload.pop("schema_version")
    else:
        payload["unknown"] = []
    with storage.begin() as conn:
        conn.execute(releases.update().where(releases.c.id == release.id).values(catalog=payload))
    for read in (lambda: store.get(release.id), store.list):
        with pytest.raises(ValueError):
            read()
    # Exercise the same domain input assembled for backup verification.
    with raw_engine(storage).connect() as conn:
        evidence = load_evidence(
            conn,
            ArtifactStore(tmp_path, read_only=True),
            read_files(tmp_path),
            lambda content: content,
        )
    with pytest.raises(ValueError):
        validate_backup(evidence)
    with storage.connect() as conn:
        assert conn.execute(select(releases.c.catalog)).scalar_one() == payload
