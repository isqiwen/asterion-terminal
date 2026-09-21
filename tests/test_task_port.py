import pytest
from sqlalchemy import create_engine
from storage_support import scheduler

from asterion.platform.storage import Storage
from asterion.platform.store import jobs
from asterion.platform.task_port import task_port
from asterion.platform.tasks.service import Conflict


def test_scoped_task_port_enforces_kind_and_retains_real_lease_validation():
    engine = create_engine("sqlite://")
    jobs.create(engine)
    tasks = scheduler(engine)
    port = task_port(tasks, frozenset({"fixture.read"}))
    try:
        with pytest.raises(ValueError, match="not granted"):
            port.submit("other", "fixture.write", {})
        with Storage(engine, ()).begin() as conn, pytest.raises(ValueError, match="not granted"):
            port.submit_batch(
                conn, [("first", "fixture.read", {}), ("second", "fixture.write", {})]
            )
        assert tasks.list() == []
        submitted = port.submit("allowed", "fixture.read", {"input": 1})
        assert port.submit("allowed", "fixture.read", {"input": 1})["id"] == submitted["id"]
        claimed = tasks.claim("worker")
        with Storage(engine, ()).begin() as conn:
            assert (
                port.require_lease(conn, claimed["id"], claimed["token"])["kind"] == "fixture.read"
            )
            with pytest.raises(Conflict):
                port.require_lease(conn, claimed["id"], "incorrect")
        tasks.submit("different", "fixture.write", {})
        other = tasks.claim("worker")
        with Storage(engine, ()).begin() as conn, pytest.raises(ValueError, match="not granted"):
            port.require_lease(conn, other["id"], other["token"])
    finally:
        engine.dispose()
