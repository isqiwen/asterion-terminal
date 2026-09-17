"""Check actionable bundling errors."""

import importlib
from pathlib import Path

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
