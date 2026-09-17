"""Native macOS/Linux app build, including Python and PostgreSQL runtimes."""

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from desktop_dependencies import LINUX_PACKAGES, ensure_linux, ensure_macos, mac_postgres_source

ROOT = Path(__file__).resolve().parents[1]


def dependency_hint(host: str) -> str:
    if host == "linux":
        return (
            "On Debian 13, install the system build dependencies once:\n"
            "  sudo apt update\n"
            f"  sudo apt install {' '.join(LINUX_PACKAGES)}\n"
            "libpq-dev provides pg_config. Other distributions: see README.md.\n"
            "Then rerun Asterion: Build as your normal user."
        )
    return "Install the build prerequisites listed in README.md, then rerun Asterion: Build."


def run(*args, **kwargs):
    subprocess.run(list(args), cwd=ROOT, check=True, **kwargs)


def bundle_diagnostics() -> str:
    """Report unreadable payload entries while preserving the failed bundle for inspection."""
    directory = ROOT / "apps/terminal/src-tauri/target/release/bundle/deb"
    problems = []
    for path in directory.rglob("*"):
        try:
            if not path.is_dir():
                with path.open("rb") as file:
                    file.read(1)
        except OSError as error:
            problems.append(f"  {path}: {error}")
            if len(problems) == 5:
                break
    if not directory.exists():
        problems.append(f"  Missing bundle directory: {directory}")
    return (
        "\n".join(problems)
        or f"No unreadable files remain. Inspect the build output and {directory}."
    )


def export_artifact(source: Path) -> Path:
    """Publish a completed bundle without copying compiler caches or stale files."""
    if not source.exists():
        raise FileNotFoundError(f"Build artifact not found: {source}")
    release = ROOT / "release"
    release.mkdir(exist_ok=True)
    destination = release / "_".join(source.name.split())
    with tempfile.TemporaryDirectory(prefix=".export-", dir=release) as temporary:
        staged = Path(temporary) / source.name
        if source.is_dir():
            shutil.copytree(source, staged, symlinks=True)
            if destination.is_dir() and not destination.is_symlink():
                shutil.rmtree(destination)
        else:
            shutil.copy2(source, staged)
        staged.replace(destination)
    print(f"\nRelease artifact: {destination}", flush=True)
    return destination


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--skip-runtime", action="store_true")
    parser.add_argument("--platform", choices=["auto", "macos", "linux"], default="auto")
    parser.add_argument("--pg-config", default=os.environ.get("PG_CONFIG", "pg_config"))
    parser.add_argument("--smoke-test", action="store_true")
    parser.add_argument(
        "--no-install", action="store_true", help="Check dependencies without offering installation"
    )
    args = parser.parse_args()
    host = {"darwin": "macos", "linux": "linux"}.get(sys.platform)
    if host is None or args.platform not in {"auto", host}:
        raise SystemExit("Build on the target OS: macOS for .app, Linux for .deb")
    if os.geteuid() == 0:
        raise SystemExit("Run the build as a normal user. Only apt installation uses sudo.")
    toolchain = ROOT / ".state/toolchain"
    required = ["uv"]
    if not args.prepare_only:
        required += ["pnpm"]
        if not (toolchain / "cargo/bin/cargo").exists():
            required += ["cargo"]
    missing = [tool for tool in required if shutil.which(tool) is None]
    if missing:
        raise SystemExit(
            "Install the developer tools first: " + ", ".join(missing) + "; see README.md"
        )
    if not args.no_install or host == "macos":
        ensure = ensure_linux if host == "linux" else ensure_macos
        ensure(
            runtime=not args.skip_runtime,
            desktop=not args.prepare_only,
            interactive=not args.no_install,
        )
    if host == "linux" and not args.skip_runtime:
        required += [args.pg_config, "patchelf", "ldd"]
    if host == "linux" and not args.prepare_only:
        required += ["pkg-config", "dpkg-deb"]
    missing = [tool for tool in required if shutil.which(tool) is None]
    if missing:
        raise SystemExit(
            "Missing build tools: " + ", ".join(missing) + "\n" + dependency_hint(host)
        )
    if host == "linux" and not args.prepare_only:
        result = subprocess.run(
            ["pkg-config", "--exists", "webkit2gtk-4.1", "gtk+-3.0", "openssl"],
            check=False,
        )
        if result.returncode:
            raise SystemExit("Missing Linux development libraries.\n" + dependency_hint(host))
    runtime = ROOT / "apps/terminal/src-tauri/runtime"
    stamp = runtime / "build-platform.json"
    identity = {"platform": host, "architecture": platform.machine()}
    if args.skip_runtime:
        if not stamp.exists() or json.loads(stamp.read_text()) != identity:
            raise SystemExit("Runtime missing or built for another platform; remove --skip-runtime")
    else:
        stamp.unlink(missing_ok=True)
    (ROOT / ".state").mkdir(exist_ok=True)
    if not args.skip_runtime:
        run(
            "uv",
            "run",
            "--group",
            "packaging",
            "pyinstaller",
            "--noconfirm",
            "--clean",
            "--onedir",
            "--name",
            "asterion-backend",
            "--distpath",
            "apps/terminal/src-tauri/runtime",
            "--workpath",
            ".state/pyinstaller",
            "--specpath",
            ".state",
            "--collect-all",
            "pyarrow",
            "--collect-all",
            "psycopg",
            "--collect-all",
            "psycopg_binary",
            "--hidden-import",
            "sqlalchemy.dialects.postgresql.psycopg",
            "--hidden-import",
            "uvicorn.logging",
            "--hidden-import",
            "uvicorn.loops.auto",
            "--hidden-import",
            "uvicorn.protocols.http.auto",
            "--hidden-import",
            "uvicorn.protocols.websockets.auto",
            "--hidden-import",
            "uvicorn.lifespan.on",
            "scripts/backend_entry.py",
        )
        if host == "macos":
            run(
                "uv",
                "run",
                "python",
                "scripts/bundle_postgres.py",
                "--source",
                str(mac_postgres_source()),
            )
        else:
            run(
                "uv",
                "run",
                "python",
                "scripts/bundle_postgres_linux.py",
                "--pg-config",
                args.pg_config,
            )
        stamp.write_text(json.dumps(identity) + "\n")
    if args.smoke_test:
        run("uv", "run", "python", "scripts/smoke_desktop_runtime.py")
    if not args.prepare_only:
        env = os.environ.copy()
        if (toolchain / "cargo/bin/cargo").exists():
            env.update(CARGO_HOME=str(toolchain / "cargo"), RUSTUP_HOME=str(toolchain / "rustup"))
            env["PATH"] = f"{toolchain}/cargo/bin:/usr/bin:/bin:" + env["PATH"]
        try:
            run(
                "pnpm",
                "--filter",
                "@asterion/terminal",
                "tauri",
                "build",
                "--bundles",
                "app" if host == "macos" else "deb",
                env=env,
            )
        except subprocess.CalledProcessError as error:
            detail = bundle_diagnostics() if host == "linux" else "See the build output above."
            raise SystemExit(
                f"Desktop build failed (exit {error.returncode}).\n{detail}\n"
                "Existing release artifacts were not updated."
            ) from None

        if host == "macos":
            bundle = (
                ROOT / "apps/terminal/src-tauri/target/release/bundle/macos/Asterion Terminal.app"
            )
            run("/usr/bin/codesign", "--force", "--deep", "--sign", "-", str(bundle))
            run("/usr/bin/codesign", "--verify", "--deep", "--strict", str(bundle))
        else:
            config = json.loads((ROOT / "apps/terminal/src-tauri/tauri.conf.json").read_text())
            architecture = subprocess.check_output(
                ["dpkg", "--print-architecture"], text=True
            ).strip()
            filename = f"{config['productName']}_{config['version']}_{architecture}.deb"
            bundle = ROOT / "apps/terminal/src-tauri/target/release/bundle/deb" / filename
        export_artifact(bundle)


if __name__ == "__main__":
    main()
