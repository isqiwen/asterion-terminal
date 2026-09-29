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
p.add_argument("--os", choices=["macos", "linux", "windows"], default={"Darwin":"macos", "Linux":"linux", "Windows":"windows"}[platform.system()])
p.add_argument("--arch", choices=["armv8", "x86_64"], default="armv8" if platform.machine().lower() in ("arm64", "aarch64") else "x86_64")
args = p.parse_args()
if args.os in ('linux', 'windows') and args.arch != 'x86_64':
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
subprocess.run(["conan", "export-pkg", str(ROOT / "conan/ctp"), "--output-folder", str(stage / "conan"), "-s", "os=" + {"macos":"Macos","linux":"Linux","windows":"Windows"}[args.os], "-s", "arch=" + args.arch, "-c", "user.ctp:sdk_root=" + str(stage)], check=True)
