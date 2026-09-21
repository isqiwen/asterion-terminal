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
    assert child_environment(settings)["PYTHONDONTWRITEBYTECODE"] == "1"


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


def test_pg_environment_sets_locale_without_mutating_input():
    from asterion.runtime import desktop

    env = {"LANG": "invalid", "PATH": "/usr/bin"}
    assert desktop.pg_environment(env) == {"LANG": "C", "LC_ALL": "C", "PATH": "/usr/bin"}
    assert env["LANG"] == "invalid"


def test_postgres_paths_follow_platform_packages(tmp_path, monkeypatch):
    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.sys, "platform", "linux")
    assert desktop.pg_directory(tmp_path, "bindir") == tmp_path / "lib/postgresql/17/bin"
    assert desktop.pg_directory(tmp_path, "sharedir") == tmp_path / "share/postgresql/17"
    monkeypatch.setattr(desktop.sys, "platform", "darwin")
    assert desktop.pg_directory(tmp_path, "bindir") == tmp_path / "bin"
    assert desktop.pg_directory(tmp_path, "sharedir") == tmp_path / "share/postgresql@17"


def test_macos_upgrade_waits_for_old_registration_then_retries(tmp_path, monkeypatch):
    import plistlib
    from types import SimpleNamespace

    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.sys, "platform", "darwin")
    monkeypatch.setattr(desktop.Path, "home", lambda: tmp_path)
    monkeypatch.setattr(desktop.time, "sleep", lambda _: None)
    path = tmp_path / "Library/LaunchAgents" / f"{desktop.LABEL}.plist"
    path.parent.mkdir(parents=True)
    path.write_bytes(plistlib.dumps({"Label": desktop.LABEL}))
    calls = []
    results = iter([0, 0, 0, 113, 5, 0])

    def control(*args, **kwargs):
        calls.append(args[0])
        return SimpleNamespace(returncode=next(results), stderr="transient", stdout="")

    monkeypatch.setattr(desktop, "launchctl", control)
    desktop.start_service(tmp_path, tmp_path / "pg")
    assert calls == ["print", "bootout", "print", "print", "bootstrap", "bootstrap"]
    assert plistlib.loads(path.read_bytes()) == desktop.service_plist(tmp_path, tmp_path / "pg")


def test_macos_bootstrap_failure_is_bounded_and_actionable(tmp_path, monkeypatch):
    from types import SimpleNamespace

    import pytest

    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.sys, "platform", "darwin")
    monkeypatch.setattr(desktop.Path, "home", lambda: tmp_path)
    monkeypatch.setattr(desktop.time, "sleep", lambda _: None)
    calls = []

    def control(*args, **kwargs):
        calls.append(args[0])
        return SimpleNamespace(returncode=5, stderr="Input/output error", stdout="")

    monkeypatch.setattr(desktop, "launchctl", control)
    with pytest.raises(RuntimeError, match="Input/output error.*服务配置.*诊断日志"):
        desktop.start_service(tmp_path, tmp_path / "pg")
    assert calls == ["print"] + ["bootstrap"] * 5


def test_macos_reuses_unchanged_loaded_service(tmp_path, monkeypatch):
    import plistlib
    from types import SimpleNamespace

    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.sys, "platform", "darwin")
    monkeypatch.setattr(desktop.Path, "home", lambda: tmp_path)
    path = tmp_path / "Library/LaunchAgents" / f"{desktop.LABEL}.plist"
    path.parent.mkdir(parents=True)
    path.write_bytes(plistlib.dumps(desktop.service_plist(tmp_path, tmp_path / "pg")))
    calls = []

    def control(*args, **kwargs):
        calls.append(args[0])
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(desktop, "launchctl", control)
    desktop.start_service(tmp_path, tmp_path / "pg")
    assert calls == ["print"]


def test_macos_does_not_replace_registration_while_old_service_stops(tmp_path, monkeypatch):
    import plistlib
    from types import SimpleNamespace

    import pytest

    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.sys, "platform", "darwin")
    monkeypatch.setattr(desktop.Path, "home", lambda: tmp_path)
    times = iter([0, 41])
    monkeypatch.setattr(desktop.time, "monotonic", lambda: next(times))
    path = tmp_path / "Library/LaunchAgents" / f"{desktop.LABEL}.plist"
    path.parent.mkdir(parents=True)
    original = plistlib.dumps({"Label": desktop.LABEL})
    path.write_bytes(original)
    calls = []

    def control(*args, **kwargs):
        calls.append(args[0])
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(desktop, "launchctl", control)
    with pytest.raises(RuntimeError, match="旧版后台服务仍在停止"):
        desktop.start_service(tmp_path, tmp_path / "pg")
    assert calls == ["print", "bootout", "print"]
    assert path.read_bytes() == original


def test_runtime_identity_uses_contents_not_location_or_mtime(tmp_path):
    import os
    import shutil

    import pytest

    from asterion.runtime.build_identity import tree_digest

    root = tmp_path / "runtime"
    root.mkdir()
    (root / "backend").write_bytes(b"binary")
    (root / "dependency.so").write_bytes(b"first")
    original = tree_digest((root,))
    os.utime(root / "backend", (1, 1))
    (root / "__pycache__").mkdir()
    (root / "__pycache__/cache.pyc").write_bytes(b"ephemeral")
    assert tree_digest((root,)) == original
    other = tmp_path / "moved"
    shutil.copytree(root, other)
    assert tree_digest((other,)) == original
    (root / "dependency.so").write_bytes(b"other")
    assert tree_digest((root,)) != original
    (root / "dependency.so").unlink()
    assert tree_digest((root,)) != original
    with pytest.raises(ValueError, match="运行文件缺失"):
        tree_digest((tmp_path / "missing",))


def test_macos_same_path_new_build_restarts_once(tmp_path, monkeypatch):
    import plistlib
    from types import SimpleNamespace

    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.sys, "platform", "darwin")
    monkeypatch.setattr(desktop.Path, "home", lambda: tmp_path)
    path = tmp_path / "Library/LaunchAgents" / f"{desktop.LABEL}.plist"
    path.parent.mkdir(parents=True)
    path.write_bytes(plistlib.dumps(desktop.service_plist(tmp_path, tmp_path, "a" * 64)))
    calls = []
    installed = True

    def control(*args, **kwargs):
        nonlocal installed
        calls.append(args[0])
        if args[0] == "print":
            return SimpleNamespace(returncode=0 if installed else 113)
        installed = args[0] == "bootstrap"
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(desktop, "launchctl", control)
    desktop.start_service(tmp_path, tmp_path, "b" * 64)
    assert calls == ["print", "bootout", "print", "bootstrap"]
    calls.clear()
    desktop.start_service(tmp_path, tmp_path, "b" * 64)
    assert calls == ["print"]
    assert (
        plistlib.loads(path.read_bytes())["EnvironmentVariables"]["ASTERION_RUNTIME_BUILD"]
        == "b" * 64
    )


def test_linux_new_build_and_failed_stop_never_overwrite_unit(tmp_path, monkeypatch):
    from types import SimpleNamespace

    import pytest

    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.sys, "platform", "linux")
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "config"))
    calls = []

    def control(*args, **kwargs):
        calls.append(args)
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(desktop, "systemctl", control)
    desktop.start_service(tmp_path, tmp_path, "a" * 64)
    path = tmp_path / "config/systemd/user" / f"{desktop.LABEL}.service"
    original = path.read_bytes()

    def failed(*args, **kwargs):
        raise RuntimeError("stop failed")

    monkeypatch.setattr(desktop, "systemctl", failed)
    with pytest.raises(RuntimeError, match="stop failed"):
        desktop.start_service(tmp_path, tmp_path, "b" * 64)
    assert path.read_bytes() == original
    monkeypatch.setattr(desktop, "systemctl", control)
    calls.clear()
    desktop.start_service(tmp_path, tmp_path, "b" * 64)
    assert calls[0][0] == "stop"
    calls.clear()
    desktop.start_service(tmp_path, tmp_path, "b" * 64)
    assert calls == [("daemon-reload",), ("start", f"{desktop.LABEL}.service")]


def test_bootstrap_requires_current_running_build_not_just_healthy_api(tmp_path, monkeypatch):
    from asterion.runtime import desktop

    monkeypatch.setattr(desktop, "runtime_identity", lambda _: "b" * 64)
    monkeypatch.setattr(desktop, "healthy", lambda _: True)
    monkeypatch.setattr(desktop, "worker_ready", lambda _: True)
    builds = iter(["a" * 64, "b" * 64])
    monkeypatch.setattr(desktop, "running_build", lambda _: next(builds))
    calls = []
    monkeypatch.setattr(desktop, "start_service", lambda *args: calls.append(args))
    waits = []
    monkeypatch.setattr(desktop.time, "sleep", waits.append)
    assert desktop.bootstrap(tmp_path, tmp_path)["api_url"]
    assert calls[0][2] == "b" * 64
    assert waits == [0.4]


def test_running_build_requires_fresh_valid_observation(tmp_path, monkeypatch):
    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.time, "time", lambda: 100)
    path = tmp_path / "runtime-status.json"
    for value in (
        {},
        {"observed_at": "invalid"},
        {"observed_at": 101, "build_id": "a"},
        {"observed_at": 90, "build_id": "a"},
    ):
        path.write_text(json.dumps(value))
        assert desktop.running_build(tmp_path) is None
    path.write_text(json.dumps({"observed_at": 99, "build_id": "a" * 64}))
    assert desktop.running_build(tmp_path) == "a" * 64


def test_supervisor_rejects_changed_installation_before_initializing_data(tmp_path, monkeypatch):
    import pytest

    from asterion.runtime import desktop

    monkeypatch.setenv("ASTERION_RUNTIME_BUILD", "a" * 64)
    monkeypatch.setattr(desktop, "runtime_identity", lambda _: "b" * 64)
    monkeypatch.setattr(
        desktop, "initialize_postgres", lambda *args: pytest.fail("must not initialize")
    )
    with pytest.raises(RuntimeError, match="运行文件已变化"):
        desktop.supervise(tmp_path, tmp_path)
    assert not (tmp_path / "desktop.json").exists()


def test_macos_waits_for_database_owner_before_replacing_configuration(tmp_path, monkeypatch):
    import plistlib
    from types import SimpleNamespace

    import pytest

    from asterion.runtime import desktop

    monkeypatch.setattr(desktop.sys, "platform", "darwin")
    monkeypatch.setattr(desktop.Path, "home", lambda: tmp_path)
    path = tmp_path / "Library/LaunchAgents" / f"{desktop.LABEL}.plist"
    path.parent.mkdir(parents=True)
    original = plistlib.dumps(desktop.service_plist(tmp_path, tmp_path, "a" * 64))
    path.write_bytes(original)
    results = iter([0, 0, 113])
    calls = []

    def control(*args, **kwargs):
        calls.append(args[0])
        return SimpleNamespace(returncode=next(results))

    def busy(_):
        raise RuntimeError("后台仍在停止")

    monkeypatch.setattr(desktop, "launchctl", control)
    monkeypatch.setattr(desktop, "wait_stopped", busy)
    with pytest.raises(RuntimeError, match="后台仍在停止"):
        desktop.start_service(tmp_path, tmp_path, "b" * 64)
    assert path.read_bytes() == original
    assert calls == ["print", "bootout", "print"]
