"""Exercise native task decisions through the actual transactional SQL driver."""

import os
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from queue import Queue
from uuid import uuid4

import pytest
from asterion_bindings.database import create_engine, native_connection
from asterion_bindings.resource import Resource
from asterion_bindings.task_models import TASK_CHANGED
from asterion_bindings.task_repository import Conflict, Tasks
from asterion_bindings.tasks import ExecutionContext
from sqlalchemy import select, text

from asterion.platform.communication.events import events, heads
from asterion.platform.communication.schema import initialize_core
from asterion.platform.store import jobs


@pytest.fixture
def scheduler():
    engine = create_engine("sqlite://")
    initialize_core(engine)
    try:
        yield Tasks(engine, lease_seconds=60)
    finally:
        engine.dispose()


def test_exact_expiry_reclaim_fences_old_worker_and_does_not_shorten_lease(scheduler, monkeypatch):
    monkeypatch.setattr("asterion_bindings.task_repository.time.time", lambda: 100.0)
    scheduler.submit("command", "fixture.read", {})
    old = scheduler.claim("first")
    monkeypatch.setattr("asterion_bindings.task_repository.time.time", lambda: 50.0)
    scheduler.heartbeat(old["id"], old["token"])
    assert scheduler.get(old["id"])["lease_until"] == old["lease_until"]
    monkeypatch.setattr("asterion_bindings.task_repository.time.time", lambda: old["lease_until"])
    with pytest.raises(Conflict, match="Lease expired"):
        scheduler.heartbeat(old["id"], old["token"])
    new = scheduler.claim("second")
    assert new["attempt"] == 2 and new["token"] != old["token"]
    with pytest.raises(Conflict, match="Lease expired"), scheduler.engine.begin() as conn:
        scheduler.complete(conn, old["id"], old["token"], {"old": True})
    with scheduler.engine.begin() as conn:
        scheduler.complete(conn, new["id"], new["token"], {"new": True})
    assert scheduler.get(new["id"])["result"] == {"new": True}
    assert scheduler.claim("third") is None


def test_native_completion_rechecks_current_lease_before_write_or_event(scheduler):
    scheduler.submit("command", "fixture.read", {})
    job = scheduler.claim("worker")
    with scheduler.engine.begin() as conn:
        conn.execute(jobs.update().where(jobs.c.id == job["id"]).values(token="superseded"))
        before = list(conn.execute(select(events)))
    with pytest.raises(Conflict, match="Lease expired"), scheduler.engine.begin() as conn:
        scheduler.complete(conn, job["id"], job["token"], {})
    with scheduler.engine.connect() as conn:
        assert list(conn.execute(select(events))) == before
        assert conn.execute(select(jobs.c.state)).scalar_one() == "RUNNING"


def test_event_failure_rolls_back_native_transition(scheduler):
    scheduler.submit("command", "fixture.read", {})
    job = scheduler.claim("worker")
    with scheduler.engine.begin() as conn:
        conn.execute(
            heads.update().where(heads.c.topic == TASK_CHANGED.id).values(sequence=2**63 - 1)
        )
    with pytest.raises(Conflict), scheduler.engine.begin() as conn:
        scheduler.complete(conn, job["id"], job["token"], {"published": True})
    assert scheduler.get(job["id"])["state"] == "RUNNING"
    assert scheduler.get(job["id"])["result"] is None


def test_progress_noop_and_heartbeat_do_not_append_events(scheduler):
    scheduler.submit("command", "fixture.read", {})
    job = scheduler.claim("worker")
    with scheduler.engine.begin() as conn:
        scheduler.progress(conn, job["id"], job["token"], {"count": 1})
        scheduler.progress(conn, job["id"], job["token"], {"count": 1})
    scheduler.heartbeat(job["id"], job["token"])
    with scheduler.engine.connect() as conn:
        assert len(list(conn.execute(select(events).where(events.c.topic == TASK_CHANGED.id)))) == 3
    scheduler.cancel(job["id"])
    with pytest.raises(Conflict):
        scheduler.fail(job["id"], job["token"], "late failure")
    assert scheduler.get(job["id"])["state"] == "CANCELLED"


def test_idempotence_never_equates_a_boolean_with_a_number(scheduler):
    scheduler.submit("command", "fixture.read", {"value": True})
    with pytest.raises(Conflict, match="different input"):
        scheduler.submit("command", "fixture.read", {"value": 1})
    assert scheduler.get(scheduler.list()[0]["id"])["state"] == "QUEUED"


def test_resource_grants_and_extracted_callbacks_expire_together():
    @dataclass(frozen=True)
    class Port:
        run: object

    class Catalog:
        def list(self):
            return ["entry"]

    port = Resource("fixture.port", Port)
    catalog = Resource("fixture.catalog", Catalog)
    with pytest.raises(ValueError, match="do not match"):
        ExecutionContext((port,), {})
    with pytest.raises(ValueError, match="Duplicate"):
        ExecutionContext((port, port), {port: Port(lambda: 1)})
    with pytest.raises(TypeError, match="Invalid execution resource"):
        ExecutionContext((port,), {port: Catalog()})
    context = ExecutionContext((port, catalog), {port: Port(lambda: 1), catalog: Catalog()})
    callback = context.resource(port).run
    listing = context.resource(catalog).list
    assert callback() == 1
    assert listing() == ["entry"]
    with pytest.raises(ValueError, match="not granted"):
        context.resource(Resource("fixture.other", Port))
    context.close()
    for action in [callback, listing, lambda: context.resource(port)]:
        with pytest.raises(ValueError, match="closed"):
            action()


def test_task_driver_rejects_wrong_or_missing_transaction_before_writing(scheduler):
    with (
        scheduler.engine.connect() as conn,
        pytest.raises(ValueError, match="database transaction"),
    ):
        scheduler.submit_batch(conn, [("command", "fixture.read", {})])
    other = create_engine("sqlite://")
    initialize_core(other)
    try:
        with other.begin() as conn:
            with pytest.raises(ValueError, match="database transaction"):
                scheduler.submit_batch(conn, [("command", "fixture.read", {})])
            assert conn.execute(select(jobs)).first() is None
    finally:
        other.dispose()


def test_task_binding_rejects_ambiguous_json_without_creating_state(scheduler):
    with scheduler.transaction() as conn, pytest.raises(Conflict, match="Invalid task operation"):
        scheduler._repository.run(
            native_connection(conn), '{"op":"list","op":"isolate","reason":"x"}', "{}"
        )
    assert scheduler.list() == []


@pytest.mark.skipif(
    not os.environ.get("ASTERION_TEST_DATABASE_URL"), reason="Isolated PostgreSQL URL required"
)
def test_completion_waiting_for_row_lock_cannot_use_an_expired_lease():
    url = os.environ["ASTERION_TEST_DATABASE_URL"]
    admin = create_engine(url)
    schema = "task_expiry_" + uuid4().hex
    with admin.begin() as conn:
        conn.execute(text(f"CREATE SCHEMA {schema}"))
    engine = create_engine(url, connect_args={"options": f"-c search_path={schema}"})
    try:
        initialize_core(engine)
        tasks = Tasks(engine)
        tasks.submit("command", "fixture.read", {})
        job = tasks.claim("worker")
        expires = time.time() + 2
        with engine.begin() as conn:
            conn.execute(jobs.update().where(jobs.c.id == job["id"]).values(lease_until=expires))
            before = list(conn.execute(select(events)))
        pids = Queue()

        def complete():
            with engine.begin() as conn:
                pids.put(conn.execute(text("SELECT pg_backend_pid()")).scalar_one())
                tasks.complete(conn, job["id"], job["token"], {"late": True})

        with ThreadPoolExecutor(max_workers=1) as pool:
            with engine.begin() as locked:
                locked.execute(select(jobs.c.id).where(jobs.c.id == job["id"]).with_for_update())
                future = pool.submit(complete)
                pid = pids.get(timeout=5)
                while True:
                    with admin.connect() as observer:
                        waiting = observer.execute(
                            text("SELECT wait_event_type FROM pg_stat_activity WHERE pid=:pid"),
                            {"pid": pid},
                        ).scalar_one()
                    if waiting == "Lock":
                        break
                    assert time.time() < expires, "Completion did not reach the row lock in time"
                    time.sleep(0.01)
                assert not future.done()
                time.sleep(max(0, expires - time.time()) + 0.05)
            with pytest.raises(Conflict, match="Lease expired"):
                future.result(timeout=5)
        assert tasks.get(job["id"])["state"] == "RUNNING"
        assert tasks.get(job["id"])["result"] is None
        with engine.connect() as conn:
            assert list(conn.execute(select(events))) == before
    finally:
        engine.dispose()
        with admin.begin() as conn:
            conn.execute(text(f"DROP SCHEMA {schema} CASCADE"))
        admin.dispose()
