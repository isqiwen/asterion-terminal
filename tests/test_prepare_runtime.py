"""Installer manifests use platform packages without publishing a database archive."""

import importlib
import json
from pathlib import Path

import pytest


@pytest.mark.parametrize(
    "host,arch,source",
    [
        ("darwin", "arm64", "homebrew"),
        ("darwin", "x86_64", "homebrew"),
        ("linux", "x86_64", "system"),
    ],
)
def test_prepare_uses_package_manager_and_only_downloads_python_tools(
    monkeypatch, tmp_path, host, arch, source
):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1] / "scripts"))
    module = importlib.import_module("prepare_runtime")
    monkeypatch.setattr(module, "ROOT", tmp_path)
    monkeypatch.setattr(module.sys, "platform", host)
    monkeypatch.setattr(module.platform, "machine", lambda: arch)
    (tmp_path / "apps/terminal/src-tauri").mkdir(parents=True)
    downloads = []

    def fetch(url, cache):
        downloads.append(url)
        path = cache / str(len(downloads))
        path.write_bytes(url.encode())
        return path

    def run(command, **kwargs):
        if command[:2] == ["uv", "build"]:
            output = Path(command[command.index("--out-dir") + 1])
            output.mkdir()
            (output / "asterion_terminal-0.1.0-cp312-abi3-linux_x86_64.whl").write_bytes(
                b"application"
            )
        else:
            Path(command[-1]).write_text("locked requirements")

    monkeypatch.setattr(module, "fetch", fetch)
    monkeypatch.setattr(module.subprocess, "run", run)
    monkeypatch.setattr(
        module.subprocess,
        "check_output",
        lambda *args, **kwargs: json.dumps(
            [
                {
                    "version": module.PYTHON_VERSION,
                    "variant": "default",
                    "implementation": "cpython",
                    "arch": arch,
                    "url": "https://example.com/python.tar.gz",
                }
            ]
        ).encode(),
    )
    bundle = module.prepare()
    manifest = json.loads((bundle / "manifest.json").read_text())
    assert manifest["postgres"]["source"] == source
    if source == "homebrew":
        assert manifest["postgres"] == {"source": "homebrew", "formula": "postgresql@17"}
    else:
        assert manifest["postgres"]["executable"] == "lib/postgresql/17/bin/postgres"
    assert len(downloads) == 2
    assert not (tmp_path / "release").exists()
    assert {path.name for path in bundle.iterdir()} == {
        "manifest.json",
        "requirements.txt",
        manifest["wheel"],
    }
