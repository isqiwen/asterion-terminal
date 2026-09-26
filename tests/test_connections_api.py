import importlib
import time
from dataclasses import replace

from asterion_bindings.database import create_engine
from connection_fakes import contribution, save_body
from fastapi.testclient import TestClient

from asterion.api.app import create_app
from asterion.platform.config import Settings


def test_plugin_lifecycle_api_unified_connection_and_retained_watchlist(tmp_path, monkeypatch):
    module = importlib.import_module("asterion.connector_ctp.plugin")
    item = contribution("ctp")
    item = replace(
        item, descriptor=item.descriptor.model_copy(update={"owner": "asterion.connector.ctp"})
    )
    monkeypatch.setattr(module, "contribution", lambda: item)
    settings = Settings(
        token="connection-integration-long-token", data_root=tmp_path, require_account=False
    )
    engine = create_engine(f"sqlite:///{tmp_path}/test.db")
    try:
        with TestClient(create_app(settings, engine)) as client:
            assert client.get("/api/v1/connections").status_code == 401
            client.headers["Authorization"] = "Bearer " + settings.token
            body = save_body(connector_id="ctp").model_dump(mode="json")
            body["secrets"]["password"]["value"] = "fixture-password"
            response = client.post("/api/v1/connections", json=body)
            assert response.status_code == 200, response.text
            key = response.json()["connection_id"]
            assert client.get("/api/v1/market/state").status_code == 422
            assert client.post(f"/api/v1/connections/{key}/connect", json={}).status_code == 200
            for _ in range(60):
                snapshot = client.get(f"/api/v1/trading/account?connection_id={key}").json()
                if snapshot["state"] == "ready":
                    break
                time.sleep(0.05)
            assert snapshot["state"] == "ready" and snapshot["positions"] == []
            state = client.get(f"/api/v1/market/state?connection_id={key}").json()
            assert state["state"] == "connected"
            for _ in range(60):
                catalog = client.get(
                    f"/api/v1/market/contracts?connection_id={key}&exchange=SHFE"
                ).json()
                if catalog["state"] == "ready":
                    break
                time.sleep(0.05)
            assert catalog["contracts"][0]["name"] == "黄金2612"
            saved = client.post(
                f"/api/v1/market/watchlist?connection_id={key}",
                json={"subscriptions": [{"exchange": "SHFE", "symbol": "au2612"}]},
            )
            assert saved.status_code == 200, saved.text
            client.post(f"/api/v1/connections/{key}/connect", json={})
            client.post(f"/api/v1/connections/{key}/disconnect", json={})
            assert (
                client.get(f"/api/v1/market/state?connection_id={key}").json()["state"]
                == "disconnected"
            )
            assert (
                client.get(f"/api/v1/trading/account?connection_id={key}").json()["account"] is None
            )
            # Saving a second profile does not silently activate it.
            second = client.post("/api/v1/connections", json=body | {"name": "second"}).json()
            assert client.get("/api/v1/connections").json()["selected_id"] == key
            client.post(f"/api/v1/connections/{key}/connect", json={})
            switched = client.post(f"/api/v1/connections/{second['connection_id']}/select", json={})
            assert switched.status_code == 200
            listing = client.get("/api/v1/connections").json()
            assert listing["active_id"] == listing["selected_id"] == second["connection_id"]
            assert listing["connections"][0]["market"]["state"] == "disconnected"
            assert listing["connections"][0]["account"]["state"] == "disconnected"
            assert client.post(f"/api/v1/connections/{key}/select", json={}).status_code == 200
            assert (
                client.post(
                    f"/api/v1/connections/{key}/delete", json={"expected_revision": 1}
                ).status_code
                == 409
            )
            assert (
                client.post(
                    f"/api/v1/connections/{second['connection_id']}/delete",
                    json={"expected_revision": 1},
                ).status_code
                == 200
            )
            assert (
                client.get(
                    f"/api/v1/trading/account?connection_id={second['connection_id']}"
                ).status_code
                == 409
            )
            assert client.post("/api/v1/trading/order", json={}).status_code == 404
        with TestClient(create_app(settings, engine)) as client:
            client.headers["Authorization"] = "Bearer " + settings.token
            listing = client.get("/api/v1/connections").json()
            assert listing["selected_id"] == key and listing["active_id"] is None
            row = listing["connections"][0]
            assert row["secret_saved"]["password"] and row["market"]["state"] == "disconnected"
            assert client.get(f"/api/v1/market/state?connection_id={key}").json()["configuration"][
                "subscriptions"
            ] == [{"exchange": "SHFE", "symbol": "au2612"}]
    finally:
        engine.dispose()


def test_market_scope_cannot_edit_credentials(tmp_path):
    settings = Settings(
        token="connection-scope-test-token", data_root=tmp_path, require_account=False
    )
    engine = create_engine("sqlite://")
    try:
        with TestClient(create_app(settings, engine)) as client:
            client.headers["Authorization"] = "Bearer " + settings.token
            client.headers["Authorization"] = "Bearer " + client.scope("market")
            assert client.get("/api/v1/connections").status_code == 200
            assert client.post("/api/v1/connections", json={}).status_code == 401
            key = "a" * 32
            assert (
                client.post(
                    f"/api/v1/connections/{key}/delete", json={"expected_revision": 1}
                ).status_code
                == 401
            )
            assert client.post(f"/api/v1/connections/{key}/connect", json={}).status_code == 409
    finally:
        engine.dispose()
