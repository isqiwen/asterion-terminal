"""Exercise the installer's application with disposable UI/Agent data.

macOS copies the DMG application into an isolated directory, never /Applications.
Linux unpacks the Debian package into an isolated directory and never installs it.
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
    subprocess.run([str(arg) for arg in args], check=True, timeout=300, **kwargs)


def exercise(program, resources, screenshots):
    if not program.is_file():
        raise RuntimeError(f"Installed desktop executable is missing: {program}")
    env = dict(os.environ, ASTERION_TEST_ELECTRON=str(program),
               ASTERION_UI_SCREENSHOTS=str(screenshots))
    # Fixtures persist provider digests. Use the installed plugins: on macOS
    # they are re-signed, so their bytes differ from the build-tree libraries.
    plugins = resources / "native/plugins"
    if not plugins.is_dir():
        raise RuntimeError(f"Installed plugin directory is missing: {plugins}")
    env["ASTERION_PLUGIN_DIRECTORY"] = str(plugins)
    node = shutil.which("node")
    if not node:
        raise RuntimeError("Node.js is required for installed desktop acceptance")
    command = [node, str(ROOT / "tests/desktop/electron_desktop.cjs")]
    run(command, cwd=ROOT, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("installer", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/installed-desktop")
    args = parser.parse_args()
    expected = {"darwin": ".dmg", "linux": ".deb"}.get(sys.platform)
    if expected is None:
        parser.error("Installed Terminal acceptance requires macOS or Linux")
    installer = args.installer.resolve(strict=True)
    if installer.suffix != expected:
        parser.error(f"This platform's acceptance requires a {expected} installer")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / "acceptance.json"
    # Never leave a prior pass looking like evidence for a failed rerun.
    report.write_text(json.dumps({"status": "running", "installer": str(installer)}, indent=2))
    try:
        with tempfile.TemporaryDirectory(prefix="asterion-installed-") as temporary:
            folder = Path(temporary)
            if sys.platform == "darwin":
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
                exercise(installed / "Contents/MacOS/asterion-terminal",
                         installed / "Contents/Resources", output)
            else:
                run(["dpkg-deb", "--extract", installer, folder / "root"])
                apps = [path for path in (folder / "root/opt").iterdir() if path.is_dir()]
                if len(apps) != 1:
                    raise RuntimeError("Debian package must contain exactly one application")
                exercise(apps[0] / "asterion-terminal", apps[0] / "resources", output)
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
        "checks": ["startup", "six workspaces and last-workspace restoration", "125% native Chromium zoom", "renderer isolation", "shortcuts", "settings reuse and bounds",
                   "locale synchronization", "retained draft", "dialog IPC adapter",
                   "deployment overview and wizard at 125% zoom", "separate data-source settings",
                   "minute and daily history paging and exact decimals",
                   "daily chart aggregation and MACD",
                   "daily download to factor analysis navigation",
                   "Agent-dispatched daily factor holdout results and restart recovery",
                   "backtest data, rules, review, execution and fixed results",
                   "Agent survives desktop exit"],
        "limits": ["disposable test profile", "no live market credentials",
                   "dialog adapter mocked; not manual native picker acceptance",
                   *(["unpacked, not installed: package maintainer scripts not exercised"]
                     if sys.platform == "linux" else [])],
    }, indent=2) + "\n")
    print(f"Installed desktop acceptance passed: {report}")


if __name__ == "__main__":
    main()
