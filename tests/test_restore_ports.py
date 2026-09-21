from dataclasses import replace

import pytest
from sqlalchemy import create_engine, select

from asterion.distribution import restore_inputs
from asterion.identity.backup import SessionReset
from asterion.identity.plugin import plugin as identity
from asterion.identity.service import sessions
from asterion.platform.backup import RestoreScope, RestoreStep, isolate_restore
from asterion.platform.store import jobs


@pytest.fixture
def restore_database():
    engine = create_engine("sqlite://")
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


@pytest.mark.parametrize("failure", ["after_clear", "skipped", "swallowed", "repeated"])
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
