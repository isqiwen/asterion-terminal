"""Prepare and verify the locked, offline analytics runtime bundled with Terminal."""
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
PROJECT = ROOT / "plugins/tools/analytics"
ENVIRONMENT = ROOT / "build/analytics-venv"
DESTINATION = ROOT / "build/electron-resources/analytics"
PYTHON = "3.12.12"


def verify(directory=DESTINATION):
    if not (directory / "manifest.json").is_file():
        raise ValueError("Analytics runtime missing; run pnpm prepare:analytics")
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest["platform"] != [sys.platform, platform.machine()]:
        raise ValueError("Analytics runtime is built for another platform; run pnpm prepare:analytics")
    executable = directory / "asterion_analytics"
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError("Analytics helper is not executable")


def prepare(build=None):
    if (sys.platform, platform.machine()) not in (("linux", "x86_64"), ("darwin", "arm64")):
        raise SystemExit("Analytics runtime supports Linux x86_64 and macOS Apple Silicon")
    build = build or Path(os.environ.get("ASTERION_CPP_BUILD", ROOT / "build/Debug"))
    environment = dict(os.environ, UV_PROJECT_ENVIRONMENT=str(ENVIRONMENT), MACOSX_DEPLOYMENT_TARGET="13.0")
    def run(args):
        subprocess.run(list(map(str, args)), cwd=ROOT, check=True, env=environment)
    # Downloads are confined to this explicit preparation entry, never app startup.
    run(["uv", "python", "install", PYTHON])
    run(["uv", "sync", "--project", PROJECT, "--frozen", "--group", "build", "--python", PYTHON, "--no-python-downloads", "--no-build",
         "--python-platform", "aarch64-apple-darwin" if sys.platform == "darwin" else "x86_64-manylinux_2_28"])
    run(["cmake", "--build", build, "--target", "asterion_analytics_python"])
    staging = ROOT / "build/analytics"
    staging.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prepare-", dir=staging) as temporary:
        temporary = Path(temporary)
        # PyInstaller gives Matplotlib a fresh config directory on every launch.
        # Prepare a relocatable cache of bundled fonts so statistics never scan
        # the user's system fonts before binding the IPC endpoint.
        font_cache = temporary / "matplotlib-cache"
        font_cache.mkdir()
        environment["MPLCONFIGDIR"] = str(temporary / "matplotlib-build-cache")
        run([ENVIRONMENT / "bin/python", "-c", """
from pathlib import Path
import sys
import matplotlib
from matplotlib import font_manager
manager = font_manager.fontManager
fonts = Path(matplotlib.get_data_path())
manager.ttflist = [font for font in manager.ttflist if Path(font.fname).is_relative_to(fonts)]
manager.afmlist = [font for font in manager.afmlist if Path(font.fname).is_relative_to(fonts)]
font_manager.json_dump(manager, Path(sys.argv[1]) / f"fontlist-v{manager.__version__}.json")
""", font_cache])
        run([ENVIRONMENT / "bin/python", "-m", "PyInstaller", "--noconfirm", "--clean",
             "--onedir", "--name", "asterion_analytics", "--paths", build / "generated-python",
             "--copy-metadata", "quantstats", "--collect-data", "quantstats",
             "--copy-metadata", "skfolio", "--copy-metadata", "polars",
             "--additional-hooks-dir", ROOT / "scripts/desktop/pyinstaller",
             "--collect-all", "_polars_runtime_compat", "--exclude-module", "_polars_runtime_32",
             "--hidden-import", "cvxpy.reductions.solvers.conic_solvers.clarabel_conif",
             "--add-data", str(font_cache) + ":matplotlib-cache",
             "--exclude-module", "tkinter", "--exclude-module", "IPython",
             "--distpath", temporary / "dist", "--workpath", temporary / "work",
             "--specpath", temporary, PROJECT / "report.py"])
        # Dereference PyInstaller's internal symlinks so packaging owns one regular tree.
        prepared = temporary / "analytics"
        shutil.copytree(temporary / "dist/asterion_analytics", prepared)
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
        (prepared / "manifest.json").write_text(json.dumps({"platform": [sys.platform, platform.machine()],
            "python": PYTHON}, indent=2) + "\n")
        verify(prepared)
        DESTINATION.parent.mkdir(parents=True, exist_ok=True)
        publish_tree(prepared, DESTINATION)
    print(f"Prepared offline Analytics runtime: {DESTINATION}")


if __name__ == "__main__":
    prepare()
