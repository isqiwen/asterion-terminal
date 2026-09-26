"""Shared wire fixtures and failure-oriented tests for core communication."""

import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest
from asterion_bindings.communication import (
    activate,
    call,
    context,
    current,
    loads,
    reply,
    result,
    validate,
)
from asterion_bindings.database import create_engine
from asterion_bindings.events import Topic
from asterion_bindings.storage import Storage, initialize_schema
from asterion_bindings.task_models import TASK_CHANGED
from asterion_bindings.task_repository import Tasks
from fastapi.testclient import TestClient
from pydantic import BaseModel
from sqlalchemy import Column, Integer, MetaData, Table, select

from asterion.platform.communication.events import TABLES, EventJournal
from asterion.platform.store import jobs

CASES = json.loads((Path(__file__).parents[1] / "contracts/communication-cases.json").read_text())


@pytest.mark.parametrize("case", CASES, ids=lambda c: c["name"])
def test_wire_shared_cases(case):
    if case["valid"]:
        validate(case["type"], case["value"])
    else:
        with pytest.raises(ValueError):
            validate(case["type"], case["value"])


@pytest.mark.parametrize(
    "payload",
    [
        {1: "not a JSON key"},
        {"tuple": (1, 2)},
        {"value": float("nan")},
        {"value": float("inf")},
        {"value": 10**100},
        {"value": object()},
    ],
)
def test_native_value_conversion_rejects_lossy_python_representations(payload):
    with pytest.raises(ValueError):
        call("fixture.strict", payload)


def test_wire_validation_preserves_identity_and_rejects_cyclic_values():
    value = context()
    assert validate("Context", value) is value
    cycle = {}
    cycle["self"] = cycle
    with pytest.raises(ValueError, match="Cyclic"):
        call("fixture.cycle", cycle)


@pytest.mark.parametrize(
    "raw", ['{"a":1,"a":2}', '{"x":[{"a":1,"a":2}]}', "NaN", "Infinity", "1e999", "{}{}"]
)
def test_shared_native_json_parser_rejects_duplicates_and_non_finite_values(raw):
    with pytest.raises(ValueError):
        loads(raw)


def test_json_number_feature_cannot_reinterpret_a_literal_object_key():
    assert loads('{"$serde_json::private::Number":"1e999"}') == {
        "$serde_json::private::Number": "1e999"
    }


def test_context_causality_deadline_and_response_identity():
    parent = context(10)
    with activate(parent):
        child = call("fixture.query", {}, kind="query", timeout=20)
        assert child["context"]["correlation_id"] == parent["correlation_id"]
        assert child["context"]["causation_id"] == parent["request_id"]
        assert child["context"]["deadline_ms"] == parent["deadline_ms"]
        assert result(child, reply(child, {"ok": True})) == {"ok": True}
        with pytest.raises(ValueError, match="mismatch"):
            result(child, reply(call("fixture.query", {}), {}))
    assert current() is None
    with pytest.raises(TimeoutError), activate(parent | {"deadline_ms": 1}):
        pytest.fail("expired call executed")


def test_mutating_context_snapshots_cannot_extend_active_budget():
    parent = context(5)
    original = dict(parent)
    with activate(parent) as admitted:
        parent["deadline_ms"] += 90000
        admitted["deadline_ms"] += 90000
        current()["correlation_id"] = "x" * 32
        child = context(30)
        assert child["deadline_ms"] == original["deadline_ms"]
        assert child["correlation_id"] == original["correlation_id"]


def test_late_replies_and_invalid_time_budgets_are_rejected():
    trace = context() | {"deadline_ms": 1}
    request = {"context": trace}
    with pytest.raises(TimeoutError):
        result(request, reply(request, {"done": True}))
    with pytest.raises(TimeoutError):
        context(0)
    for seconds in (-1, float("nan"), float("inf")):
        with pytest.raises(ValueError):
            context(seconds)


def test_async_tasks_start_a_new_execution_budget_without_losing_causality(event_store):
    engine, _, _, journal = event_store
    tasks = Tasks(engine)
    parent = context(1)
    with activate(parent):
        tasks.submit("separate-execution-budget", "fixture.work", {})
        job = tasks.claim("worker")
    trace = job["communication"]
    assert trace["deadline_ms"] > parent["deadline_ms"] + 80_000_000
    assert trace["correlation_id"] == parent["correlation_id"]
    assert trace["causation_id"] == journal.read(TASK_CHANGED.id)["items"][-1]["id"]


class Fact(BaseModel):
    value: int


@pytest.fixture
def event_store(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/events.db", connect_args={"timeout": 10})
    owned = Table("fixture_owned", MetaData(), Column("id", Integer, primary_key=True))
    initialize_schema(engine, (owned, jobs, *TABLES))
    topic = Topic("fixture.changed", "fixture.owner", Fact, "/fixture")
    return engine, owned, topic, EventJournal(engine, (topic, TASK_CHANGED))


def test_event_and_business_rollback_replay_and_grants(event_store):
    engine, owned, topic, journal = event_store
    store = Storage(engine, (owned,))
    port = journal.publisher(topic.owner, (topic,))
    with pytest.raises(RuntimeError), store.begin() as transaction:
        transaction.execute(owned.insert().values(id=1))
        port.publish(transaction, topic, "one", {"value": 1})
        raise RuntimeError("rollback")
    assert journal.read(topic.id)["items"] == []
    with store.begin() as transaction:
        transaction.execute(owned.insert().values(id=1))
        saved = port.publish(transaction, topic, "one", {"value": 1})
    assert saved["sequence"] == "1"
    assert journal.read(topic.id, "0")["items"] == [saved]
    assert journal.read(topic.id, "1")["items"] == []
    assert journal.read(topic.id, "latest")["cursor"] == "1"
    with pytest.raises(ValueError):
        journal.publisher("fixture.other", (topic,))
    with store.connect() as transaction, pytest.raises(ValueError):
        port.publish(transaction, topic, "one", {"value": 2})
    with pytest.raises(ValueError):
        port.publish(transaction, topic, "one", {"value": 2})


def test_concurrent_topic_sequences_and_tasks_share_transactions(event_store):
    engine, _owned, topic, journal = event_store

    def publish(i):
        with engine.begin() as conn:
            journal.publish(conn, topic, str(i), {"value": i})

    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(publish, range(16)))
    assert [e["sequence"] for e in journal.read(topic.id)["items"]] == [
        str(n) for n in range(1, 17)
    ]
    tasks = Tasks(engine)
    job = tasks.submit("once", "fixture.work", {})
    assert tasks.submit("once", "fixture.work", {})["id"] == job["id"]
    claimed = tasks.claim("worker")
    with pytest.raises(RuntimeError), engine.begin() as conn:
        tasks.complete(conn, job["id"], claimed["token"], {})
        raise RuntimeError("crash")
    assert tasks.get(job["id"])["state"] == "RUNNING"
    assert len(journal.read(TASK_CHANGED.id)["items"]) == 2
    with engine.begin() as conn:
        tasks.complete(conn, job["id"], claimed["token"], {})
    messages = journal.read(TASK_CHANGED.id)["items"]
    assert [m["payload"]["state"] for m in messages] == ["QUEUED", "RUNNING", "SUCCEEDED"]
    assert len({m["correlation_id"] for m in messages}) == 1
    assert messages[-1]["causation_id"] == messages[-2]["id"]


def test_http_context_and_event_authorization(tmp_path):
    from asterion.api.app import create_app
    from asterion.platform.config import Settings

    app = create_app(
        Settings(token="communication-test-master-token", data_root=tmp_path),
        create_engine(f"sqlite:///{tmp_path}/api.db"),
    )
    with TestClient(app) as client:
        trace = context()
        headers = {
            "Authorization": "Bearer communication-test-master-token",
            "X-Asterion-Context": json.dumps(trace),
        }
        response = client.get("/api/v1/health", headers=headers)
        assert response.status_code == 200
        assert json.loads(response.headers["x-asterion-context"]) == trace
        headers["X-Asterion-Context"] = json.dumps(trace | {"version": 2})
        assert client.get("/api/v1/health", headers=headers).status_code == 422
        headers["X-Asterion-Context"] = json.dumps(trace | {"deadline_ms": 1})
        assert client.get("/api/v1/health", headers=headers).status_code == 408


def test_local_capability_call_propagates_context_without_serializing_objects():
    from collections.abc import Callable
    from dataclasses import dataclass

    from asterion_bindings.local import bind

    @dataclass(frozen=True)
    class Port:
        read: Callable

    marker = object()
    captured = []
    port = bind(Port(lambda value: (captured.append(current()), value)[1]))
    parent = context()
    with activate(parent):
        assert port.read(marker) is marker
    assert captured[0]["correlation_id"] == parent["correlation_id"]
    assert captured[0]["causation_id"] == parent["request_id"]


def test_backup_event_head_corruption_rejected_without_mutation(event_store):
    from asterion_bindings.events import validate_journal

    from asterion.platform.communication.events import heads

    engine, _, topic, journal = event_store
    with engine.begin() as conn:
        journal.publish(conn, topic, "one", {"value": 1})
    with engine.connect() as conn:
        assert validate_journal(conn) == 1
    with engine.begin() as conn:
        conn.execute(heads.update().values(sequence=3))
    with engine.connect() as conn, pytest.raises(ValueError, match="cursor"):
        validate_journal(conn)
    with engine.connect() as conn:
        assert conn.execute(select(heads.c.sequence)).scalar_one() == 3


def test_postgres_commit_order_cannot_skip_late_transaction(tmp_path, system_postgres):
    from threading import Event

    from asterion.platform.communication.schema import initialize_core
    from asterion.runtime.desktop import (
        initialize_postgres,
        load_config,
        pg_command,
        runtime_settings,
    )

    state = tmp_path / "communication-pg"
    config = load_config(state)
    initialize_postgres(state, system_postgres, config)
    engine = create_engine(runtime_settings(state, config).database_url)
    try:
        initialize_core(engine)
        topic = Topic("fixture.committed", "fixture.owner", Fact, "/fixture")
        journal = EventJournal(engine, (topic,))
        started = Event()
        with ThreadPoolExecutor(max_workers=1) as pool:
            with engine.begin() as first:
                journal.publish(first, topic, "first", {"value": 1})

                def second():
                    with engine.begin() as conn:
                        started.set()
                        journal.publish(conn, topic, "second", {"value": 2})

                pending = pool.submit(second)
                assert started.wait(3)
                assert journal.read(topic.id, "latest")["cursor"] == "0"
                assert not pending.done()
            pending.result(timeout=5)
        page = journal.read(topic.id, "0")
        assert [m["payload"]["value"] for m in page["items"]] == [1, 2]
        assert page["cursor"] == "2"
    finally:
        engine.dispose()
        pg_command(
            system_postgres, "pg_ctl", "-D", str(state / "postgres"), "-m", "fast", "-w", "stop"
        )


def test_task_progress_notifies_ui_but_heartbeat_does_not(event_store):
    engine, _, _, journal = event_store
    tasks = Tasks(engine)
    tasks.submit("progress", "fixture.work", {})
    job = tasks.claim("worker")
    before = journal.read(TASK_CHANGED.id, "latest")["cursor"]
    tasks.heartbeat(job["id"], job["token"])
    assert journal.read(TASK_CHANGED.id, before)["items"] == []
    with engine.begin() as conn:
        tasks.progress(conn, job["id"], job["token"], {"completed": 1, "total": 2})
        tasks.progress(conn, job["id"], job["token"], {"completed": 1, "total": 2})
    assert len(journal.read(TASK_CHANGED.id, before)["items"]) == 1
    assert tasks.get(job["id"])["result"]["completed"] == 1


def test_generated_communication_bindings_are_current():
    import subprocess
    import sys

    root = Path(__file__).parents[1]
    subprocess.run(
        [sys.executable, str(root / "scripts/generate_communication.py"), "--check"],
        check=True,
        cwd=root,
    )


def test_lifecycle_cli_uses_the_same_context_and_sanitized_error(tmp_path, monkeypatch, capsys):
    import sys

    from asterion.runtime import cli
    from asterion.runtime.environment import EnvironmentHost

    trace = context()
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "asterion",
            "desktop-info",
            "--state",
            str(tmp_path),
            "--pg-root",
            str(tmp_path),
            "--communication-context",
            json.dumps(trace),
        ],
    )

    def info(_):
        assert current() == trace
        return {"state": "fixture"}

    monkeypatch.setattr(EnvironmentHost, "info", info)
    cli.main()
    assert result({"context": trace}, json.loads(capsys.readouterr().out)) == {"state": "fixture"}

    def fail(_):
        raise ValueError("synthetic-private-token")

    monkeypatch.setattr(EnvironmentHost, "info", fail)
    with pytest.raises(SystemExit):
        cli.main()
    captured = capsys.readouterr()
    assert "synthetic-private-token" not in captured.out + captured.err
    response = validate("Reply", json.loads(captured.out))
    assert response["context"] == trace and response["error"]["code"] == "OPERATION_FAILED"
