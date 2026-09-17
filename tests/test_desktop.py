import json
import stat
from pathlib import Path

from asterion.runtime.desktop import child_environment, load_config, runtime_settings, service_plist


def test_persistent_private_session_and_separate_app_data(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    Path(".env").write_text("ASTERION_TOKEN=unrelated-development-token\n")
    config = load_config(tmp_path / "application")
    assert load_config(tmp_path / "application") == config
    path = tmp_path / "application/desktop.json"
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700
    settings = runtime_settings(path.parent, config)
    assert settings.token == config["token"]
    assert str(settings.data_root) == str(path.parent / "data")
    assert settings.api_url.endswith(str(config["api_port"]))


def test_launch_agent_has_no_secret_arguments(tmp_path):
    config = load_config(tmp_path)
    settings = runtime_settings(tmp_path, config)
    agent = service_plist(tmp_path, tmp_path / "postgres-runtime")
    assert config["token"] not in json.dumps(agent)
    assert config["db_password"] not in json.dumps(agent)
    assert agent["KeepAlive"] is True
    assert "desktop-supervise" in agent["ProgramArguments"]
    assert child_environment(settings)["ASTERION_TOKEN"] == config["token"]
    assert child_environment(settings)["PYINSTALLER_RESET_ENVIRONMENT"] == "1"


def test_postgres_start_has_valid_locale_without_shell_environment(tmp_path, monkeypatch):
    from asterion.runtime.desktop import pg_command

    observed = {}

    def capture(*args, **kwargs):
        observed.update(kwargs)

    monkeypatch.delenv("LANG", raising=False)
    monkeypatch.delenv("LC_ALL", raising=False)
    monkeypatch.setattr("asterion.runtime.desktop.subprocess.run", capture)
    pg_command(tmp_path, "pg_ctl", "status")
    assert observed["env"]["LC_ALL"] == "C"
    assert observed["env"]["LANG"] == "C"


def test_linux_unit_escapes_paths_and_stops_supervisor_first(tmp_path, monkeypatch):
    from asterion.runtime import desktop

    monkeypatch.setattr(desktop, "executable", lambda: ["/opt/Asterion $test%/backend"])
    state = tmp_path / "application space%"
    unit = desktop.service_unit(state, tmp_path / "postgres")
    assert '"/opt/Asterion $$test%%/backend"' in unit
    assert f"WorkingDirectory={str(state).replace('%', '%%')}\n" in unit
    assert "KillMode=mixed" in unit
    assert "Restart=always" in unit
    assert "[Install]" not in unit  # Starts on application launch, not automatically at login.
    assert "ASTERION_TOKEN" not in unit


def test_linux_service_reload_recovers_and_reuses_existing_unit(tmp_path, monkeypatch):
    from types import SimpleNamespace

    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.sys, "platform", "linux")
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "config"))
    calls = []

    def control(*args, **kwargs):
        calls.append(args)
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(desktop, "systemctl", control)
    desktop.start_service(tmp_path, tmp_path / "pg")
    assert calls == [
        ("stop", f"{desktop.LABEL}.service"),
        ("daemon-reload",),
        ("start", f"{desktop.LABEL}.service"),
    ]
    calls.clear()
    desktop.start_service(tmp_path, tmp_path / "pg")
    assert calls == [("daemon-reload",), ("start", f"{desktop.LABEL}.service")]
    calls.clear()
    desktop.start_service(tmp_path, tmp_path / "new-pg")
    assert calls[0] == ("stop", f"{desktop.LABEL}.service")
    desktop.stop(tmp_path)
    assert calls[-1] == ("stop", f"{desktop.LABEL}.service")


def test_linux_pg_environment_does_not_inherit_pyinstaller_libraries(monkeypatch):
    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.sys, "platform", "linux")
    monkeypatch.setattr(desktop.sys, "frozen", True, raising=False)
    env = {"LD_LIBRARY_PATH": "/frozen/_internal", "LD_LIBRARY_PATH_ORIG": "/original"}
    assert desktop.pg_environment(env)["LD_LIBRARY_PATH"] == "/original"
    assert env["LD_LIBRARY_PATH"] == "/frozen/_internal"
    assert "LD_LIBRARY_PATH" not in desktop.pg_environment({"LD_LIBRARY_PATH": "/frozen"})


def test_postgres_manifest_selects_relocated_paths(tmp_path):
    from asterion.runtime.desktop import pg_directory

    assert pg_directory(tmp_path, "bindir") == tmp_path / "bin"
    (tmp_path / "layout.json").write_text(
        json.dumps(
            {
                "bindir": "lib/postgresql/17/bin",
                "sharedir": "share/postgresql/17",
            }
        )
    )
    assert pg_directory(tmp_path, "bindir") == tmp_path / "lib/postgresql/17/bin"
    assert pg_directory(tmp_path, "sharedir") == tmp_path / "share/postgresql/17"
