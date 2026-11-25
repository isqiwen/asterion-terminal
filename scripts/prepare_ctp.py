"""Explicit, hash-pinned CTP SDK provisioning; never called implicitly by Conan/CMake.

Supports staging official SFIT / broker SDK directories or archives via --local-dir,
recording official checksums into conan/ctp/sources.json via --record-origin, and
switching between production (prod) and compliance evaluation (eval) builds.
"""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "conan/ctp/sources.json"
TRANSIENT_HTTP = frozenset({429, 500, 502, 503, 504})

REQUIRED_FILES = {
    "linux": {
        "ThostFtdcMdApi.h": ("ThostFtdcMdApi.h",),
        "ThostFtdcUserApiDataType.h": ("ThostFtdcUserApiDataType.h",),
        "ThostFtdcUserApiStruct.h": ("ThostFtdcUserApiStruct.h",),
        "ThostFtdcTraderApi.h": ("ThostFtdcTraderApi.h",),
        "ctp-md.so": ("ctp-md.so", "thostmduserapi_se.so"),
        "ctp-trader.so": ("ctp-trader.so", "thosttraderapi_se.so"),
    },
    "macos": {
        "ThostFtdcMdApi.h": ("ThostFtdcMdApi.h",),
        "ThostFtdcUserApiDataType.h": ("ThostFtdcUserApiDataType.h",),
        "ThostFtdcUserApiStruct.h": ("ThostFtdcUserApiStruct.h",),
        "ThostFtdcTraderApi.h": ("ThostFtdcTraderApi.h",),
        "ctp-md.dylib": (
            "ctp-md.dylib",
            "thostmduserapi_se.dylib",
            "thostmduserapi_se.framework/Versions/A/thostmduserapi_se",
            "thostmduserapi_se",
        ),
        "ctp-trader.dylib": (
            "ctp-trader.dylib",
            "thosttraderapi_se.dylib",
            "thosttraderapi_se.framework/Versions/A/thosttraderapi_se",
            "thosttraderapi_se",
        ),
    },
}


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def download_bytes(url: str, *, attempts: int = 4, backoff: float = 2.0) -> bytes:
    """Fetch url bytes, retrying transient GitHub/CDN failures."""
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code not in TRANSIENT_HTTP or attempt + 1 == attempts:
                raise
        except (urllib.error.URLError, TimeoutError, ConnectionError, http.client.IncompleteRead):
            if attempt + 1 == attempts:
                raise
        time.sleep(backoff * (2**attempt))


def _safe_extract(archive: Path, destination: Path) -> Path:
    dest_root = destination.resolve()
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                target = (dest_root / info.filename).resolve()
                if target != dest_root and dest_root not in target.parents:
                    raise SystemExit(f"CTP archive member leaves target directory: {info.filename}")
                if info.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(zf.read(info))
        return dest_root
    if tarfile.is_tarfile(archive):
        with tarfile.open(archive) as tf:
            for member in tf.getmembers():
                if member.issym() or member.islnk():
                    continue
                target = (dest_root / member.name).resolve()
                if target != dest_root and dest_root not in target.parents:
                    raise SystemExit(f"CTP archive member leaves target directory: {member.name}")
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    extracted = tf.extractfile(member)
                    if extracted is not None:
                        target.write_bytes(extracted.read())
        return dest_root
    raise SystemExit(f"Unsupported CTP SDK archive format: {archive}")


def resolve_local_files(source: Path, target_os: str) -> dict[str, bytes]:
    if not source.exists():
        raise SystemExit(f"CTP SDK local path does not exist: {source}")
    if source.is_file():
        with tempfile.TemporaryDirectory(prefix="asterion-ctp-extract-") as tmp:
            return resolve_local_files(_safe_extract(source, Path(tmp)), target_os)
    if not source.is_dir():
        raise SystemExit(f"CTP SDK local path must be a directory or archive: {source}")

    all_files = sorted(p for p in source.rglob("*") if p.is_file() and not p.is_symlink())
    resolved: dict[str, bytes] = {}
    for target_name, candidates in REQUIRED_FILES[target_os].items():
        matches: list[Path] = []
        for candidate in candidates:
            if "/" in candidate:
                matches = [p for p in all_files if p.as_posix().endswith("/" + candidate) or p.as_posix() == candidate]
            else:
                matches = [p for p in all_files if p.name == candidate]
            if matches:
                break
        if not matches:
            raise SystemExit(
                f"Missing {target_name} in local CTP SDK {source} "
                f"(expected one of: {', '.join(candidates)})"
            )
        contents = [(p, p.read_bytes()) for p in matches]
        digests = {sha256_bytes(data) for _, data in contents}
        if len(digests) > 1:
            paths = ", ".join(str(p.relative_to(source)) for p, _ in contents)
            raise SystemExit(f"Ambiguous {target_name} in local CTP SDK {source}: {paths}")
        resolved[target_name] = contents[0][1]
    return resolved


def stage_sdk(
    *,
    target_os: str,
    target_arch: str,
    profile: str,
    manifest_path: Path = MANIFEST_PATH,
    stage_dir: Path | None = None,
    local_dir: Path | None = None,
    record_origin: str | None = None,
    require_official: bool = False,
    fetch=None,
) -> Path:
    if (target_os, target_arch) not in (("macos", "armv8"), ("linux", "x86_64")):
        raise SystemExit("CTP SDK supports macOS armv8 and Linux x86_64 only")
    if record_origin is not None:
        if not record_origin.strip():
            raise SystemExit("--record-origin requires a non-empty provenance description")
        if local_dir is None:
            raise SystemExit("--record-origin requires --local-dir")

    full_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if profile not in full_manifest or target_os not in full_manifest[profile]:
        raise SystemExit(f"Unknown CTP profile/OS in manifest: {profile}/{target_os}")
    entry = full_manifest[profile][target_os]
    expected_files: dict[str, dict] = entry.get("files", {})
    required_names = list(REQUIRED_FILES[target_os])

    stage = stage_dir or (ROOT / "build/ctp-sdk" / f"{target_os}-{target_arch}")
    staged_bytes: dict[str, bytes] = {}
    staged_hashes: dict[str, str] = {}

    if local_dir is not None:
        local_files = resolve_local_files(local_dir, target_os)
        if record_origin is not None:
            new_files = {}
            for name in required_names:
                data = local_files[name]
                digest = sha256_bytes(data)
                staged_bytes[name] = data
                staged_hashes[name] = digest
                new_files[name] = {"sha256": digest}
            entry["origin"] = record_origin.strip()
            entry["official"] = True
            entry["files"] = new_files
            manifest_path.write_text(
                json.dumps(full_manifest, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            official = True
            origin = entry["origin"]
        else:
            if not expected_files:
                raise SystemExit(
                    f"CTP profile '{profile}' ({target_os}) has no pinned checksums in {manifest_path}; "
                    f"re-run with --local-dir {local_dir} --record-origin \"<official source>\" "
                    "after verifying the official SFIT/broker package"
                )
            for name in required_names:
                if name not in expected_files or "sha256" not in expected_files[name]:
                    raise SystemExit(f"Manifest {manifest_path} lacks pinned sha256 for {profile}/{target_os}/{name}")
                data = local_files[name]
                digest = sha256_bytes(data)
                expected = expected_files[name]["sha256"]
                if digest != expected:
                    raise SystemExit(
                        f"CTP SDK checksum mismatch for {name}: expected {expected}, got {digest}. "
                        "If this is a newly verified official SFIT/broker SDK, pass --record-origin to update sources.json"
                    )
                staged_bytes[name] = data
                staged_hashes[name] = digest
            official = True
            origin = entry.get("origin") or f"local:{local_dir.name}"
        source_mode = "local"
    else:
        official = bool(entry.get("official", False))
        origin = str(entry.get("origin", ""))
        if require_official and not official:
            raise SystemExit(
                f"CTP profile '{profile}' ({target_os}) is pinned to non-official source '{origin}'. "
                "Import an official SFIT/broker SDK via --local-dir (and --record-origin) before production builds"
            )
        if not expected_files:
            raise SystemExit(
                f"CTP profile '{profile}' ({target_os}) requires an official SFIT/broker package via --local-dir"
            )
        if not official:
            print(
                f"WARNING: staging non-official CTP mirror ({origin}) for {profile}/{target_os}; "
                "use --local-dir with an official SFIT/broker package before live deployment",
                file=sys.stderr,
            )
        downloader = fetch or download_bytes
        for name in required_names:
            item = expected_files.get(name)
            if not item or "sha256" not in item:
                raise SystemExit(f"Manifest {manifest_path} lacks pinned entry for {profile}/{target_os}/{name}")
            target = stage / name
            if target.exists() and sha256_bytes(target.read_bytes()) == item["sha256"]:
                staged_hashes[name] = item["sha256"]
                continue
            url = item.get("url")
            if not url:
                raise SystemExit(
                    f"Official CTP profile '{profile}' ({target_os}) has no download URL for {name}; "
                    "pass --local-dir pointing to the verified official SFIT/broker SDK"
                )
            data = downloader(url)
            if sha256_bytes(data) != item["sha256"]:
                raise SystemExit("CTP SDK checksum mismatch: " + name)
            staged_bytes[name] = data
            staged_hashes[name] = item["sha256"]
        source_mode = "manifest"

    stage.mkdir(parents=True, exist_ok=True)
    for name, data in staged_bytes.items():
        (stage / name).write_bytes(data)
    provenance = {
        "profile": profile,
        "os": target_os,
        "arch": target_arch,
        "official": official,
        "origin": origin,
        "source_mode": source_mode,
        "files": staged_hashes,
    }
    (stage / "provenance.json").write_text(
        json.dumps(provenance, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return stage


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--os",
        choices=["macos", "linux"],
        default={"Darwin": "macos", "Linux": "linux"}.get(platform.system()),
    )
    parser.add_argument(
        "--arch",
        choices=["armv8", "x86_64"],
        default={"arm64": "armv8", "aarch64": "armv8", "x86_64": "x86_64", "amd64": "x86_64"}.get(
            platform.machine().lower()
        ),
    )
    parser.add_argument(
        "--profile",
        choices=["prod", "eval"],
        default=os.environ.get("ASTERION_CTP_PROFILE", "prod"),
        help="CTP SDK profile: prod (生产版) or eval (看穿式监管测评版)",
    )
    parser.add_argument(
        "--local-dir",
        type=Path,
        default=Path(os.environ["ASTERION_CTP_LOCAL_DIR"])
        if os.environ.get("ASTERION_CTP_LOCAL_DIR")
        else None,
        help="Official SFIT/broker CTP SDK directory or .tar.gz/.zip archive",
    )
    parser.add_argument(
        "--record-origin",
        default=None,
        help="Record SHA-256 of --local-dir as an official source in conan/ctp/sources.json",
    )
    parser.add_argument(
        "--require-official",
        action="store_true",
        default=os.environ.get("ASTERION_CTP_REQUIRE_OFFICIAL") == "1",
        help="Refuse non-official mirror sources",
    )
    args = parser.parse_args(argv)
    if args.os is None or args.arch is None:
        parser.error("Specify a supported --os and --arch; host target could not be inferred")
    if (args.os, args.arch) not in (("macos", "armv8"), ("linux", "x86_64")):
        parser.error("CTP SDK supports macOS armv8 and Linux x86_64 only")

    stage = stage_sdk(
        target_os=args.os,
        target_arch=args.arch,
        profile=args.profile,
        local_dir=args.local_dir,
        record_origin=args.record_origin,
        require_official=args.require_official,
    )
    subprocess.run(
        [
            "conan",
            "export-pkg",
            str(ROOT / "conan/ctp"),
            "--output-folder",
            str(stage / "conan"),
            "-s",
            "os=" + {"macos": "Macos", "linux": "Linux"}[args.os],
            "-s",
            "arch=" + args.arch,
            "-c",
            "user.ctp:sdk_root=" + str(stage),
        ],
        check=True,
    )
    # Local recipes that patch an upstream dependency; conan.lock pins their revisions.
    subprocess.run(["conan", "export", str(ROOT / "conan/duckdb"), "--version", "1.4.3"], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
