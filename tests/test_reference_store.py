from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from test_reference import fixture

from asterion.api.app import create_app
from asterion.data.reference import ReferenceCatalog
from asterion.data.reference_store import ReferenceStore
from asterion.platform.config import Settings


def test_publication_is_immutable_idempotent_and_survives_reopen(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/reference.db")
    store = ReferenceStore(engine)
    original = store.publish(ReferenceCatalog.model_validate(fixture()))
    assert store.publish(original.catalog) == original
    changed = fixture()
    changed["contracts"][0]["tick_size"] = "2"
    revised = store.publish(ReferenceCatalog.model_validate(changed))
    assert revised.id != original.id
    reopened = ReferenceStore(engine)
    assert reopened.get(original.id) == original
    assert len(reopened.list()) == 2
    assert len(reopened.list(1, 1)) == 1


def test_reference_api_auth_validation_and_version_lookup(tmp_path):
    settings = Settings(token="reference-test-secret-123456789", data_root=tmp_path)
    app = create_app(settings, create_engine(f"sqlite:///{tmp_path}/api.db"))
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
    assert client.get(f"{endpoint}/missing").status_code == 404
    assert client.get(endpoint + "?limit=101").status_code == 422
    assert len(client.get(endpoint).json()) == 1


def test_native_reference_requires_account(tmp_path):
    app = create_app(
        Settings(token="test-token-12345678901234567890", require_account=True, data_root=tmp_path),
        create_engine(f"sqlite:///{tmp_path}/protected.db"),
    )
    client = TestClient(app, headers={"Authorization": "Bearer test-token-12345678901234567890"})
    assert client.get("/api/v1/reference/releases").status_code == 401
    assert client.post("/api/v1/reference/releases", json=fixture()).status_code == 401
