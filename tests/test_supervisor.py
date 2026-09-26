"""Real native supervision and Python signal delivery, without touching installed services."""

import json
import os
import signal
import subprocess
import sys
import time

import pytest
from asterion_bindings import _native


def configuration(root):
    return {
        "processes": [
            {
                "name": "isolated-role",
                "program": "/bin/sh",
                "arguments": ["-c", "trap '' TERM; echo $$ > leader; exec /bin/sleep 60"],
                "environment": {},
                "cwd": str(root),
            }
        ],
        "status_file": str(root / "status.json"),
        "build_id": "a" * 64,
        "policy": {"restart_millis": 100, "snapshot_millis": 10, "shutdown_millis": 100},
    }


def test_python_signal_stops_native_loop_and_reaps_owned_child(tmp_path):
    declaration = tmp_path / "configuration.json"
    declaration.write_text(json.dumps(configuration(tmp_path)))
    runner = subprocess.Popen(
        [
            sys.executable,
            "-c",
            (
                "import signal, sys; from asterion_bindings import _native; "
                "s = _native.Supervisor(open(sys.argv[1]).read()); "
                "signal.signal(signal.SIGTERM, lambda *_: s.request_stop()); s.run()"
            ),
            str(declaration),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    leader = None
    reaped = False
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if (tmp_path / "status.json").exists():
                try:
                    leader = int((tmp_path / "leader").read_text())
                except (OSError, ValueError):
                    pass
                else:
                    break
            if runner.poll() is not None:
                pytest.fail("isolated supervisor exited before readiness")
            time.sleep(0.01)
        assert leader is not None
        snapshot = json.loads((tmp_path / "status.json").read_text())
        assert snapshot["isolated-role"] == "running"
        runner.terminate()
        output, error = runner.communicate(timeout=5)
        assert runner.returncode == 0, (output, error)
        assert not (tmp_path / "status.json").exists()
        with pytest.raises(ProcessLookupError):
            os.kill(leader, 0)
        reaped = True
    finally:
        if runner.poll() is None:
            runner.kill()
            runner.communicate(timeout=5)
        # A failed supervisor may leave its separately owned child group alive.
        if not reaped and leader is None:
            try:
                leader = int((tmp_path / "leader").read_text())
            except (OSError, ValueError):
                pass
        if not reaped and leader is not None:
            try:
                os.killpg(leader, signal.SIGKILL)
            except ProcessLookupError:
                pass


def test_stopped_supervisor_cannot_run_again_or_spawn(tmp_path):
    supervisor = _native.Supervisor(json.dumps(configuration(tmp_path)))
    supervisor.request_stop()
    supervisor.run()
    with pytest.raises(ValueError, match="once"):
        supervisor.run()
    assert not (tmp_path / "leader").exists()


def test_declaration_rejects_unknown_fields_before_start(tmp_path):
    value = configuration(tmp_path)
    value["processes"][0]["unknown"] = True
    with pytest.raises(ValueError, match="configuration"):
        _native.Supervisor(json.dumps(value))
    assert not (tmp_path / "leader").exists()


def test_supervisor_assembles_absolute_paths_from_cli_input(tmp_path, monkeypatch):
    from pathlib import Path

    from asterion.runtime import desktop

    captured = {}

    def capture(processes, status_file, build_id):
        captured.update(processes=processes, status_file=status_file, build_id=build_id)
        raise RuntimeError("captured declaration")

    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("ASTERION_RUNTIME_BUILD", raising=False)
    monkeypatch.setattr(desktop, "runtime_identity", lambda _: "a" * 64)
    monkeypatch.setattr("asterion_bindings.supervisor.Supervisor", capture)
    monkeypatch.setattr(desktop, "initialize_postgres", lambda *_: pytest.fail("no startup"))
    with pytest.raises(RuntimeError, match="captured declaration"):
        desktop.supervise(Path("state"), Path("runtime"))
    assert captured["status_file"] == tmp_path / "state/runtime-status.json"
    processes = {item["name"]: item for item in captured["processes"]}
    assert set(processes) == {"server", "serve", "worker"}
    config = json.loads((tmp_path / "state/desktop.json").read_text())
    server = processes["server"]["arguments"]
    assert server[:2] == ["--listen", f"127.0.0.1:{config['api_port']}"]
    internal = processes["serve"]["arguments"][-1]
    assert server[2:] == ["--upstream", f"http://127.0.0.1:{internal}"]
    assert internal != str(config["api_port"])
    entry = processes["server"]["environment"]
    assert {key for key in entry if key.startswith("ASTERION_")} == {
        "ASTERION_TOKEN",
        "ASTERION_DATABASE_URL",
        "ASTERION_DATA_ROOT",
        "ASTERION_ACCOUNT_VERIFICATION",
        "ASTERION_REQUIRE_ACCOUNT",
        "ASTERION_LEASE_SECONDS",
    }
    settings = desktop.runtime_settings(tmp_path / "state", config)
    assert entry["ASTERION_TOKEN"] == config["token"]
    assert entry["ASTERION_DATABASE_URL"] == settings.database_url
    assert entry["ASTERION_DATA_ROOT"] == str(settings.data_root)
    assert entry["ASTERION_ACCOUNT_VERIFICATION"] == "local"
    assert entry["ASTERION_REQUIRE_ACCOUNT"] == "true"
    assert entry["ASTERION_LEASE_SECONDS"] == "60"
    python = processes["serve"]["environment"]
    assert "ASTERION_ACCOUNT_VERIFICATION" not in python
    assert all(item["cwd"] == str(tmp_path / "state") for item in captured["processes"])
