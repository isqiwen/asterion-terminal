"""Build the small installer payload and its separately hosted PostgreSQL archive."""

import gzip
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UV_VERSION = "0.11.31"
PYTHON_VERSION = "3.12.12"


def sha(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fetch(url: str, cache: Path) -> Path:
    path = cache / hashlib.sha256(url.encode()).hexdigest()
    if not path.exists():
        temporary = path.with_suffix(".part")
        print(f"Downloading release input: {url}", flush=True)
        with urllib.request.urlopen(url, timeout=120) as source, temporary.open("wb") as target:
            shutil.copyfileobj(source, target)
        temporary.replace(path)
    return path


def artifact(url: str, path: Path) -> dict:
    return {"url": url, "sha256": sha(path), "size": path.stat().st_size}


def prepare(release_url: str) -> Path:
    if not release_url.startswith("https://"):
        raise ValueError("Runtime release URL must use HTTPS")
    cache = ROOT / ".state/runtime-downloads"
    cache.mkdir(parents=True, exist_ok=True)
    bundle = ROOT / "apps/terminal/src-tauri/setup"
    bundle.mkdir(exist_ok=True)
    env = os.environ | {"UV_CACHE_DIR": str(ROOT / ".state/uv-cache")}
    host, arch = sys.platform, platform.machine()
    rust_arch = {"arm64": "aarch64", "x86_64": "x86_64", "aarch64": "aarch64"}[arch]
    triple = f"{rust_arch}-" + ("apple-darwin" if host == "darwin" else "unknown-linux-gnu")
    uv_url = f"https://github.com/astral-sh/uv/releases/download/{UV_VERSION}/uv-{triple}.tar.gz"
    uv_archive = fetch(uv_url, cache)
    listing = json.loads(
        subprocess.check_output(
            ["uv", "python", "list", "--all-versions", "--output-format", "json"], env=env
        )
    )
    python = next(
        row
        for row in listing
        if row["version"] == PYTHON_VERSION
        and row["variant"] == "default"
        and row["implementation"] == "cpython"
        and row["arch"] in {arch, rust_arch}
        and row["url"]
    )
    python["url"] = python["url"].replace(
        "https://releases.astral.sh/github/", "https://github.com/astral-sh/"
    )
    python_archive = fetch(python["url"], cache)
    # Keep upstream archives as separate downloads; only their identities enter the app.
    with tempfile.TemporaryDirectory(dir=ROOT / ".state", prefix="runtime-release-") as directory:
        stage = Path(directory)
        published = None
        postgres = {
            "source": "system",
            "root": "/usr",
            "executable": "lib/postgresql/17/bin/postgres",
        }
        if host == "darwin":
            from bundle_postgres import bundle as bundle_pg
            from desktop_dependencies import mac_postgres_source

            pg = stage / "postgres"
            bundle_pg(mac_postgres_source(), pg)
            archive = stage / "postgres.tar.gz"

            def metadata(info):
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mtime = 0
                return info

            with (
                archive.open("wb") as raw,
                gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as compressed,
                tarfile.open(fileobj=compressed, mode="w", dereference=True) as target,
            ):
                target.add(pg, arcname=".", filter=metadata)
            # Content-addressed asset names make a rebuild unable to silently replace an input.
            filename = f"postgres-17-{host}-{rust_arch}-{sha(archive)[:16]}.tar.gz"
            release = ROOT / "release/runtime"
            release.mkdir(parents=True, exist_ok=True)
            published = release / filename
            shutil.copy2(archive, published)
            postgres = {
                "source": "archive",
                "artifact": artifact(f"{release_url.rstrip('/')}/{filename}", published),
                "executable": "bin/postgres",
            }
        subprocess.run(
            ["uv", "build", "--wheel", "--out-dir", str(stage / "wheel")],
            cwd=ROOT,
            env=env,
            check=True,
        )
        wheel = next((stage / "wheel").glob("*.whl"))
        shutil.copy2(wheel, bundle / wheel.name)
        subprocess.run(
            [
                "uv",
                "export",
                "--locked",
                "--no-dev",
                "--no-group",
                "packaging",
                "--no-emit-project",
                "--format",
                "requirements-txt",
                "--output-file",
                str(bundle / "requirements.txt"),
            ],
            cwd=ROOT,
            env=env,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        manifest = {
            "schema": 1,
            "platform": "macos" if host == "darwin" else host,
            "architecture": rust_arch,
            "uv": artifact(uv_url, uv_archive),
            "python": artifact(python["url"], python_archive),
            "postgres": postgres,
            "uv_executable": f"uv-{triple}/uv",
            "python_version": PYTHON_VERSION,
            "wheel": wheel.name,
            "wheel_sha256": sha(bundle / wheel.name),
            "requirements_sha256": sha(bundle / "requirements.txt"),
        }
        (bundle / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        # Seed the independent test cache with the exact release inputs. Production still downloads.
        test_cache = ROOT / ".state/setup-smoke/cache"
        test_cache.mkdir(parents=True, exist_ok=True)
        for path in (uv_archive, python_archive, *([published] if published else [])):
            shutil.copy2(path, test_cache / sha(path))
        if published:
            print(f"Runtime release attachment: {published}", flush=True)
        else:
            print("PostgreSQL 17 is provided by the system package manager.", flush=True)
    return bundle
