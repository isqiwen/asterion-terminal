import pytest
from configuration_support import set_token
from credential_helpers import provider_secrets
from pydantic import SecretStr
from sqlalchemy import create_engine
from storage_support import data_store, domain_tasks, scheduler

from asterion.data.configuration import ConfigurationUpdate
from asterion.data.connections import ConnectionUpdate, NewConnection
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.data.sync import DataSync
from asterion.platform.store import metadata
from asterion.platform.tasks.service import Conflict


@pytest.fixture
def sync(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/lifecycle.db")
    metadata.create_all(engine)
    service = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path / "data",
        provider_secrets("lifecycle-test-key-long-enough"),
    )
    set_token(service, "tushare", "test-only-token")
    yield service
    engine.dispose()


def req(command):
    return SyncRequest(command_id=command, provider="tushare", dataset="contracts", exchange="SHFE")


def test_default_connection_can_rename_disable_archive_and_restore(sync):
    first = sync.submit(req("existing"))
    changed = sync.connections.update(
        "tushare", ConnectionUpdate(expected_revision=0, name="研究连接", state="disabled")
    )
    assert changed.revision == 1
    assert sync.providers()[0]["name"] == "研究连接"
    with pytest.raises(ProviderError, match="停用"):
        sync.submit(req("new"))
    assert sync.submit(req("existing"))["id"] == first["id"]
    with pytest.raises(Conflict):
        sync.connections.update(
            "tushare", ConnectionUpdate(expected_revision=0, name="过期修改", state="enabled")
        )
    sync.connections.update(
        "tushare", ConnectionUpdate(expected_revision=1, name="研究连接", state="archived")
    )
    assert scheduler(sync.engine).list()[0]["state"] == "QUEUED"
    sync.connections.update(
        "tushare", ConnectionUpdate(expected_revision=2, name="研究连接", state="enabled")
    )
    assert sync.submit(req("after-restore"))["state"] == "QUEUED"


def test_saved_verification_is_versioned_and_drafts_do_not_update_it(sync, monkeypatch):
    monkeypatch.setattr(Tushare, "probe", lambda self, values: "success")
    assert sync.configuration.verification("tushare").status == "never"
    sync.configuration.check("tushare", ConfigurationUpdate(expected_revision=1))
    assert sync.configuration.verification("tushare").status == "never"
    verified = sync.configuration.verify_saved("tushare")
    assert verified.status == "verified" and verified.checked_at
    sync.configuration.apply(
        "tushare",
        ConfigurationUpdate(expected_revision=1, secrets={"token": SecretStr("different-token")}),
    )
    assert sync.configuration.verification("tushare").status == "stale"

    def fail(*_):
        raise ProviderError("sensitive-token-must-not-be-persisted")

    monkeypatch.setattr(Tushare, "probe", fail)
    failure = sync.configuration.verify_saved("tushare")
    assert failure.status == "failed"
    assert "sensitive" not in failure.message


def test_configuration_changed_during_probe_is_not_marked_verified(sync, monkeypatch):
    def probe(*_):
        sync.configuration.apply(
            "tushare",
            ConfigurationUpdate(expected_revision=1, secrets={"token": SecretStr("new-token")}),
        )
        return "success"

    monkeypatch.setattr(Tushare, "probe", probe)
    assert sync.configuration.verify_saved("tushare").status == "stale"


def test_instance_lifecycle_is_independent(sync):
    instance = sync.connections.create(NewConnection(provider="tushare", name="第二连接"))
    sync.connections.update(
        instance["id"], ConnectionUpdate(expected_revision=0, name="归档连接", state="archived")
    )
    assert sync.connections.state("tushare").state == "enabled"
    assert (
        next(p for p in sync.providers() if p["id"] == instance["id"])["lifecycle"]["state"]
        == "archived"
    )
