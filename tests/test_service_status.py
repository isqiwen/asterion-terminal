import json
import time
from unittest.mock import Mock

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from sqlalchemy.exc import OperationalError
from storage_support import raw_engine

from asterion.api.app import create_app
from asterion.platform.config import Settings
from asterion.platform.diagnostics import local_services
from asterion.platform.store import metadata


@pytest.mark.parametrize(
    "value,state",
    [
        ({"worker": "running", "observed_at": "fresh"}, "ready"),
        ({"worker": "stopped", "observed_at": "fresh"}, "unavailable"),
        ({"worker": "running", "observed_at": 0}, "unknown"),
        ({"worker": "running", "observed_at": "bad"}, "unknown"),
        ({}, "unknown"),
    ],
)
def test_worker_requires_fresh_observation(tmp_path, value, state):
    value = dict(value)
    if value.get("observed_at") == "fresh":
        value["observed_at"] = time.time()
    (tmp_path / "runtime-status.json").write_text(json.dumps(value))
    engine = create_engine("sqlite://")
    assert local_services(engine, tmp_path / "data")[2].state == state
    engine.dispose()


def test_database_failure_is_reported_without_connection_details(tmp_path):
    engine = Mock()
    engine.connect.side_effect = OperationalError("secret database URL", {}, Exception("password"))
    states = local_services(engine, tmp_path / "data")
    assert states[0].state == "ready"
    assert states[1].state == "unavailable"
    assert states[2].state == "unknown"
    assert "password" not in str(states)


def test_status_endpoint_auth_and_provider_semantics(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/test.db")
    metadata.create_all(engine)
    settings = Settings(
        token="service-test-token-long-enough", data_root=tmp_path / "data", require_account=False
    )
    with TestClient(create_app(settings, raw_engine(engine))) as client:
        assert client.get("/api/v1/services").status_code == 401
        response = client.get(
            "/api/v1/services", headers={"Authorization": "Bearer service-test-token-long-enough"}
        )
        assert response.status_code == 200
        services = {s["id"]: s for s in response.json()["services"]}
        assert services["provider:tushare"]["state"] == "unconfigured"
        assert "provider:synthetic" not in services
        assert services["trading"]["state"] == "unavailable"
    settings.require_account = True
    with TestClient(create_app(settings, raw_engine(engine))) as client:
        assert (
            client.get(
                "/api/v1/services",
                headers={"Authorization": "Bearer service-test-token-long-enough"},
            ).status_code
            == 401
        )
    engine.dispose()
