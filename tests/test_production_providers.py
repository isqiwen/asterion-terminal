from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from storage_support import raw_engine

from asterion.api.app import create_app
from asterion.data.connections import connections
from asterion.data.providers import builtin_registry
from asterion.platform.config import Settings
from asterion.platform.store import metadata


def test_removed_development_connections_do_not_break_product(tmp_path):
    assert [p.manifest.id for p in builtin_registry().all()] == ["tushare"]
    engine = create_engine(f"sqlite:///{tmp_path}/product.db")
    metadata.create_all(engine)
    with engine.begin() as conn:
        conn.execute(
            connections.insert().values(id="c_" + "a" * 32, provider="synthetic", name="旧开发连接")
        )
    token = "production-registry-test-token"
    with TestClient(
        create_app(Settings(token=token, data_root=tmp_path / "data"), raw_engine(engine)),
        headers={"Authorization": "Bearer " + token},
    ) as client:
        assert [p["id"] for p in client.get("/api/v1/data/providers").json()] == ["tushare"]
        assert client.get("/api/v1/services").status_code == 200
        assert (
            client.post(
                "/api/v1/data/connections", json={"provider": "synthetic", "name": "演示"}
            ).status_code
            == 422
        )
        assert (
            client.post(
                "/api/v1/data/sync",
                json={
                    "command_id": "no-demo",
                    "provider": "synthetic",
                    "dataset": "daily",
                    "exchange": "SIM",
                    "symbol": "DEMO001.SIM",
                    "start": "2024-01-02",
                    "end": "2024-01-05",
                },
            ).status_code
            == 422
        )
        assert client.get("/api/v1/data/catalog").status_code == 200
    engine.dispose()
