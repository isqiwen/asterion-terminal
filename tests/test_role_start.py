"""First-day publication uses real role scopes and refuses late campaign seeds."""

from datetime import datetime
from importlib import import_module

import pytest
from asterion_bindings.database import create_engine
from fastapi.testclient import TestClient
from role_source_support import port
from test_role_sequence import campaign

from asterion.api.app import create_app
from asterion.contract_roles.computed import ComputedSources
from asterion.platform.config import Settings


@pytest.fixture
def initial_api(tmp_path, monkeypatch):
    values, _, first, second, _ = campaign()
    monkeypatch.setattr(
        ComputedSources, "__init__", lambda self, versions: setattr(self, "versions", port(values))
    )
    now = [first.published_at]

    class Clock(datetime):
        @classmethod
        def now(cls, tz=None):
            return now[0]

    monkeypatch.setattr(import_module("asterion.contract_roles.plugin"), "datetime", Clock)
    engine = create_engine(f"sqlite:///{tmp_path}/initial.db")
    settings = Settings(
        token="initial-role-test-token-long-enough", data_root=tmp_path, require_account=False
    )
    try:
        with TestClient(create_app(settings, engine)) as client:
            client.headers["Authorization"] = "Bearer " + settings.token
            client.headers["Authorization"] = "Bearer " + client.scope("roles")
            yield client, values, first, second, now
    finally:
        engine.dispose()


def test_first_publication_is_scoped_and_lost_response_retry_preserves_time(initial_api):
    client, values, first, _, now = initial_api
    root = "/api/v1/contract-roles/computed"
    preview = client.post(root + "/preview", json=first.spec.request.model_dump(mode="json"))
    assert preview.status_code == 200, preview.text
    assert client.get(root).json() == []
    saved = client.post(root + "/start", json=preview.json())
    assert saved.status_code == 200, saved.text
    assert saved.json() == first.model_dump(mode="json")
    product = first.spec.request.product_id
    assert client.get(root, params={"product_id": product}).json() == [saved.json()]
    assert client.get(root, params={"product_id": "DCE.I"}).json() == []
    hierarchy = client.get("/api/v1/contract-roles/hierarchy")
    assert hierarchy.status_code == 200
    assert hierarchy.json() == [
        {"product_id": product, "role": "main", "count": 1},
        {"product_id": product, "role": "secondary", "count": 1},
    ]
    now[0] = now[0].replace(hour=22)
    assert client.post(root + "/start", json=preview.json()).json() == saved.json()
    assert len(client.get(root).json()) == 1
    assert client.get(root + f"/{first.id}/sync-plan").status_code == 200
    # A role-scoped client cannot bypass timely publication via the archive endpoint.
    assert client.post(root, json=preview.json()).status_code == 401
    assert client.post("/api/v1/trading-time", json={}).status_code == 401
    assert client.post("/api/v1/data/sync", json={}).status_code == 401
    values["daily-10-0"]["rows"][0]["oi"] = "999"
    assert client.post(root + "/start", json=preview.json()).status_code == 422
    assert client.get(root).json() == [saved.json()]


@pytest.mark.parametrize("failure", ["late", "multiple_days"])
def test_invalid_seed_does_not_publish(initial_api, failure):
    client, _, first, second, now = initial_api
    if failure == "late":
        now[0] = now[0].replace(hour=22)
    spec = second.spec if failure == "multiple_days" else first.spec
    response = client.post(
        "/api/v1/contract-roles/computed/start", json=spec.model_dump(mode="json")
    )
    assert response.status_code == 422, response.text
    assert client.get("/api/v1/contract-roles/computed").json() == []
    client.headers.pop("Authorization")
    assert (
        client.post(
            "/api/v1/contract-roles/computed/start", json=spec.model_dump(mode="json")
        ).status_code
        == 401
    )


def test_late_archive_cannot_be_reused_as_timely_seed(initial_api):
    client, _, first, _, now = initial_api
    now[0] = now[0].replace(hour=22)
    root = "/api/v1/contract-roles/computed"
    scoped = client.headers["Authorization"]
    client.headers["Authorization"] = "Bearer initial-role-test-token-long-enough"
    archived = client.post(root, json=first.spec.model_dump(mode="json"))
    assert archived.status_code == 200, archived.text
    client.headers["Authorization"] = scoped
    response = client.post(root + "/start", json=first.spec.model_dump(mode="json"))
    assert response.status_code == 422, response.text
    assert client.get(root).json() == [archived.json()]
