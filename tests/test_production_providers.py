from asterion_bindings.database import create_engine
from fastapi.testclient import TestClient
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
        assert client.get("/api/v1/services").status_code == 200
    from storage_support import data_store

    from asterion.data.library import DataLibrary

    assert DataLibrary(data_store(engine), tmp_path / "data").list()["total"] == 0
    engine.dispose()
