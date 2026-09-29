"""Exercise the desktop from an actual native installer, with disposable UI/Agent data.

Linux/Windows install into the OS and require an explicitly disposable test machine.
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
    node = shutil.which("node")
    if not node:
        raise RuntimeError("Node.js is required for installed desktop acceptance")
    command = [node, str(ROOT / "tests/electron_desktop.cjs")]
    if sys.platform == "linux":
        command = ["xvfb-run", "-a", *command]
    run(command, cwd=ROOT, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("installer", type=Path)
    parser.add_argument("--allow-system-install", action="store_true",
                        help="Only use on a disposable Linux/Windows test machine")
    parser.add_argument("--output", type=Path, default=ROOT / "build/installed-desktop")
    args = parser.parse_args()
    installer = args.installer.resolve(strict=True)
    expected = {"darwin": ".dmg", "linux": ".deb", "win32": ".exe"}.get(sys.platform)
    if installer.suffix != expected:
        parser.error(f"This platform requires a {expected} installer")
    if sys.platform != "darwin" and not args.allow_system_install:
        parser.error("Linux/Windows acceptance requires --allow-system-install on a disposable machine")
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
                exercise(installed / "Contents/MacOS/asterion-terminal", output)
            elif sys.platform == "linux":
                package = subprocess.check_output(
                    ["dpkg-deb", "--field", str(installer), "Package"], text=True).strip()
                if package != "asterion-terminal":
                    raise RuntimeError(f"Unexpected Debian package identity: {package}")
                run(["sudo", "-n", "apt-get", "install", "-y", "--reinstall", installer])
                exercise(Path("/usr/bin/asterion-terminal"), output)
            else:
                installed = folder / "app"
                # Assisted NSIS does not start the app on a silent install without --force-run.
                # The explicit directory must be the last installer parameter.
                run([installer, "/S", "/currentuser", f"/D={installed}"])
                try:
                    exercise(installed / "asterion-terminal.exe", output)
                finally:
                    uninstallers = list(installed.glob("Uninstall*.exe"))
                    if len(uninstallers) != 1:
                        raise RuntimeError("Cannot locate installed NSIS uninstaller for cleanup")
                    uninstaller = folder / "uninstall.exe"
                    shutil.copy2(uninstallers[0], uninstaller)
                    run([uninstaller, "/S", "/currentuser", f"_?={installed}"])
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
