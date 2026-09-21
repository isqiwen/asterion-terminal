"""Execution diagnostics stay bounded, shared across processes and free of call data."""

import json
from concurrent.futures import ThreadPoolExecutor

import pytest
from extension_support import package_content
from fastapi.testclient import TestClient
from sqlalchemy import create_engine

from asterion.api.app import create_app
from asterion.distribution import extension_packages
from asterion.platform.config import Settings
from asterion.platform.extensions.diagnostics import Observation, recent
from asterion.platform.extensions.process import PackageSession, call_package


def test_failure_categories_never_persist_request_or_stderr(tmp_path):
    packages = extension_packages(tmp_path)
    record = packages.install(
        package_content(
            script="import sys,time; print('private-stderr',file=sys.stderr); time.sleep(10)"
        )
    )
    _, path = packages.resolve(record["digest"])
    with pytest.raises(ValueError, match="超时"):
        call_package(path, "probe", {"token": "private-parameter"}, timeout=0.1)
    row = recent(packages.root, record["digest"])[0]
    assert row["code"] == "timeout" and row["phase"] == "probe"
    assert row["duration_ms"] >= 100 and row["calls"] == 1
    assert "private" not in json.dumps(row)
    assert b"private" not in (packages.root / "diagnostics.sqlite").read_bytes()
    with pytest.raises(ValueError, match="停用"):
        call_package(path, "probe", {}, authorized=lambda: False)
    assert recent(packages.root, record["digest"])[0]["code"] == "revoked"
    session = PackageSession(path, authorized=lambda: True, timeout=0)
    with pytest.raises(ValueError):
        session.call("strategy.open", {})
    session.close()
    rows = recent(packages.root, record["digest"])
    assert len(rows) == 3 and rows[0]["code"] == "timeout"
    assert rows[0]["phase"] == "strategy.open"


def test_concurrent_observations_are_bounded(tmp_path):
    path = tmp_path / "objects" / ("d" * 64)

    def record(_):
        observation = Observation(path)
        for _ in range(10):
            observation.call("strategy.close")
        observation.finish()
        observation.finish("protocol")

    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(record, range(215)))
    rows = recent(tmp_path, path.name)
    assert len(rows) == 30
    assert all(row["code"] == "success" and row["calls"] == 10 for row in rows)
    import sqlite3

    with sqlite3.connect(tmp_path / "diagnostics.sqlite") as conn:
        assert conn.execute("SELECT COUNT(*) FROM executions").fetchone()[0] == 200


def test_diagnostics_endpoint_requires_access_and_current_artifact(tmp_path):
    packages = extension_packages(tmp_path)
    record = packages.install(package_content())
    _, path = packages.resolve(record["digest"])
    assert call_package(path, "probe", {})
    settings = Settings(token="diagnostics-test-token-at-least-24", data_root=tmp_path)
    engine = create_engine(f"sqlite:///{tmp_path}/test.db")
    try:
        with TestClient(create_app(settings, engine)) as client:
            endpoint = "/api/v1/extensions/test.calendar/diagnostics"
            assert client.get(endpoint).status_code == 401
            client.headers["Authorization"] = "Bearer " + settings.token
            report = client.get(endpoint).json()
            assert report["digest"] == record["digest"]
            assert report["items"][0]["code"] == "success"
            assert client.get("/api/v1/extensions/missing.plugin/diagnostics").status_code == 404
    finally:
        engine.dispose()
