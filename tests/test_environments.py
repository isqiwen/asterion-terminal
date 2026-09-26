"""Failure-injected service transitions never change or delete the original environment."""

import json
from pathlib import Path

import pytest

from asterion.runtime import environment as env
from asterion.runtime.desktop import load_config
from asterion.runtime.environment import EnvironmentHost


@pytest.fixture
def setup(tmp_path, monkeypatch):
    host, target = tmp_path / "host", tmp_path / "restored"
    for state in (host, target):
        load_config(state)
        (state / "data").mkdir()
        (state / "postgres").mkdir()
        (state / "postgres/PG_VERSION").write_text("17")
        (state / "data/retained").write_text(state.name)
    (target / "restore-report.json").write_text(
        json.dumps({"status": "verified", "application_format": 1})
    )
    calls = []
    monkeypatch.setattr(env, "stop", lambda state: calls.append(("stop", state)))
    monkeypatch.setattr(env, "bootstrap", lambda state, _: calls.append(("start", state)))
    monkeypatch.setattr(env, "create_backup", lambda state, _: calls.append(("backup", state)))
    monkeypatch.setattr(env, "restore_backup", lambda *_: calls.append(("verify-backup",)))
    monkeypatch.setattr(
        env, "validate_database", lambda state, _, **kw: {"quarantine": kw["quarantine"]}
    )
    with EnvironmentHost(host, host) as environment:
        yield host, target, calls, environment


def test_switch_survives_restart_and_rollback_protects_new_records(setup):
    host, target, calls, environment = setup
    result = environment.switch(target)
    assert result["verification"]["quarantine"]
    assert environment.active() == target
    assert environment.info()["data_directory"] == str(target / "data")
    assert calls == [("stop", host), ("backup", host), ("verify-backup",), ("start", target)]
    (target / "data/new-record").write_text("created after activation")
    calls.clear()
    environment.switch()
    assert environment.active() == host
    assert environment.status()["previous"] == str(target)
    assert calls == [("stop", target), ("backup", target), ("verify-backup",), ("start", host)]
    assert (target / "data/new-record").read_text() == "created after activation"
    assert (host / "data/retained").read_text() == "host"
    assert (host / "environment.json").stat().st_mode & 0o777 == 0o600


@pytest.mark.parametrize("failure", ["copy", "backup-validation", "target-validation", "startup"])
def test_failure_before_commit_restarts_source(setup, monkeypatch, failure):
    host, target, calls, environment = setup

    def fail(*args, **kwargs):
        raise RuntimeError("injected failure")

    if failure == "startup":
        monkeypatch.setattr(
            env,
            "bootstrap",
            lambda state, _: fail() if state == target else calls.append(("start", state)),
        )
    else:
        monkeypatch.setattr(
            env,
            {
                "copy": "create_backup",
                "backup-validation": "restore_backup",
                "target-validation": "validate_database",
            }[failure],
            fail,
        )
    with pytest.raises(RuntimeError, match="injected"):
        environment.switch(target)
    assert environment.active() == host
    assert environment.status()["pending"] is None
    assert calls[-1] == ("start", host)
    assert (host / "data/retained").exists()


def test_interrupted_switch_recovered_before_next_bootstrap(setup, monkeypatch):
    host, target, calls, environment = setup

    def crash(*args, **kwargs):
        raise KeyboardInterrupt()

    monkeypatch.setattr(env, "validate_database", crash)
    with pytest.raises(KeyboardInterrupt):
        environment.switch(target)
    assert environment.status()["pending"] == str(target)
    assert environment.active() == host
    environment.recover()
    assert environment.status()["pending"] is None
    assert calls[-1] == ("start", host)


def test_failed_fallback_keeps_journal_for_retry(setup, monkeypatch):
    host, target, _, environment = setup

    def unavailable(*args):
        raise RuntimeError("service unavailable")

    monkeypatch.setattr(env, "bootstrap", unavailable)
    with pytest.raises(RuntimeError):
        environment.switch(target)
    assert environment.active() == host
    assert environment.status()["pending"] == str(target)
    monkeypatch.setattr(env, "bootstrap", lambda *_: None)
    environment.recover()
    assert environment.status()["pending"] is None


def test_reject_incomplete_nested_and_running_targets_before_source_stop(setup):
    host, target, calls, environment = setup
    for bad in (host, host / "nested", host.parent):
        with pytest.raises(ValueError):
            environment.switch(bad)
    report_path = target / "restore-report.json"
    original_report = report_path.read_bytes()
    report_path.write_text(json.dumps({"status": "unverified", "application_format": 1}))
    with pytest.raises(ValueError):
        environment.switch(target)
    report_path.write_bytes(original_report)
    (target / "postgres/postmaster.pid").write_text("123")
    with pytest.raises(ValueError):
        environment.switch(target)
    assert calls == []


def test_maintenance_excludes_second_window_and_missing_active_never_reinitialized(setup):
    host, target, _, environment = setup
    with pytest.raises(ValueError, match="维护"), EnvironmentHost(host, host):
        pass
    selection = environment.status() | {"active": str(target / "missing")}
    (host / "environment.json").write_text(json.dumps(selection))
    with pytest.raises(ValueError, match="不可用"):
        environment.active()
    assert not (target / "missing").exists()
    selection["active"] = str(target)
    (host / "environment.json").write_text(json.dumps(selection))
    (target / "postgres/PG_VERSION").unlink()
    with pytest.raises(ValueError, match="不可用"):
        environment.active()


@pytest.mark.parametrize("mutation", ["missing", "unknown", "relative", "nested", "large"])
def test_invalid_current_selection_is_rejected_without_rewriting(setup, mutation):
    host, _, _, environment = setup
    selection = environment.status()
    if mutation == "missing":
        del selection["pending"]
    elif mutation == "unknown":
        selection["unexpected"] = None
    elif mutation == "relative":
        selection["active"] = "relative"
    elif mutation == "nested":
        selection["pending"] = str(host / "nested")
    raw = json.dumps(selection) if mutation != "large" else " " * 65537
    manifest = host / "environment.json"
    manifest.write_text(raw)
    with pytest.raises(ValueError):
        environment.status()
    assert manifest.read_text() == raw


def test_closed_environment_rejects_operations_and_releases_lock(tmp_path):
    with EnvironmentHost(tmp_path, tmp_path) as environment:
        assert environment.active() == tmp_path
    with pytest.raises(ValueError, match="not open"):
        environment.status()
    with EnvironmentHost(tmp_path, tmp_path) as another:
        assert another.active() == tmp_path


def test_actual_postgres_supervisor_activation_and_rollback(tmp_path, monkeypatch, system_postgres):
    import subprocess
    import sys
    import time

    from asterion.runtime.desktop import healthy, runtime_settings, worker_ready

    pg = system_postgres
    host, target = tmp_path / "host", tmp_path / "restored"
    processes = {}

    def start(state, pg_root):
        state.mkdir(exist_ok=True)
        with (state / "test-service.log").open("a") as log:
            processes[state] = subprocess.Popen(
                [
                    sys.executable,
                    "-m",
                    "asterion.runtime.cli",
                    "desktop-supervise",
                    "--state",
                    str(state),
                    "--pg-root",
                    str(pg_root),
                ],
                stdout=log,
                stderr=log,
            )
        settings = runtime_settings(state, load_config(state))
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            if healthy(settings) and worker_ready(state):
                return
            if processes[state].poll() is not None:
                raise RuntimeError("isolated supervisor stopped")
            time.sleep(0.2)
        raise RuntimeError("isolated supervisor readiness timeout")

    def halt(state):
        process = processes.pop(state, None)
        if process:
            process.terminate()
            try:
                process.wait(timeout=40)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise

    monkeypatch.setattr(env, "bootstrap", start)
    monkeypatch.setattr(env, "stop", halt)
    try:
        start(host, pg)
        halt(host)
        archive = tmp_path / "original.zip"
        env.create_backup(host, archive)
        env.restore_backup(archive, target, pg)
        start(host, pg)
        with EnvironmentHost(host, pg) as environment:
            switched = environment.switch(target)
            assert environment.active() == target and worker_ready(target)
        assert not (host / "postgres/postmaster.pid").exists()
        assert Path(switched["protection_backup"]).is_file()
        (target / "data/after-switch.txt").write_text("retained")
        with EnvironmentHost(host, pg) as environment:
            rolled_back = environment.switch()
            assert environment.active() == host and worker_ready(host)
        assert not (target / "postgres/postmaster.pid").exists()
        assert (target / "data/after-switch.txt").read_text() == "retained"
        assert rolled_back["protection_backup"] != switched["protection_backup"]
    finally:
        for state in list(processes):
            halt(state)
