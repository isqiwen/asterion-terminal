import threading
from dataclasses import replace

import pytest
from asterion_bindings.secrets import secret_port
from connection_fakes import blocked_profiles_path, contribution, manager, save_body, source_session

from asterion.connections.public import CREDENTIAL_SCOPE, ConnectorError
from asterion.connections.service import ConnectionService


def test_secret_restart_and_source_isolation(tmp_path):
    service = manager(tmp_path, [contribution(), contribution("another")])
    a = service.save(save_body())
    b = service.save(save_body(connector_id="another", name="另一账户"))
    assert a.connection_id != b.connection_id
    assert "test-secret" not in service.path.read_text()
    assert "test-secret" not in service.snapshot().model_dump_json()
    restored = manager(tmp_path, [contribution(), contribution("another")])
    assert restored.view(a.connection_id).secret_saved == {"password": True}
    restored.connect(a.connection_id)
    assert restored.account(a.connection_id, threading.Event()).connection_id == a.connection_id
    assert restored.channel(a.connection_id, "market").state == "ready"
    assert restored.channel(b.connection_id, "account").state == "disconnected"
    restored.close()


def test_switch_closes_old_source_and_rejects_late_callback(tmp_path):
    service = manager(tmp_path)
    a = service.save(save_body(name="first")).connection_id
    b = service.save(save_body(name="second")).connection_id
    service.connect(a)
    old = source_session(service, a)
    callback = old.emit
    service.select(b)
    assert old.closed and service.channel(a, "market").state == "disconnected"
    callback("connected", None)
    assert service.channel(a, "market").state == "disconnected"
    assert service.channel(a, "account").state == "disconnected"
    assert service.snapshot().selected_id == service.snapshot().active_id == b
    assert service.account(b, threading.Event()).complete
    service.close()
    restored = manager(tmp_path)
    assert restored.snapshot().selected_id == b
    assert restored.snapshot().active_id is None
    assert restored.view(b).secret_saved["password"]
    restored.select(a)
    assert restored.snapshot().active_id is None
    restored.connect(a)
    assert restored.snapshot().active_id == a
    restored.close()


def test_cancelled_account_result_cannot_revive_channel(tmp_path):
    service = manager(tmp_path)
    key = service.save(save_body()).connection_id
    service.connect(key)
    service.connect(key)
    session = source_session(service, key)
    session.release = threading.Event()
    errors = []

    def read():
        try:
            service.account(key, threading.Event())
        except ConnectorError as error:
            errors.append(error)

    thread = threading.Thread(target=read)
    thread.start()
    assert session.entered.wait(2)
    service.disconnect(key)
    session.release.set()
    thread.join(2)
    assert errors and service.channel(key, "account").state == "disconnected"
    service.close()


def test_missing_connector_and_capabilities(tmp_path):
    service = manager(tmp_path)
    key = service.save(save_body()).connection_id
    empty = ConnectionService(tmp_path, secret_port("test-connections-key", CREDENTIAL_SCOPE), {})
    empty.initialize([])
    assert not empty.view(key).available
    with pytest.raises(ConnectorError):
        empty.connect(key)
    limited = manager(tmp_path, [contribution(features=["market_quotes"])])
    limited.connect(key)
    assert limited.channel(key, "account").state == "disconnected"
    limited.close()
    assert not limited.supports(key, "positions")


def test_immutable_identity_revision_and_explicit_secret_actions(tmp_path):
    service = manager(tmp_path)
    first = service.save(save_body())
    args = {"connection_id": first.connection_id, "expected_revision": 1}
    for change in (
        {"config": {"user": "other", "endpoint": "server"}},
        {"expected_revision": 2},
    ):
        with pytest.raises(ValueError):
            service.save(save_body(**(args | change)))
    with pytest.raises(ValueError):
        service.save(
            save_body(
                **args,
                config={"user": "investor", "endpoint": "different"},
                secrets={"password": {"action": "keep"}},
            )
        )
    changed = service.save(save_body(**args, secrets={"password": {"action": "clear"}}))
    assert not changed.secret_saved
    with pytest.raises(ValueError):
        service.connect(first.connection_id)


def test_registration_validation_and_atomic_failure(tmp_path):
    item = contribution()
    base = ConnectionService(
        tmp_path, secret_port("test-connections-key", CREDENTIAL_SCOPE), {"fixture": "test.fixture"}
    )
    with pytest.raises(ValueError):
        base.initialize([lambda: item, lambda: item])
    with pytest.raises(ValueError):
        base.initialize(
            [lambda: replace(item, descriptor=item.descriptor.model_copy(update={"version": 2}))]
        )
    service = manager(tmp_path)
    row = service.save(save_body())
    original = service.path.read_bytes()
    assert service.path.stat().st_mode & 0o777 == 0o600
    with blocked_profiles_path(service), pytest.raises(OSError):
        service.save(
            save_body(connection_id=row.connection_id, expected_revision=1, name="changed")
        )
    assert (
        service.path.read_bytes() == original and service.view(row.connection_id).name == row.name
    )


def test_invalid_storage_preserved(tmp_path):
    path = tmp_path / "connections" / "profiles.json"
    path.parent.mkdir()
    path.write_text('{"version":99}')
    service = manager(tmp_path)
    assert service.snapshot().notice
    with pytest.raises(ValueError):
        service.save(save_body())
    assert path.read_text() == '{"version":99}'


def test_malformed_source_batch_rejected_and_safe_error(tmp_path):
    service = manager(tmp_path)
    key = service.save(save_body()).connection_id
    service.connect(key)
    session = source_session(service, key)
    account = session.account
    session.account = lambda request, cancel: account(request, cancel).model_copy(
        update={"complete": False}
    )
    with pytest.raises(ConnectorError, match="未通过校验"):
        service.account(key, threading.Event())
    assert service.channel(key, "account").state == "error"
    service.close()


def test_ctp_query_serialization_and_credential_release(tmp_path, monkeypatch):
    import importlib
    import time
    from itertools import pairwise

    from connection_fakes import instrument

    from asterion.connections.public import ConnectionProfile, ReadRequest

    module = importlib.import_module("asterion.connector_ctp.plugin")
    profile = ConnectionProfile(
        connection_id="a" * 32,
        connector_id="ctp",
        name="CTP",
        config_revision=1,
        config={
            "broker_id": "9999",
            "user_id": "fixture",
            "app_id": "simnow_client_test",
            "front": "tcp://host:1234",
            "trade_front": "tcp://host:1235",
        },
    )
    session = module.Session(profile, {"password": "private", "auth_code": "public-test"})
    active = [0]
    overlap = []
    starts = []
    errors = []

    def query(*args):
        active[0] += 1
        overlap.append(active[0])
        starts.append(time.monotonic())
        time.sleep(0.05)
        active[0] -= 1
        return [instrument()]

    monkeypatch.setattr(module, "query_instruments", query)

    def read():
        try:
            session.instruments(
                ReadRequest(
                    connection_id=profile.connection_id,
                    generation=1,
                    request_id="a" * 32,
                    started_at=time.time(),
                ),
                threading.Event(),
            )
        except Exception as error:  # noqa: BLE001 - assert every reader failure in the test thread.
            errors.append(error)

    threads = [threading.Thread(target=read) for _ in range(3)]
    try:
        for thread in threads:
            thread.start()
        # Three serialized reads include two mandatory 1.1 s provider intervals;
        # the first join target is not necessarily the first scheduled reader.
        deadline = time.monotonic() + 10
        for thread in threads:
            thread.join(max(0, deadline - time.monotonic()))
        assert not any(thread.is_alive() for thread in threads)
        assert not errors
        assert overlap == [1, 1, 1]
        assert all(right - left >= 1.1 for left, right in pairwise(starts))
    finally:
        # Keep the fake installed until every reader has left the adapter.
        session.close()
        for thread in threads:
            thread.join(2)
    assert session.password == "" and session.configuration.auth_code == ""


def test_concurrent_switches_never_overlap_sessions(tmp_path):
    from connection_fakes import Session

    live = set()
    seen = []
    guard = threading.Lock()

    class Tracked(Session):
        def __init__(self, profile, secrets):
            super().__init__(profile, secrets)
            with guard:
                live.add(profile.connection_id)
                seen.append(len(live))

        def close(self):
            with guard:
                live.discard(self.profile.connection_id)
            super().close()

    service = manager(tmp_path, [replace(contribution(), factory=Tracked)])
    keys = [service.save(save_body(name=str(i))).connection_id for i in range(3)]
    threads = [threading.Thread(target=service.connect, args=(k,)) for k in keys * 3]
    for t in threads:
        t.start()
    for t in threads:
        t.join(5)
        assert not t.is_alive()
    assert max(seen) == 1 and len(live) == 1
    assert service.snapshot().selected_id == service.snapshot().active_id
    service.close()
    assert not live


def test_switch_validation_and_shutdown_failure_do_not_open_new_session(tmp_path):
    service = manager(tmp_path)
    a = service.save(save_body(name="a")).connection_id
    b = service.save(save_body(name="b", secrets={"password": {"action": "clear"}})).connection_id
    service.connect(a)
    old = source_session(service, a)
    with pytest.raises(ValueError):
        service.select(b)
    assert service.snapshot().selected_id == a and not old.closed
    service.save(save_body(connection_id=b, expected_revision=1, name="b"))
    old.close = lambda: (_ for _ in ()).throw(RuntimeError("private-secret"))
    with pytest.raises(ConnectorError, match="尚未完成关闭") as error:
        service.select(b)
    assert "private-secret" not in str(error.value)
    assert service.active_id() == a and source_session(service, b) is None
    old.close = lambda: None
    service.select(b)
    assert service.active_id() == b
    service.close()


def test_failed_new_source_leaves_old_closed_and_configuration_reusable(tmp_path):
    from connection_fakes import Session

    def factory(profile, secrets):
        session = Session(profile, secrets)
        if profile.name == "fails":
            session.start_market = lambda *args: (_ for _ in ()).throw(RuntimeError("secret"))
        return session

    service = manager(tmp_path, [replace(contribution(), factory=factory)])
    a = service.save(save_body(name="works")).connection_id
    b = service.save(save_body(name="fails")).connection_id
    service.connect(a)
    old = source_session(service, a)
    with pytest.raises(ConnectorError, match="启动失败"):
        service.select(b)
    assert old.closed and service.active_id() is None
    assert service.snapshot().selected_id == b and service.view(b).secret_saved["password"]
    service.select(a)
    service.connect(a)
    assert service.active_id() == a
    service.close()


def test_delete_requires_disconnected_current_revision_and_preserves_history(tmp_path):
    service = manager(tmp_path)
    row = service.save(save_body())
    key = row.connection_id
    history = tmp_path / "market" / "connections" / key / "watchlist.json"
    history.parent.mkdir(parents=True)
    history.write_text("retained-business-data")
    service.connect(key)
    callback = source_session(service, key).emit
    with pytest.raises(ValueError, match="先断开"):
        service.delete(key, 1)
    service.disconnect(key)
    edited = service.save(
        save_body(
            connection_id=key,
            expected_revision=1,
            name="renamed",
            secrets={"password": {"action": "keep"}},
        )
    )
    with pytest.raises(ValueError, match="配置已改变"):
        service.delete(key, 1)
    snapshot = service.delete(key, edited.config_revision)
    assert not snapshot.connections and snapshot.selected_id is None and snapshot.active_id is None
    assert key not in service.path.read_text()
    assert history.read_text() == "retained-business-data"
    callback("connected", None)
    assert not manager(tmp_path).profiles()
    with pytest.raises(ValueError, match="不存在"):
        service.connect(key)


def test_delete_other_profile_preserves_active_and_failed_write_preserves_credentials(tmp_path):
    service = manager(tmp_path)
    a = service.save(save_body(name="active")).connection_id
    b = service.save(save_body(name="inactive")).connection_id
    service.connect(a)
    original = service.path.read_bytes()
    with blocked_profiles_path(service), pytest.raises(OSError):
        service.delete(b, 1)
    assert service.path.read_bytes() == original
    assert service.view(b).secret_saved["password"]
    service.delete(b, 1)
    assert service.snapshot().active_id == service.snapshot().selected_id == a
    service.close()


def test_missing_connector_profile_can_be_deleted(tmp_path):
    service = manager(tmp_path)
    key = service.save(save_body()).connection_id
    empty = ConnectionService(tmp_path, secret_port("test-connections-key", CREDENTIAL_SCOPE), {})
    empty.initialize([])
    assert not empty.view(key).available
    assert not empty.delete(key, 1).connections


def test_cross_thread_synchronous_sdk_callback_can_read_snapshot(tmp_path):
    from connection_fakes import Session

    class Threaded(Session):
        def start_market(self, subscriptions, emit):
            self.emit = emit
            callback = threading.Thread(target=lambda: emit("connected", None))
            callback.start()
            callback.join(2)
            assert not callback.is_alive(), "snapshot must not wait for the lifecycle gate"

    service = manager(tmp_path, [replace(contribution(), factory=Threaded)])
    key = service.save(save_body()).connection_id
    seen = []
    service.listen(lambda *_: seen.append(service.snapshot().active_id))
    service.connect(key)
    assert seen == [key]
    service.close()


def test_reconnect_advances_market_epoch_and_queries_reject_wrong_request_id(tmp_path):
    service = manager(tmp_path)
    key = service.save(save_body()).connection_id
    service.connect(key)
    session = source_session(service, key)
    initial = service.channel(key, "market").generation
    session.emit("reconnecting", None)
    session.emit("connected", None)
    assert service.channel(key, "market").generation == initial + 1
    assert service.channel(key, "account").generation == initial
    account = session.account
    session.account = lambda request, cancel: account(request, cancel).model_copy(
        update={"request_id": "f" * 32}
    )
    with pytest.raises(ConnectorError, match="未通过校验"):
        service.account(key, threading.Event())
    assert service.channel(key, "account").state == "error"
    service.close()


def test_awaitable_source_lifecycle_is_rejected_without_releasing_owner(tmp_path):
    service = manager(tmp_path)
    key = service.save(save_body()).connection_id
    service.connect(key)
    session = source_session(service, key)
    close = session.close

    async def pending():
        raise AssertionError("synchronous host must not run a coroutine")

    session.close = pending
    with pytest.raises(ConnectorError, match="尚未完成关闭"):
        service.disconnect(key)
    assert service.active_id() == key
    session.close = close
    service.close()
