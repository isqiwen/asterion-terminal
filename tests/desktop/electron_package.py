"""Run the packaged Terminal with disposable data.

macOS copies the DMG's application into a temporary directory, never
/Applications; Linux unpacks the .deb there without installing it.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, timeout=900, **kwargs)


def exercise(program, screenshots):
    if not program.is_file():
        raise RuntimeError(f"Packaged desktop executable is missing: {program}")
    env = dict(os.environ, ASTERION_TEST_ELECTRON=str(program),
               ASTERION_UI_SCREENSHOTS=str(screenshots))
    node = shutil.which("node")
    if not node:
        raise RuntimeError("Node.js is required for packaged desktop acceptance")
    run([sys.executable, ROOT / "tests/support/e2e.py", node, ROOT / "tests/desktop/electron_desktop.cjs"],
        cwd=ROOT, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/installed-desktop")
    args = parser.parse_args()
    expected = {"darwin": ".dmg", "linux": ".deb"}.get(sys.platform)
    if expected is None:
        parser.error("Packaged Terminal acceptance requires macOS or Linux")
    package = args.package.resolve(strict=True)
    if package.suffix != expected:
        parser.error(f"This platform's acceptance requires a {expected} package")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / "acceptance.json"
    # Never leave a prior pass looking like evidence for a failed rerun.
    report.write_text(json.dumps({"status": "running", "package": str(package)}, indent=2))
    try:
        with tempfile.TemporaryDirectory(prefix="asterion-installed-") as temporary:
            folder = Path(temporary)
            if sys.platform == "darwin":
                mount = folder / "volume"
                mount.mkdir()
                run(["hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", mount, package])
                try:
                    apps = list(mount.glob("*.app"))
                    if len(apps) != 1:
                        raise RuntimeError("DMG must contain exactly one application")
                    installed = folder / apps[0].name
                    shutil.copytree(apps[0], installed, symlinks=True)
                finally:
                    run(["hdiutil", "detach", mount])
                # Verify and launch the copied app after unmount, not the build tree.
                run(["codesign", "--verify", "--deep", "--strict", installed])
                exercise(installed / "Contents/MacOS/asterion_terminal", output)
            else:
                run(["dpkg-deb", "-x", package, folder])
                programs = list(folder.rglob("asterion_terminal"))
                if len(programs) != 1:
                    raise RuntimeError("Package must contain exactly one Terminal program")
                exercise(programs[0], output)
    except Exception as error:
        report.write_text(json.dumps({"status": "failed", "package": str(package),
                                      "error": str(error)}, indent=2) + "\n")
        raise
    with package.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    report.write_text(json.dumps({
        "status": "passed",
        "platform": sys.platform,
        "package": str(package),
        "bytes": package.stat().st_size,
        "sha256": digest,
        "checks": ["startup", "workspaces and last-workspace restoration", "125% native Chromium zoom",
                   "renderer isolation", "shortcuts", "settings reuse and bounds",
                   "locale synchronization", "retained draft", "dialog IPC adapter",
                   "Market connection", "research page on the lab service"],
        "limits": ["disposable test profile", "test Market, no live market credentials",
                   "dialog adapter mocked; not manual native picker acceptance",
                   *(["package unpacked, not installed with apt"] if sys.platform == "linux" else [])],
    }, indent=2) + "\n")
    print(f"Packaged desktop acceptance passed: {report}")


if __name__ == "__main__":
    main()
