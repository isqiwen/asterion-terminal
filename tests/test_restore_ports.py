import gc
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.recovery import RestoreScope, RestoreStep
from sqlalchemy import Column, Float, MetaData, String, Table, select

from asterion.distribution import restore_inputs
from asterion.identity.backup import SessionReset
from asterion.identity.plugin import plugin as identity
from asterion.platform.store import jobs
from asterion.runtime.restore import isolate_restore

# Owned and created by the Rust entry; restore clears it offline.
sessions = Table(
    "identity_sessions",
    MetaData(),
    Column("digest", String, primary_key=True),
    Column("account_id", String, nullable=False),
    Column("expires", Float, nullable=False),
)


@pytest.fixture
def restore_database():
    engine = create_engine("sqlite://")
    from asterion.platform.communication.events import TABLES

    for table in TABLES:
        table.create(engine)
    jobs.create(engine)
    sessions.create(engine)
    with engine.begin() as conn:
        for state in ("QUEUED", "RUNNING", "SUCCEEDED"):
            conn.execute(
                jobs.insert().values(
                    id=state,
                    command_id=state,
                    kind="fixture",
                    payload={},
                    state=state,
                    token="lease",
                    worker_id="worker",
                    lease_until=100,
                    created_at=1,
                )
            )
        conn.execute(sessions.insert().values(digest="session", account_id="account", expires=100))
    yield engine
    engine.dispose()


def snapshot(engine):
    with engine.connect() as conn:
        return (
            list(conn.execute(select(jobs).order_by(jobs.c.id))),
            list(conn.execute(select(sessions))),
        )


def test_restore_cancels_only_unfinished_tasks_and_expires_session_operation(restore_database):
    retained = []

    def reset(operation):
        retained.append(operation.clear)
        operation.clear()

    plugin = replace(identity, restore=RestoreStep(SessionReset, reset))
    assert isolate_restore(restore_database, (plugin,), restore_inputs) == 2
    rows, session_rows = snapshot(restore_database)
    assert session_rows == []
    for row in rows:
        if row.id == "SUCCEEDED":
            assert row.state == "SUCCEEDED" and row.token == "lease"
        else:
            assert row.state == "CANCELLED"
            assert row.token is row.worker_id is row.lease_until is None
    with pytest.raises(ValueError, match="closed"):
        retained[0]()


@pytest.mark.parametrize(
    "failure", ["after_clear", "skipped", "swallowed", "repeated", "swallowed_repeat"]
)
def test_restore_failure_rolls_back_tasks_and_sessions(restore_database, failure):
    before = snapshot(restore_database)
    retained = []

    def reset(operation):
        retained.append(operation.clear)
        if failure == "skipped":
            return
        if failure == "swallowed":
            try:
                operation.clear()
            except RuntimeError:
                pass
            return
        operation.clear()
        if failure == "swallowed_repeat":
            try:
                operation.clear()
            except ValueError:
                pass
            return
        if failure == "repeated":
            operation.clear()
        raise RuntimeError("domain failed after clear")

    def bind(conn, plugins, scope):
        if failure != "swallowed":
            return restore_inputs(conn, plugins, scope)

        def fail():
            conn.execute(sessions.delete())
            raise RuntimeError("session operation failed")

        return {identity.id: SessionReset(scope.operation(fail))}

    plugin = replace(identity, restore=RestoreStep(SessionReset, reset))
    with pytest.raises((RuntimeError, ValueError)):
        isolate_restore(restore_database, (plugin,), bind)
    assert snapshot(restore_database) == before
    with pytest.raises(ValueError, match="closed"):
        retained[0]()


@pytest.mark.parametrize("invalid", ["missing", "extra", "type"])
def test_restore_checks_all_inputs_before_callbacks(restore_database, invalid):
    before = snapshot(restore_database)
    calls = []
    plugin = replace(identity, restore=RestoreStep(SessionReset, lambda value: calls.append(value)))

    def bind(conn, plugins, scope):
        inputs = restore_inputs(conn, plugins, scope)
        if invalid == "missing":
            inputs.clear()
        elif invalid == "extra":
            inputs["fixture.extra"] = inputs[identity.id]
        else:
            inputs[identity.id] = object()
        return inputs

    with pytest.raises((ValueError, TypeError)):
        isolate_restore(restore_database, (plugin,), bind)
    assert calls == []
    assert snapshot(restore_database) == before


def test_restore_operation_cannot_repeat_or_register_after_close():
    scope = RestoreScope()
    calls = []
    operation = scope.operation(lambda: calls.append("cleared"))
    operation()
    with pytest.raises(ValueError, match="already invoked"):
        operation()
    assert calls == ["cleared"]
    scope.close()
    with pytest.raises(ValueError, match="closed"):
        scope.operation(lambda: None)


def test_native_operation_is_not_constructible_and_owner_drop_revokes_it():
    scope = RestoreScope()
    operation = scope.operation(lambda: None)
    with pytest.raises(TypeError):
        type(operation)()
    assert not hasattr(operation, "complete")
    del scope
    gc.collect()
    with pytest.raises(ValueError, match="closed"):
        operation()


def test_restore_operations_reject_wrong_threads_and_reentrance_without_deadlock():
    scope = RestoreScope()
    calls = []

    def callback():
        calls.append("entered")
        with pytest.raises(ValueError, match="already invoked"):
            operation()

    operation = scope.operation(callback)
    with (
        ThreadPoolExecutor(max_workers=1) as pool,
        pytest.raises(ValueError, match="another thread"),
    ):
        pool.submit(operation).result(timeout=5)
    with pytest.raises(ValueError, match="did not complete"):
        operation()
    with pytest.raises(ValueError, match="did not complete"):
        scope.verify()
    assert calls == ["entered"]
    scope.close()
    with pytest.raises(ValueError, match="closed"):
        scope.operation(lambda: None)


def test_restore_scope_closed_inside_action_cannot_pass_completion():
    scope = RestoreScope()
    operation = scope.operation(scope.close)
    with pytest.raises(ValueError, match="closed"):
        operation()
    with pytest.raises(ValueError, match="closed"):
        scope.verify()


def test_awaitable_restore_result_is_rejected_and_cannot_be_marked_complete():
    scope = RestoreScope()
    calls = []

    async def action():
        calls.append("executed")

    operation = scope.operation(action)
    with pytest.raises(ValueError, match="synchronously"):
        operation()
    assert calls == []
    with pytest.raises(ValueError, match="did not complete"):
        scope.verify()
    scope.close()


def test_plain_restore_return_value_does_not_need_to_be_none():
    scope = RestoreScope()
    operation = scope.operation(lambda: object())
    operation()
    scope.verify()
