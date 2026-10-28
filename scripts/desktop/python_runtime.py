"""Prepare and verify the Python runtime bundled with Terminal.

A relocatable CPython (python-build-standalone, as uv installs it) with the
locked dependencies and Terminal's Python code installed into its own
site-packages. No virtual environment: a venv records absolute paths, the
standalone interpreter finds its prefix from where it is. Programs run as
`python3 -I -B -m <module>`: no user site, no environment, no bytecode written
into a signed or shared installation.
"""
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
DESTINATION = ROOT / "build/electron-resources/python"
PYTHON = "3.12.12"
SITE = "lib/python3.12/site-packages"
# Parts of the standalone distribution nothing in Terminal uses.
UNUSED = ["include", "share", "lib/python3.12/test", "lib/python3.12/idlelib",
          "lib/python3.12/tkinter", "lib/python3.12/turtledemo", "lib/python3.12/ensurepip",
          "lib/python3.12/lib2to3",
          # Linux's link-time copy; the interpreter loads libpython3.12.so.1.0. On
          # macOS libpython3.12.dylib is the library the interpreter loads.
          "lib/libpython3.12.so"]


def executable(directory=DESTINATION):
    return directory / "bin/python3"


def library_directories(directory=DESTINATION):
    """Where the runtime's own shared libraries live, for dependency analysis."""
    return [directory / "lib", *sorted((directory / SITE).glob("*.libs"))]


def verify(directory=DESTINATION):
    if not (directory / "manifest.json").is_file():
        raise ValueError("Python runtime missing; run pnpm prepare:python")
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest["platform"] != [sys.platform, platform.machine()]:
        raise ValueError("Python runtime is built for another platform; run pnpm prepare:python")
    if not executable(directory).is_file() or not os.access(executable(directory), os.X_OK):
        raise ValueError("Bundled Python is not executable")


def prepare(build=None):
    if (sys.platform, platform.machine()) not in (("linux", "x86_64"), ("darwin", "arm64")):
        raise SystemExit("The Python runtime supports Linux x86_64 and macOS Apple Silicon")
    build = build or Path(os.environ.get("ASTERION_CPP_BUILD", ROOT / "build/Debug"))
    environment = dict(os.environ, MACOSX_DEPLOYMENT_TARGET="13.0")

    def run(args, **options):
        return subprocess.run(list(map(str, args)), cwd=ROOT, check=True, env=environment, **options)

    # Downloads are confined to this explicit preparation entry, never app startup.
    run(["uv", "python", "install", PYTHON])
    found = run(["uv", "python", "find", "--no-project", PYTHON], capture_output=True, text=True)
    standalone = Path(found.stdout.strip()).resolve().parents[1]
    run(["cmake", "--build", build, "--target", "asterion_analytics_python"])
    requirements = run(["uv", "export", "--project", PROJECT, "--frozen", "--no-dev",
                        "--no-emit-project", "--quiet"], capture_output=True, text=True).stdout
    staging = ROOT / "build/python"
    staging.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prepare-", dir=staging) as temporary:
        temporary = Path(temporary)
        prepared = temporary / "python"
        # Symlinks dereferenced: packaging owns one regular tree.
        shutil.copytree(standalone, prepared)
        (prepared / "lib/python3.12/EXTERNALLY-MANAGED").unlink(missing_ok=True)
        for name in UNUSED:
            path = prepared / name
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink(missing_ok=True)
        for pattern in ("lib/libtcl*", "lib/libtk*", "lib/tcl*", "lib/tk*", "lib/itcl*",
                        "lib/thread*", "lib/python3.12/lib-dynload/_tkinter*"):
            for path in prepared.glob(pattern):
                shutil.rmtree(path) if path.is_dir() else path.unlink()
        locked = temporary / "requirements.txt"
        locked.write_text(requirements)
        python = executable(prepared)
        run(["uv", "pip", "install", "--python", python, "--require-hashes", "--no-build",
             "--no-python-downloads", "--python-platform",
             "aarch64-apple-darwin" if sys.platform == "darwin" else "x86_64-manylinux_2_28",
             "-r", locked])
        # Polars needs one CPU runtime: the compatible one, not the default AVX2 build.
        run(["uv", "pip", "uninstall", "--python", python, "polars-runtime-32"])
        site = prepared / SITE
        shutil.copy2(PROJECT / "report.py", site / "asterion_analytics.py")
        shutil.copytree(build / "generated-python/asterion", site / "asterion")
        # Installed command scripts name the staging interpreter, and nothing
        # installs packages into a prepared runtime: keep the interpreter only.
        for path in (prepared / "bin").iterdir():
            if path.name not in ("python3", "python3.12"):
                path.unlink()
        for path in [prepared / "share", prepared / "lib/pkgconfig", *site.glob("pip"),
                     *site.glob("pip-*.dist-info"), *prepared.glob("lib/python3.12/config-*"),
                     *prepared.rglob("__pycache__")]:
            shutil.rmtree(path)
        (prepared / "manifest.json").write_text(json.dumps(
            {"platform": [sys.platform, platform.machine()], "python": PYTHON}, indent=2) + "\n")
        verify(prepared)
        # The relocated runtime imports Terminal's Python code without a system Python.
        run([python, "-I", "-B", "-c", "import asterion_analytics"])
        DESTINATION.parent.mkdir(parents=True, exist_ok=True)
        publish_tree(prepared, DESTINATION)
    print(f"Prepared offline Python runtime: {DESTINATION}")


if __name__ == "__main__":
    prepare()
