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
