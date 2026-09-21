"""Check actionable bundling errors."""

import importlib
from pathlib import Path
from types import SimpleNamespace

import pytest


@pytest.fixture
def build(monkeypatch, tmp_path):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1] / "scripts"))
    module = importlib.import_module("build_desktop")
    monkeypatch.setattr(module, "ROOT", tmp_path)
    return module


def test_bundle_diagnostics_identifies_dangling_resource(build, tmp_path):
    root = tmp_path / "apps/terminal/src-tauri/target/release/bundle/deb/package/data"
    root.mkdir(parents=True)
    link = root / "missing-library.so"
    link.symlink_to("absent-library.so")
    assert str(link) in build.bundle_diagnostics()
    assert link.is_symlink()


def test_bundle_diagnostics_identifies_removed_directory(build):
    assert "Missing bundle directory" in build.bundle_diagnostics()


def test_smoke_and_bundle_use_local_rust_toolchain(build, monkeypatch, tmp_path):
    toolchain = tmp_path / ".state/toolchain"
    cargo = toolchain / "cargo/bin/cargo"
    cargo.parent.mkdir(parents=True)
    cargo.touch()
    native = tmp_path / "apps/terminal/src-tauri"
    (native / "setup").mkdir(parents=True)
    (native / "setup/manifest.json").write_text("{}")
    (native / "tauri.conf.json").write_text(
        '{"version": "0.1.0", "productName": "Asterion Terminal"}'
    )
    monkeypatch.setattr(build.sys, "argv", ["build_desktop.py", "--reuse-setup", "--smoke-test"])
    monkeypatch.setattr(build.sys, "platform", "darwin")
    monkeypatch.setattr(build.platform, "machine", lambda: "arm64")
    monkeypatch.setattr(build.os, "geteuid", lambda: 1000)
    monkeypatch.setenv("PATH", "/usr/bin:/bin")
    monkeypatch.setattr(build.shutil, "which", lambda name: None if name == "cargo" else name)
    monkeypatch.setattr(build, "ensure_macos", lambda **kwargs: None)
    monkeypatch.setattr(build, "export_artifact", lambda source: None)
    monkeypatch.setattr(
        build, "macos_installer", lambda app, output, config: output / "installer.dmg"
    )
    smoke_calls = []
    bundle_calls = []

    def smoke(command, **kwargs):
        smoke_calls.append((command, kwargs))
        return SimpleNamespace(stdout=str(tmp_path / "installed") + "\n")

    monkeypatch.setattr(build.subprocess, "run", smoke)
    monkeypatch.setattr(build, "run", lambda *args, **kwargs: bundle_calls.append((args, kwargs)))
    build.main()

    command, options = smoke_calls[0]
    assert command[0] == "cargo"
    environment = options["env"]
    assert environment["PATH"].split(":")[0] == str(cargo.parent)
    assert environment["CARGO_HOME"] == str(toolchain / "cargo")
    assert environment["RUSTUP_HOME"] == str(toolchain / "rustup")
    tauri = next(options for command, options in bundle_calls if command[0] == "pnpm")
    assert tauri["env"] == environment
    command = next(command for command, _ in bundle_calls if command[0] == "pnpm")
    assert command[-2:] == ("--bundles", "app")


def test_export_installer_preserves_existing_release_on_missing_input(build, tmp_path):
    release = tmp_path / "release"
    release.mkdir()
    destination = release / "Asterion_Terminal.dmg"
    destination.write_bytes(b"existing")
    with pytest.raises(FileNotFoundError):
        build.export_artifact(tmp_path / "Asterion Terminal.dmg")
    assert destination.read_bytes() == b"existing"


def test_export_copies_only_installer_file(build, tmp_path):
    source = tmp_path / "Asterion Terminal.dmg"
    source.write_bytes(b"installer")
    destination = build.export_artifact(source)
    assert destination.name == "Asterion_Terminal.dmg"
    assert destination.read_bytes() == source.read_bytes()
    assert list((tmp_path / "release").iterdir()) == [destination]


def test_dmg_detaches_when_signature_verification_fails(build, monkeypatch, tmp_path):
    calls = []

    def run(*command):
        calls.append(command)
        if command[0] == "/usr/bin/codesign":
            raise build.subprocess.CalledProcessError(1, command)

    monkeypatch.setattr(build, "run", run)
    with pytest.raises(build.subprocess.CalledProcessError):
        build.verify_dmg(tmp_path / "installer.dmg", "Asterion Terminal")
    assert calls[-1][:2] == ("/usr/bin/hdiutil", "detach")
    assert not Path(calls[-1][2]).exists()


def test_macos_package_replaces_fixed_application_without_version_directories(
    build, monkeypatch, tmp_path
):
    import plistlib
    import xml.etree.ElementTree as ET

    app = tmp_path / "Asterion Terminal.app"
    app.mkdir()
    (app / "application-code").write_text("current")
    observed = {}

    def run(*command):
        if command[0] == "/usr/bin/pkgbuild":
            observed["component"] = plistlib.loads(
                Path(command[command.index("--component-plist") + 1]).read_bytes()
            )[0]
            observed["identifier"] = command[command.index("--identifier") + 1]
            observed["location"] = command[command.index("--install-location") + 1]
            root = Path(command[command.index("--root") + 1])
            assert list(root.iterdir()) == [root / "Applications"]
            assert list((root / "Applications").iterdir()) == [root / "Applications" / app.name]
        elif command[0] == "/usr/bin/productbuild":
            observed["domains"] = (
                ET.parse(command[command.index("--distribution") + 1]).find("domains").attrib
            )

    monkeypatch.setattr(build, "run", run)
    monkeypatch.setattr(build, "verify_dmg", lambda *args: None)
    monkeypatch.setattr(build.platform, "machine", lambda: "arm64")
    build.macos_installer(
        app,
        tmp_path / "output",
        {
            "productName": "Asterion Terminal",
            "version": "0.1.0",
            "identifier": "me.asterion.terminal",
        },
    )
    assert observed["identifier"] == "me.asterion.terminal"
    assert observed["location"] == "/"
    assert observed["component"]["RootRelativeBundlePath"] == "Applications/Asterion Terminal.app"
    assert observed["component"]["BundleIsRelocatable"] is False
    assert observed["component"]["BundleIsVersionChecked"] is False
    assert observed["component"]["BundleOverwriteAction"] == "upgrade"
    assert observed["domains"] == {
        "enable_anywhere": "false",
        "enable_currentUserHome": "false",
        "enable_localSystem": "true",
    }
