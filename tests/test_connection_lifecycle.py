import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.task_repository import Conflict
from configuration_support import set_token
from credential_helpers import provider_secrets
from storage_support import data_store, domain_tasks, scheduler

from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.sync import DataSync
from asterion.platform.store import metadata


@pytest.fixture
def sync(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/lifecycle.db")
    metadata.create_all(engine)
    (tmp_path / "data").mkdir()
    service = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path / "data",
        provider_secrets("lifecycle-test-key-long-enough", tmp_path / "data"),
    )
    set_token(service, "tushare", "test-only-token")
    yield service
    engine.dispose()


def req(command):
    return SyncRequest(command_id=command, provider="tushare", dataset="contracts", exchange="SHFE")


def test_default_connection_can_rename_disable_archive_and_restore(sync):
    first = sync.submit(req("existing"))
    changed = sync.sources.update("tushare", 0, "研究连接", "disabled")
    assert changed["revision"] == 1
    assert sync.providers()[0]["name"] == "研究连接"
    with pytest.raises(ProviderError, match="停用"):
        sync.submit(req("new"))
    assert sync.submit(req("existing"))["id"] == first["id"]
    with pytest.raises(Conflict):
        sync.sources.update("tushare", 0, "过期修改", "enabled")
    sync.sources.update("tushare", 1, "研究连接", "archived")
    assert scheduler(sync.engine).list()[0]["state"] == "QUEUED"
    sync.sources.update("tushare", 2, "研究连接", "enabled")
    assert sync.submit(req("after-restore"))["state"] == "QUEUED"


def test_instance_lifecycle_is_independent(sync):
    instance = sync.sources.create("tushare", "第二连接")
    sync.sources.update(instance["id"], 0, "归档连接", "archived")
    assert sync.sources.connection("tushare")["state"] == "enabled"
    assert (
        next(p for p in sync.providers() if p["id"] == instance["id"])["lifecycle"]["state"]
        == "archived"
    )
