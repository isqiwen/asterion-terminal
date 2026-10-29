"""Package the recorder for a recording host as one archive laid out like its
installation: asterion-recorder/{bin,lib,config}.

    python3 apps/services/recorder/scripts/recorder_bundle.py <build directory>

Build it where the services are built for Linux hosts (Ubuntu 24.04), so the
program needs no newer system libraries than the host has.
"""
from pathlib import Path
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[4]
build = Path(sys.argv[1]).resolve()
subprocess.run(["cmake", "--build", str(build), "--target", "asterion_recorder"], check=True)
files = {
    "bin/asterion_recorder": build / "asterion_recorder",
    "bin/setup.sh": ROOT / "apps/services/recorder/scripts/setup.sh",
    "lib/ctp-md.so": build / "ctp-md.so",
    "lib/ctp-trader.so": build / "ctp-trader.so",
    "config/recorder.json": ROOT / "config/recorder.json",
}
output = build / "recorder" / "asterion-recorder-linux-x86_64.tar.gz"
output.parent.mkdir(exist_ok=True)
with tarfile.open(output, "w:gz") as bundle:
    for name, path in files.items():
        entry = bundle.gettarinfo(path, f"asterion-recorder/{name}")
        entry.mode = 0o644 if name.startswith("config/") else 0o755
        entry.uid = entry.gid = 0
        entry.uname = entry.gname = "root"
        with open(path, "rb") as data:
            bundle.addfile(entry, data)
print(output)
