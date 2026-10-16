"""Prepare and verify the locked, offline analytics runtime bundled with Terminal."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

from generated_resources import publish_tree

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "plugins/tools/quantstats"
ENVIRONMENT = ROOT / "build/quantstats-venv"
DESTINATION = ROOT / "build/electron-resources/quantstats"
PYTHON = "3.12.12"
SOURCES = ("plugins/tools/quantstats/report.py", "plugins/tools/quantstats/pyproject.toml",
           "plugins/tools/quantstats/uv.lock", "protocol/proto/asterion/v1/analytics.proto",
           "scripts/desktop/quantstats_runtime.py", "plugins/tools/quantstats/PYTHON-LICENSE.txt")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def source_identity():
    return {name: digest(ROOT / name) for name in SOURCES}


def verify(directory=DESTINATION, signed=False):
    if not (directory / "manifest.json").is_file():
        raise ValueError("QuantStats runtime missing; run pnpm prepare:quantstats")
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest["platform"] != [sys.platform, platform.machine()] or manifest["sources"] != source_identity():
        raise ValueError("QuantStats runtime is stale or built for another platform; run pnpm prepare:quantstats")
    entries = [path for path in directory.rglob("*") if path.is_file() and path.name != "manifest.json"]
    if {path.relative_to(directory).as_posix() for path in entries} != manifest["files"].keys():
        raise ValueError("QuantStats runtime differs from its distribution manifest")
    if not signed and any(digest(path) != manifest["files"][path.relative_to(directory).as_posix()] for path in entries):
        raise ValueError("QuantStats runtime content digest differs")
    if not os.access(directory / "asterion-quantstats", os.X_OK):
        raise ValueError("QuantStats helper is not executable")


def prepare(build=None):
    if (sys.platform, platform.machine()) not in (("linux", "x86_64"), ("darwin", "arm64")):
        raise SystemExit("QuantStats runtime supports Linux x86_64 and macOS Apple Silicon")
    build = build or Path(os.environ.get("ASTERION_CPP_BUILD", ROOT / "build/Debug"))
    environment = dict(os.environ, UV_PROJECT_ENVIRONMENT=str(ENVIRONMENT), MACOSX_DEPLOYMENT_TARGET="13.0")
    def run(args):
        subprocess.run(list(map(str, args)), cwd=ROOT, check=True, env=environment)
    # Downloads are confined to this explicit preparation entry, never app startup.
    run(["uv", "python", "install", PYTHON])
    run(["uv", "sync", "--project", PROJECT, "--frozen", "--group", "build", "--python", PYTHON, "--no-python-downloads", "--no-build",
         "--python-platform", "aarch64-apple-darwin" if sys.platform == "darwin" else "x86_64-manylinux_2_28"])
    run(["cmake", "--build", build, "--target", "asterion_analytics_python"])
    staging = ROOT / "build/quantstats"
    staging.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prepare-", dir=staging) as temporary:
        temporary = Path(temporary)
        run([ENVIRONMENT / "bin/python", "-m", "PyInstaller", "--noconfirm", "--clean",
             "--onedir", "--name", "asterion-quantstats", "--paths", build / "generated-python",
             "--copy-metadata", "quantstats", "--collect-data", "quantstats",
             "--exclude-module", "tkinter", "--exclude-module", "IPython",
             "--distpath", temporary / "dist", "--workpath", temporary / "work",
             "--specpath", temporary, PROJECT / "report.py"])
        # Dereference PyInstaller's internal symlinks so packaging owns one regular tree.
        prepared = temporary / "quantstats"
        shutil.copytree(temporary / "dist/asterion-quantstats", prepared)
        licenses = prepared / "licenses"
        licenses.mkdir()
        shutil.copy2(PROJECT / "PYTHON-LICENSE.txt", licenses / "PYTHON-LICENSE.txt")
        for package in (ENVIRONMENT / "lib/python3.12/site-packages").glob("*.dist-info"):
            records = [path for path in package.rglob("*") if path.is_file()
                       and ("license" in path.name.lower() or "copying" in path.name.lower())]
            for path in records:
                target = licenses / package.name / path.relative_to(package)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
        files = {path.relative_to(prepared).as_posix(): digest(path) for path in prepared.rglob("*") if path.is_file()}
        (prepared / "manifest.json").write_text(json.dumps({"platform": [sys.platform, platform.machine()],
            "python": PYTHON, "sources": source_identity(), "files": files}, indent=2) + "\n")
        verify(prepared)
        DESTINATION.parent.mkdir(parents=True, exist_ok=True)
        publish_tree(prepared, DESTINATION)
    print(f"Prepared offline QuantStats runtime: {DESTINATION}")


if __name__ == "__main__":
    prepare()
