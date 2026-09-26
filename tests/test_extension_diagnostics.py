"""Execution diagnostics stay bounded, shared across processes and free of call data."""

import json
from concurrent.futures import ThreadPoolExecutor

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.diagnostics import Observation, recent
from extension_support import package_content
from fastapi.testclient import TestClient

from asterion.api.app import create_app
from asterion.distribution import extension_packages
from asterion.platform.config import Settings
from asterion.platform.extensions.process import PackageSession, call_package


def test_failure_categories_never_persist_request_or_stderr(tmp_path):
    packages = extension_packages(tmp_path)
    record = packages.install(
        package_content(
            script="import sys,time; print('private-stderr',file=sys.stderr); time.sleep(10)"
        )
    )
    _, path = packages.resolve(record["digest"])
    with pytest.raises(ValueError, match="超时"):
        call_package(path, "probe", {"token": "private-parameter"}, timeout=0.1)
    row = recent(packages.root, record["digest"])[0]
    assert row["code"] == "timeout" and row["phase"] == "probe"
    assert row["duration_ms"] >= 100 and row["calls"] == 1
    assert "private" not in json.dumps(row)
    assert b"private" not in (packages.root / "diagnostics.sqlite").read_bytes()
    with pytest.raises(ValueError, match="停用"):
        call_package(path, "probe", {}, authorized=lambda: False)
    assert recent(packages.root, record["digest"])[0]["code"] == "revoked"
    session = PackageSession(path, authorized=lambda: True, timeout=0)
    with pytest.raises(ValueError):
        session.call("strategy.open", {})
    session.close()
    rows = recent(packages.root, record["digest"])
    assert len(rows) == 3 and rows[0]["code"] == "timeout"
    assert rows[0]["phase"] == "strategy.open"


def test_concurrent_observations_are_bounded(tmp_path):
    path = tmp_path / "objects" / ("d" * 64)

    def record(_):
        observation = Observation(path)
        for _ in range(10):
            observation.call("strategy.close")
        observation.finish()
        observation.finish("protocol")

    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(record, range(215)))
    rows = recent(tmp_path, path.name)
    assert len(rows) == 30
    assert all(row["code"] == "success" and row["calls"] == 10 for row in rows)
    import sqlite3

    with sqlite3.connect(tmp_path / "diagnostics.sqlite") as conn:
        assert conn.execute("SELECT COUNT(*) FROM executions").fetchone()[0] == 200


def test_diagnostics_endpoint_requires_access_and_current_artifact(tmp_path):
    packages = extension_packages(tmp_path)
    record = packages.install(package_content())
    _, path = packages.resolve(record["digest"])
    assert call_package(path, "strategy.prepare", {"parameters": {}}) == 1
    settings = Settings(token="diagnostics-test-token-at-least-24", data_root=tmp_path)
    engine = create_engine(f"sqlite:///{tmp_path}/test.db")
    try:
        with TestClient(create_app(settings, engine)) as client:
            endpoint = "/api/v1/extensions/test.strategy/diagnostics"
            assert client.get(endpoint).status_code == 401
            client.headers["Authorization"] = "Bearer " + settings.token
            report = client.get(endpoint).json()
            assert report["digest"] == record["digest"]
            assert report["items"][0]["code"] == "success"
            assert client.get("/api/v1/extensions/missing.plugin/diagnostics").status_code == 404
    finally:
        engine.dispose()


def record_in_separate_process(root, count):
    """Spawn target: each process owns its native journal connections."""
    observation_path = root / "objects" / ("e" * 64)
    for _ in range(count):
        observation = Observation(observation_path)
        observation.call("probe")
        observation.finish()


def test_native_journal_shared_across_processes_and_rejects_unsupported_schema(tmp_path):
    import multiprocessing
    import sqlite3
    from concurrent.futures import ProcessPoolExecutor

    with ProcessPoolExecutor(
        max_workers=4, mp_context=multiprocessing.get_context("spawn")
    ) as pool:
        list(pool.map(record_in_separate_process, [tmp_path] * 4, [55] * 4))
    file = tmp_path / "diagnostics.sqlite"
    with sqlite3.connect(file) as conn:
        assert conn.execute("SELECT COUNT(*) FROM executions").fetchone()[0] == 200
    assert len(recent(tmp_path, "e" * 64)) == 30
    assert file.stat().st_size <= 1_048_576
    before = file.read_bytes()
    recent(tmp_path, "e" * 64)
    assert file.read_bytes() == before
    with sqlite3.connect(file) as conn:
        conn.execute("ALTER TABLE executions ADD COLUMN extra TEXT")
    before = file.read_bytes()
    Observation(tmp_path / "objects" / ("e" * 64)).finish()
    assert file.read_bytes() == before
    with pytest.raises(ValueError, match="Unsupported diagnostics database contract"):
        recent(tmp_path, "e" * 64)


def test_finish_concurrent_calls_and_storage_failures_never_change_execution(tmp_path):
    import subprocess
    import sys
    import time

    path = tmp_path / "objects" / ("f" * 64)
    observation = Observation(path)
    observation.call("probe")
    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(lambda _: observation.finish("timeout"), range(16)))
    assert len(recent(tmp_path, path.name)) == 1
    # A separate process exercises OS-level locking, including the real busy
    # bound. Python sqlite3 and bundled native SQLite must not own the same
    # database concurrently within one process (POSIX locks are process-wide).
    script = (
        "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); "
        "c.execute('BEGIN EXCLUSIVE'); print('locked',flush=True); "
        "sys.stdin.read(1); c.rollback(); c.close()"
    )
    with subprocess.Popen(
        [sys.executable, "-c", script, str(tmp_path / "diagnostics.sqlite")],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    ) as locked:
        assert locked.stdout is not None
        assert locked.stdout.readline().strip() == "locked"
        started = time.monotonic()
        Observation(path).finish()
        assert 1.5 <= time.monotonic() - started < 4
        locked.communicate("x", timeout=5)
        assert locked.returncode == 0
    assert len(recent(tmp_path, path.name)) == 1
    obstruction = tmp_path / "blocked"
    obstruction.write_text("preserve")
    Observation(obstruction / "objects" / path.name).finish()
    assert obstruction.read_text() == "preserve"


def test_native_diagnostics_reject_free_text_and_keep_runtime_events_bounded(tmp_path):
    import sqlite3

    from asterion_bindings.diagnostics import events, record_event

    observation = Observation(tmp_path / "objects" / ("f" * 64))
    observation.call("private secret body: credential")
    observation.finish("timeout")
    rejected = Observation(tmp_path / "objects" / ("f" * 64))
    rejected.finish("private exception message")
    record_event(tmp_path, "private component", "execution_failed")
    record_event(tmp_path, "worker", "private exception message")
    for _ in range(205):
        record_event(tmp_path, "worker", "execution_failed")
    assert len(events(tmp_path)) == 30
    assert set(events(tmp_path)[0]) == {
        "id",
        "started",
        "component",
        "code",
        "level",
        "correlation_id",
        "request_id",
    }
    with sqlite3.connect(tmp_path / "diagnostics.sqlite") as conn:
        assert conn.execute("SELECT COUNT(*) FROM runtime_events").fetchone()[0] == 200
        assert conn.execute("SELECT COUNT(*) FROM executions").fetchone()[0] == 1
    assert b"private" not in (tmp_path / "diagnostics.sqlite").read_bytes()


def test_runtime_event_context_uses_validated_ids_and_explicit_none_does_not_inherit(tmp_path):
    from asterion_bindings.communication import activate, context
    from asterion_bindings.diagnostics import events, record_event

    trace = context()
    with activate(trace):
        record_event(tmp_path, "worker", "execution_failed")
        record_event(tmp_path, "worker", "control_plane_unavailable", context=None)
        record_event(tmp_path, "worker", "subprocess_failed", context={"private": object()})
    rows = events(tmp_path)
    assert len(rows) == 2
    by_code = {row["code"]: row for row in rows}
    assert by_code["execution_failed"]["request_id"] == trace["request_id"]
    assert by_code["execution_failed"]["correlation_id"] == trace["correlation_id"]
    assert by_code["control_plane_unavailable"]["request_id"] is None
    assert by_code["control_plane_unavailable"]["correlation_id"] is None
    assert b"private" not in (tmp_path / "diagnostics.sqlite").read_bytes()


def test_nonhex_unicode_context_keeps_journal_readable_and_writable(tmp_path):
    from asterion_bindings.communication import activate, context
    from asterion_bindings.diagnostics import events, record_event

    trace = context()
    trace["request_id"] = "z" * 32
    trace["correlation_id"] = "界" * 32
    with activate(trace):
        record_event(tmp_path, "worker", "execution_failed")
    row = events(tmp_path)[0]
    assert row["request_id"] == trace["request_id"]
    assert row["correlation_id"] == trace["correlation_id"]
    record_event(tmp_path, "supervisor", "subprocess_failed", context=None)
    Observation(tmp_path / "objects" / ("a" * 64)).finish()
    assert len(events(tmp_path)) == 2
    assert len(recent(tmp_path, "a" * 64)) == 1


def test_supervisor_failure_records_only_fixed_native_event(monkeypatch, tmp_path, capsys):
    import subprocess

    from asterion_bindings.diagnostics import events

    from asterion.runtime import desktop

    settings = Settings(data_root=tmp_path / "data")
    failure = subprocess.CalledProcessError(1, ["private-command"], output="private-output")

    def unavailable(*args):
        raise failure

    monkeypatch.setattr(desktop, "runtime_identity", lambda root: "a" * 64)
    monkeypatch.setattr(desktop, "load_config", lambda state: {"api_port": 8000})
    monkeypatch.setattr(desktop, "runtime_settings", lambda state, config: settings)
    monkeypatch.setattr(desktop, "initialize_postgres", unavailable)
    monkeypatch.setattr(desktop, "pg_command", lambda *args: None)
    monkeypatch.setattr(desktop.signal, "signal", lambda *args: None)
    monkeypatch.delenv("ASTERION_RUNTIME_BUILD", raising=False)
    with pytest.raises(subprocess.CalledProcessError) as caught:
        desktop.supervise(tmp_path, tmp_path / "postgres")
    assert caught.value is failure
    assert "private" not in capsys.readouterr().err
    row = events(settings.data_root / ".diagnostics")[0]
    assert row["component"] == "supervisor" and row["code"] == "subprocess_failed"
    assert "private" not in json.dumps(row)
