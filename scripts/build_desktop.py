"""Reproducible macOS app build. Build dependencies never become launch requirements."""

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    subprocess.run(list(args), cwd=ROOT, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--skip-runtime", action="store_true")
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise SystemExit("This build currently targets macOS only")
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
        run("uv", "run", "python", "scripts/bundle_postgres.py")
    if not args.prepare_only:
        env = os.environ.copy()
        toolchain = ROOT / ".state/toolchain"
        if (toolchain / "cargo/bin/cargo").exists():
            env.update(CARGO_HOME=str(toolchain / "cargo"), RUSTUP_HOME=str(toolchain / "rustup"))
            env["PATH"] = f"{toolchain}/cargo/bin:/usr/bin:/bin:" + env["PATH"]
        run("pnpm", "--filter", "@asterion/terminal", "tauri", "build", "--bundles", "app", env=env)

        bundle = ROOT / "apps/terminal/src-tauri/target/release/bundle/macos/Asterion Terminal.app"
        run("/usr/bin/codesign", "--force", "--deep", "--sign", "-", str(bundle))
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", str(bundle))


if __name__ == "__main__":
    main()
