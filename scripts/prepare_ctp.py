"""Explicit, hash-pinned SDK provisioning; never called implicitly by Conan/CMake."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--os", choices=["macos", "linux"], default={"Darwin":"macos", "Linux":"linux"}.get(platform.system()))
p.add_argument("--arch", choices=["armv8", "x86_64"], default={"arm64":"armv8", "aarch64":"armv8", "x86_64":"x86_64", "amd64":"x86_64"}.get(platform.machine().lower()))
args = p.parse_args()
if args.os is None or args.arch is None:
    p.error('Specify a supported --os and --arch; host target could not be inferred')
if args.os == 'linux' and args.arch != 'x86_64':
    p.error(f'{args.os} CTP SDK currently supports x86_64 only')
manifest = json.loads((ROOT / "conan/ctp/sources.json").read_text())[args.os]
stage = ROOT / "build/ctp-sdk" / (args.os + "-" + args.arch)
stage.mkdir(parents=True, exist_ok=True)
for name, item in manifest.items():
    target = stage / name
    if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest() != item["sha256"]:
        data = urllib.request.urlopen(item["url"], timeout=60).read()
        if hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise SystemExit("CTP SDK checksum mismatch: " + name)
        target.write_bytes(data)
subprocess.run(["conan", "export-pkg", str(ROOT / "conan/ctp"), "--output-folder", str(stage / "conan"), "-s", "os=" + {"macos":"Macos","linux":"Linux"}[args.os], "-s", "arch=" + args.arch, "-c", "user.ctp:sdk_root=" + str(stage)], check=True)
# Local recipes that patch an upstream dependency; conan.lock pins their revisions.
subprocess.run(["conan", "export", str(ROOT / "conan/duckdb"), "--version", "1.4.3"], check=True)
