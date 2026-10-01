"""Exercise the macOS DMG with disposable UI/Agent data.

macOS copies the DMG application into an isolated directory, never /Applications.
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

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, timeout=300, **kwargs)


def exercise(program, screenshots):
    if not program.is_file():
        raise RuntimeError(f"Installed desktop executable is missing: {program}")
    env = dict(os.environ, ASTERION_TEST_ELECTRON=str(program),
               ASTERION_UI_SCREENSHOTS=str(screenshots))
    # Fixtures persist provider digests. Use the installed, re-signed
    # plugins, whose bytes differ from the build-tree libraries.
    plugins = program.parent.parent / "Resources/native/plugins"
    if not plugins.is_dir():
        raise RuntimeError(f"Installed plugin directory is missing: {plugins}")
    env["ASTERION_PLUGIN_DIRECTORY"] = str(plugins)
    node = shutil.which("node")
    if not node:
        raise RuntimeError("Node.js is required for installed desktop acceptance")
    command = [node, str(ROOT / "tests/electron_desktop.cjs")]
    run(command, cwd=ROOT, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("installer", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/installed-desktop")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("Installed Terminal acceptance requires macOS")
    installer = args.installer.resolve(strict=True)
    if installer.suffix != ".dmg":
        parser.error("macOS acceptance requires a .dmg installer")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / "acceptance.json"
    # Never leave a prior pass looking like evidence for a failed rerun.
    report.write_text(json.dumps({"status": "running", "installer": str(installer)}, indent=2))
    try:
        with tempfile.TemporaryDirectory(prefix="asterion-installed-") as temporary:
            folder = Path(temporary)
            mount = folder / "volume"
            mount.mkdir()
            run(["hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", mount, installer])
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
            exercise(installed / "Contents/MacOS/asterion-terminal", output)
    except Exception as error:
        report.write_text(json.dumps({"status": "failed", "installer": str(installer),
                                      "error": str(error)}, indent=2) + "\n")
        raise
    with installer.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    report.write_text(json.dumps({
        "status": "passed",
        "platform": sys.platform,
        "installer": str(installer),
        "bytes": installer.stat().st_size,
        "sha256": digest,
        "checks": ["startup", "125% native Chromium zoom", "renderer isolation", "shortcuts", "settings reuse and bounds",
                   "locale synchronization", "retained draft", "dialog IPC adapter",
                   "minute and daily history paging and exact decimals",
                   "daily chart aggregation and MACD",
                   "daily download to factor analysis navigation",
                   "Agent-dispatched daily factor holdout results and restart recovery",
                   "risk rejection", "fills", "desktop restart and ledger restoration",
                   "Agent survives desktop exit"],
        "limits": ["disposable test profile", "no live market credentials",
                   "dialog adapter mocked; not manual native picker acceptance"],
    }, indent=2) + "\n")
    print(f"Installed desktop acceptance passed: {report}")


if __name__ == "__main__":
    main()
