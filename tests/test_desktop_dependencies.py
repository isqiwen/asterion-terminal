"""Exercise dependency setup without changing the host system."""

import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest


@pytest.fixture
def deps(monkeypatch):
    path = Path(__file__).resolve().parents[1] / "scripts/desktop_dependencies.py"
    spec = importlib.util.spec_from_file_location("desktop_dependencies", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    monkeypatch.delenv("CI", raising=False)
    monkeypatch.setattr(module.sys.stdin, "isatty", lambda: True)
    return module


@pytest.mark.parametrize(
    "ci,tty,interactive",
    [("true", True, True), ("1", True, True), ("", False, True), ("", True, False)],
)
def test_unattended_build_never_prompts(deps, monkeypatch, ci, tty, interactive):
    monkeypatch.setenv("CI", ci)
    monkeypatch.setattr(deps.sys.stdin, "isatty", lambda: tty)
    monkeypatch.setattr("builtins.input", lambda _: pytest.fail("Unexpected prompt"))
    with pytest.raises(SystemExit, match="interactive terminal"):
        deps.confirm_install(["libpq-dev"], "apt", interactive)


def test_declining_installation_stops(deps, monkeypatch):
    monkeypatch.setattr("builtins.input", lambda _: "no")
    with pytest.raises(SystemExit, match="cancelled"):
        deps.confirm_install(["libpq-dev"], "apt", True)


@pytest.mark.parametrize("available", [True, False])
def test_apt_install_uses_existing_sources_and_rechecks(deps, monkeypatch, available):
    monkeypatch.setattr(deps.platform, "freedesktop_os_release", lambda: {"ID": "debian"})
    missing = iter([["libpq-dev", "postgresql-17"], []])
    monkeypatch.setattr(deps, "apt_missing", lambda _: next(missing))
    monkeypatch.setattr(deps.shutil, "which", lambda _: "/usr/bin/sudo")
    monkeypatch.setattr("builtins.input", lambda _: "yes")
    calls = []

    def execute(*args, **kwargs):
        calls.append(args)
        return SimpleNamespace(
            returncode=0, stdout="Candidate: " + ("17.11" if available else "(none)")
        )

    monkeypatch.setattr(deps, "execute", execute)
    if available:
        deps.ensure_linux(runtime=True, desktop=True, interactive=True)
        assert calls[-1] == (
            "sudo",
            "apt-get",
            "install",
            "-y",
            "--no-remove",
            "libpq-dev",
            "postgresql-17",
        )
    else:
        with pytest.raises(SystemExit, match="repositories"):
            deps.ensure_linux(runtime=True, desktop=True, interactive=True)
        assert not any("install" in c for c in calls)
    assert calls[0] == ("sudo", "apt-get", "update")


def test_ready_linux_environment_does_not_prompt(deps, monkeypatch):
    monkeypatch.setattr(deps.platform, "freedesktop_os_release", lambda: {"ID": "ubuntu"})
    monkeypatch.setattr(deps, "apt_missing", lambda _: [])
    monkeypatch.setattr("builtins.input", lambda _: pytest.fail("Unexpected prompt"))
    deps.ensure_linux(runtime=True, desktop=True, interactive=True)


def test_homebrew_install_is_unprivileged_and_uses_platform_prefix(deps, monkeypatch, tmp_path):
    monkeypatch.setattr(deps, "mac_homebrew_prefix", lambda: tmp_path)
    brew = tmp_path / "bin/brew"
    brew.parent.mkdir()
    brew.touch()
    monkeypatch.setattr("builtins.input", lambda _: "y")
    calls = []

    def execute(*args, **kwargs):
        calls.append(args)
        if args == (str(brew), "install", "postgresql@17"):
            binary = tmp_path / "opt/postgresql@17/bin"
            binary.mkdir(parents=True)
            for name in ("postgres", "initdb", "pg_ctl"):
                (binary / name).touch()
        return SimpleNamespace(returncode=0, stdout=str(tmp_path))

    monkeypatch.setattr(deps, "execute", execute)
    deps.ensure_macos(runtime=True, desktop=True, interactive=True)
    assert (str(brew), "install", "postgresql@17") in calls
    assert not any("sudo" in c for c in calls)
    assert deps.mac_postgres_root() == tmp_path / "opt/postgresql@17"


def test_failed_installation_does_not_continue(deps, monkeypatch):
    monkeypatch.setattr(deps.platform, "freedesktop_os_release", lambda: {"ID": "debian"})
    monkeypatch.setattr(deps, "apt_missing", lambda _: ["libpq-dev"])
    monkeypatch.setattr(deps.shutil, "which", lambda _: "/usr/bin/sudo")
    monkeypatch.setattr("builtins.input", lambda _: "y")

    def execute(*args, **kwargs):
        return SimpleNamespace(returncode=1 if "install" in args else 0, stdout="Candidate: 0.18")

    monkeypatch.setattr(deps, "execute", execute)
    with pytest.raises(SystemExit, match="installation failed"):
        deps.ensure_linux(runtime=True, desktop=True, interactive=True)


def test_unsupported_distribution_never_installs(deps, monkeypatch):
    monkeypatch.setattr(deps.platform, "freedesktop_os_release", lambda: {"ID": "fedora"})
    monkeypatch.setattr(deps, "execute", lambda *a, **kw: pytest.fail("Unexpected command"))
    with pytest.raises(SystemExit, match="Debian/Ubuntu only"):
        deps.ensure_linux(runtime=True, desktop=True, interactive=True)
