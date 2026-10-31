"""Exercise the installer's application with disposable data.

macOS copies the DMG application into an isolated directory, never /Applications.
Linux installs the archive's Terminal with the installer's own executor into a
disposable home, runs the installed Terminal, then uninstalls it with its user
data and checks that nothing is left.
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
import tarfile

ROOT = Path(__file__).resolve().parents[2]


def run(args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, timeout=300, **kwargs)


def maintain(executor, home, operation, source=None, remove=False):
    """One installer operation, run as the installer window runs it."""
    destination = home / ".local/opt/asterion-terminal"
    args = [executor, "--operation", operation, "--destination", destination]
    if source:
        args += ["--source", source]
    if remove:
        args.append("--remove-user-data")
    env = {key: value for key, value in os.environ.items() if not key.startswith("ASTERION_")}
    env.update(HOME=str(home), ASTERION_ENVIRONMENT="production")
    env.pop("XDG_DATA_HOME", None)
    reply = subprocess.run([str(arg) for arg in args], env=env, capture_output=True, text=True,
                           timeout=300)
    lines = [json.loads(line) for line in reply.stdout.splitlines() if line.startswith("{")]
    if reply.returncode or not lines or "result" not in lines[-1]:
        raise RuntimeError(f"installer {operation} failed: {reply.stdout}{reply.stderr}")
    return lines[-1]["result"], destination


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
    expected = {"darwin": ".dmg", "linux": ".gz"}.get(sys.platform)
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
                installed = installed / "Contents/Resources/terminal.app"
                exercise(installed / "Contents/MacOS/asterion_terminal",
                         installed / "Contents/Resources", output)
            else:
                with tarfile.open(installer) as archive:
                    archive.extractall(folder, filter="data")
                apps = list(folder.rglob("asterion_installer"))
                if len(apps) != 1:
                    raise RuntimeError("Archive must contain exactly one installer")
                package = apps[0].parent / "resources/terminal"
                executor = package / "resources/native/asterion_maintenance"
                home = folder / "home"
                home.mkdir()
                state, _ = maintain(executor, home, "inspect", package)
                if state["installed"] or state["deployment_action"] != "install":
                    raise RuntimeError(f"unexpected state before install: {state}")
                state, destination = maintain(executor, home, "install", package)
                launcher = home / ".local/share/applications/asterion-terminal.desktop"
                root = home / ".local/share/asterion"
                if not state["installed"] or not launcher.is_file() or not (
                        root / "terminal/config/logging.json").is_file():
                    raise RuntimeError(f"install left an incomplete installation: {state}")
                exercise(destination / "asterion_terminal", destination / "resources", output)
                state, _ = maintain(executor, home, "uninstall", remove=True)
                left = [path for path in root.rglob("*") if path.is_file()
                        and "logs" not in path.parts
                        and path.name not in ("installation.lock", "terminal.lock")]
                if state["installed"] or destination.exists() or launcher.exists() or left:
                    raise RuntimeError(f"uninstall left files behind: {left or state}")
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
        "checks": [*(["install, launcher and first configuration through the installer",
                      "uninstall with user data removal leaves only logs and locks"]
                     if sys.platform == "linux" else []),
                   "startup", "workspaces and last-workspace restoration", "125% native Chromium zoom",
                   "renderer isolation", "shortcuts", "settings reuse and bounds",
                   "locale synchronization", "retained draft", "dialog IPC adapter",
                   "local services page", "separate data-source settings",
                   "minute and daily history paging and exact decimals",
                   "daily chart aggregation and MACD",
                   "daily download to factor analysis navigation",
                   "daily factor holdout results and restart recovery",
                   "backtest data, rules, review, execution and fixed results"],
        "limits": ["disposable test profile", "no live market credentials",
                   "dialog adapter mocked; not manual native picker acceptance",
                   *(["installer executor run directly; the installer window is not driven"]
                     if sys.platform == "linux" else [])],
    }, indent=2) + "\n")
    print(f"Installed desktop acceptance passed: {report}")


if __name__ == "__main__":
    main()
