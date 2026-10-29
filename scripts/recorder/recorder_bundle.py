"""Package asterion_recorder and the CTP libraries for a recording host.

    python3 scripts/recorder/recorder_bundle.py <build directory>

Build it where the services are built for Linux hosts (the Ubuntu 24.04
container of scripts/services/build_linux_services.sh), so the program needs
no newer system libraries than the host has.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1]).resolve()
subprocess.run(["cmake", "--build", str(build), "--target", "asterion_recorder"], check=True)
# Laid out like the installation on the host.
files = {
    "bin/asterion_recorder": build / "asterion_recorder",
    "bin/recording-host.py": ROOT / "scripts/recorder/recording-host.py",
    "bin/configure.sh": ROOT / "scripts/recorder/configure.sh",
    "lib/ctp-md.so": build / "ctp-md.so",
    "lib/ctp-trader.so": build / "ctp-trader.so",
    "config/recorder.json": ROOT / "config/recorder.json",
}
version = json.loads((ROOT / "apps/clients/terminal/electron/package.json").read_text())["version"]
manifest = {"product_version": version,
            "files": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in files.items()}}
output = build / "recorder" / "asterion-recorder-linux-x86_64.zip"
output.parent.mkdir(exist_ok=True)
with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
    for name, path in files.items():
        bundle.write(path, name)
    bundle.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
print(output)
