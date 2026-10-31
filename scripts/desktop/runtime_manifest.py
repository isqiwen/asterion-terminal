#!/usr/bin/env python3
"""The Python runtime the Terminal installs at its first launch (M1.11).

Writes build/electron-resources/runtime/: manifest.json, naming the
interpreter archive and every wheel of the lab's lock for this platform with
its SHA-256 and size, grouped into the modules the first launch shows; and
wheels/ with the few packages PyPI publishes no wheel of, built here once.
The Terminal downloads the rest from a mirror or the official source and
installs nothing it cannot verify against this file.

    python3 scripts/desktop/runtime_manifest.py
"""
import hashlib
import json
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "apps/services/lab"
DESTINATION = ROOT / "build/electron-resources/runtime"
PYTHON = "3.12.12"
# Where python-build-standalone publishes; uv's own copy is what it resolves to.
INTERPRETER_SOURCES = {
    "official": "https://github.com/astral-sh/python-build-standalone/releases/download/",
    "mirror": "https://registry.npmmirror.com/-/binary/python-build-standalone/",
}
UV_RELEASES = "https://releases.astral.sh/github/python-build-standalone/releases/download/"
PYPI = "https://files.pythonhosted.org/packages/"
# What the first launch shows, in order: each module is what its roots need
# beyond the modules before it.
MODULES = [
    ("core", "核心", ["protobuf", "duckdb", "numpy", "polars", "certifi"]),
    ("reports", "报表", ["quantstats"]),
    ("portfolio", "组合优化", ["skfolio"]),
    ("akshare", "AKShare 数据源", ["akshare"]),
]
# Polars needs one CPU runtime: the compatible one, not the AVX2 default.
EXCLUDED = {"polars-runtime-32"}
TARGETS = {
    ("darwin", "arm64"): {"sys_platform": "darwin", "platform_machine": "arm64",
                          "interpreter": "macos-aarch64-none"},
    ("linux", "x86_64"): {"sys_platform": "linux", "platform_machine": "x86_64",
                          "interpreter": "linux-x86_64-gnu"},
}


def normal(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def applies(marker: str | None, target: dict) -> bool:
    """Whether an environment marker of the lock holds on the target. The lock
    names only the platform, the machine and the implementation."""
    if not marker:
        return True
    names = {"sys_platform": target["sys_platform"], "platform_machine": target["platform_machine"],
             "implementation_name": "cpython"}
    if set(re.findall(r"[a-z_]+(?= *[=!]=)", marker)) - set(names):
        raise SystemExit(f"unsupported marker: {marker}")
    return bool(eval(marker, {"__builtins__": {}}, names))


def wheel_rank(filename: str, target: dict):
    """How well a wheel fits the target, higher better; None when it does not."""
    _, _, python, abi, platform_tag = filename[:-4].rsplit("-", 4)
    if not any(tag in ("cp312", "py3", "py2.py3") or re.fullmatch(r"cp3(\d+)", tag) and abi == "abi3"
               and int(tag[3:]) <= 12 for tag in python.split(".")):
        return None
    if abi not in ("cp312", "abi3", "none"):
        return None
    best = None
    for tag in platform_tag.split("."):
        if tag == "any":
            score = 0
        elif target["sys_platform"] == "darwin":
            match = re.fullmatch(r"macosx_(\d+)_(\d+)_(arm64|universal2)", tag)
            # The application supports macOS 13 and later.
            score = 1 if match and (int(match[1]), int(match[2])) <= (13, 0) else None
        else:
            legacy = {"manylinux1_x86_64": (2, 5), "manylinux2010_x86_64": (2, 12),
                      "manylinux2014_x86_64": (2, 17)}
            match = re.fullmatch(r"manylinux_(\d+)_(\d+)_x86_64", tag)
            version = (int(match[1]), int(match[2])) if match else legacy.get(tag)
            # Built on Ubuntu 24.04's glibc floor the release supports: 2.28.
            score = 1 if version and version <= (2, 28) else None
        if score is not None and (best is None or score > best):
            best = score
    if best is None:
        return None
    return (abi == "cp312", abi == "abi3", best)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            value.update(block)
    return value.hexdigest()


def interpreter(target: dict, staging: Path) -> dict:
    listed = json.loads(subprocess.run(
        ["uv", "python", "list", PYTHON, "--only-downloads", "--all-platforms", "--output-format", "json"],
        check=True, capture_output=True, text=True).stdout)
    key = f"cpython-{PYTHON}-{target['interpreter']}"
    item = next((entry for entry in listed if entry["key"] == key), None)
    if item is None or not item["url"].startswith(UV_RELEASES):
        raise SystemExit(f"uv publishes no {key}")
    path = item["url"][len(UV_RELEASES):]
    archive = staging / "interpreter.tar.gz"
    # The build reads the archive once from the official source and pins it.
    with urllib.request.urlopen(INTERPRETER_SOURCES["official"] + path) as response, \
            archive.open("wb") as output:
        shutil.copyfileobj(response, output)
    return {"path": path, "sha256": digest(archive), "size": archive.stat().st_size}


def verify(directory=DESTINATION):
    """What an application ships for its first launch: the manifest for this
    platform, the wheels it bundles as recorded, and its own Python code."""
    manifest_file = directory / "manifest.json"
    if not manifest_file.is_file():
        raise ValueError("Runtime manifest missing; run pnpm prepare:python")
    manifest = json.loads(manifest_file.read_text())
    if manifest["platform"] != [sys.platform, platform.machine()]:
        raise ValueError("Runtime manifest is for another platform; run pnpm prepare:python")
    for module in manifest["modules"]:
        for item in module["files"]:
            if item.get("bundled") and digest(directory / "wheels" / item["file"]) != item["sha256"]:
                raise ValueError(f"Bundled wheel differs from the manifest: {item['file']}")
    application = directory / "app"
    if not (application / "asterion_lab" / "__main__.py").is_file() or \
            not list(application.glob("asterion_engine.*")):
        raise ValueError("The application's Python code is missing beside the runtime manifest")


def main():
    target = TARGETS.get((sys.platform, platform.machine()))
    if target is None:
        raise SystemExit("The runtime manifest is built on macOS arm64 or Linux x86_64")
    lock = tomllib.loads((PROJECT / "uv.lock").read_text())
    packages = {normal(item["name"]): item for item in lock["package"]}
    exported = subprocess.run(
        ["uv", "export", "--project", str(PROJECT), "--frozen", "--no-dev", "--no-emit-project",
         "--no-hashes", "--quiet"], check=True, capture_output=True, text=True).stdout
    wanted = {}
    for line in exported.splitlines():
        if not line or line.startswith(("#", " ")):
            continue
        requirement, _, marker = line.partition(" ; ")
        name, _, version = requirement.partition("==")
        if applies(marker or None, target) and normal(name) not in EXCLUDED:
            wanted[normal(name)] = version
    # Each module: its roots and everything they need, less earlier modules.
    owner = {}
    for module, _, roots in MODULES:
        pending = [normal(root) for root in roots]
        while pending:
            name = pending.pop()
            if name in owner or name not in wanted:
                continue
            owner[name] = module
            for dependency in packages[name].get("dependencies", []):
                pending.append(normal(dependency["name"]))
            for dependencies in packages[name].get("optional-dependencies", {}).values():
                pending.extend(normal(item["name"]) for item in dependencies)
    missing = sorted(set(wanted) - set(owner))
    if missing:
        raise SystemExit(f"packages no module needs: {', '.join(missing)}")
    shutil.rmtree(DESTINATION, ignore_errors=True)
    (DESTINATION / "wheels").mkdir(parents=True)
    modules = {module: {"id": module, "title": title, "files": []} for module, title, _ in MODULES}
    with tempfile.TemporaryDirectory() as temporary:
        staging = Path(temporary)
        for name, version in sorted(wanted.items()):
            item = packages[name]
            if item["version"] != version:
                raise SystemExit(f"{name}: the export and the lock disagree")
            ranked = [(wheel_rank(wheel["url"].rsplit("/", 1)[1], target), wheel)
                      for wheel in item.get("wheels", [])]
            ranked = [entry for entry in ranked if entry[0] is not None]
            files = modules[owner[name]]["files"]
            if ranked:
                wheel = max(ranked, key=lambda entry: entry[0])[1]
                if not wheel["url"].startswith(PYPI):
                    raise SystemExit(f"{name}: not on PyPI")
                files.append({"name": name, "file": wheel["url"].rsplit("/", 1)[1],
                              "path": wheel["url"][len(PYPI):], "sha256": wheel["hash"].removeprefix("sha256:"),
                              "size": wheel["size"]})
                continue
            # No wheel published: built here from the locked source and shipped.
            sdist = item["sdist"]
            source = staging / sdist["url"].rsplit("/", 1)[1]
            with urllib.request.urlopen(sdist["url"]) as response, source.open("wb") as output:
                shutil.copyfileobj(response, output)
            if digest(source) != sdist["hash"].removeprefix("sha256:"):
                raise SystemExit(f"{name}: the source archive does not match the lock")
            built = staging / "built"
            subprocess.run(["uv", "build", "--wheel", "--out-dir", str(built), str(source)], check=True,
                           capture_output=True)
            (wheel,) = built.glob("*.whl")
            if wheel_rank(wheel.name, target) is None or wheel_rank(wheel.name, target)[2] != 0:
                raise SystemExit(f"{name}: builds a platform wheel; it cannot ship as a file")
            shutil.move(wheel, DESTINATION / "wheels" / wheel.name)
            files.append({"name": name, "file": wheel.name, "bundled": True,
                          "sha256": digest(DESTINATION / "wheels" / wheel.name),
                          "size": (DESTINATION / "wheels" / wheel.name).stat().st_size})
        manifest = {
            "python": PYTHON,
            "platform": [sys.platform, platform.machine()],
            "interpreter": {**interpreter(target, staging), "sources": INTERPRETER_SOURCES},
            "packages": {"official": PYPI,
                         "mirrors": ["https://pypi.tuna.tsinghua.edu.cn/packages/",
                                     "https://mirrors.aliyun.com/pypi/packages/"]},
            "modules": list(modules.values()),
        }
    (DESTINATION / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n")
    total = manifest["interpreter"]["size"] + sum(f["size"] for m in modules.values() for f in m["files"])
    for module in modules.values():
        print(f"{module['id']}: {len(module['files'])} files, "
              f"{sum(f['size'] for f in module['files']) / 2**20:.0f} MiB")
    print(f"interpreter {manifest['interpreter']['size'] / 2**20:.0f} MiB; total {total / 2**20:.0f} MiB")


if __name__ == "__main__":
    main()
