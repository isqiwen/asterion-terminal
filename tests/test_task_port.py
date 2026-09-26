import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.storage import Storage
from asterion_bindings.task_repository import Conflict, task_port
from storage_support import scheduler

from asterion.platform.communication.schema import initialize_core


def test_scoped_task_port_enforces_kind_and_retains_real_lease_validation():
    engine = create_engine("sqlite://")
    initialize_core(engine)
    tasks = scheduler(engine)
    port = task_port(tasks, frozenset({"fixture.read"}))
    try:
        with pytest.raises(ValueError, match="not granted"):
            port.submit("other", "fixture.write", {})
        with pytest.raises(ValueError, match="not granted"), Storage(engine, ()).begin() as conn:
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
        with pytest.raises(Conflict), Storage(engine, ()).begin() as conn:
            port.require_lease(conn, claimed["id"], "incorrect")
        tasks.submit("different", "fixture.write", {})
        other = tasks.claim("worker")
        with pytest.raises(ValueError, match="not granted"), Storage(engine, ()).begin() as conn:
            port.require_lease(conn, other["id"], other["token"])
    finally:
        engine.dispose()
