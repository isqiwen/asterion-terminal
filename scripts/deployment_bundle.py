"""Package native binaries and host preparation tools for Terminal deployment."""
import hashlib
import json
from pathlib import Path
import platform
import sys
import zipfile

if sys.platform != "linux":
    raise SystemExit("Remote service bundles target Linux only; desktop bundles include local native services separately")
build = Path(sys.argv[1]).resolve()
os_name = "linux"
if platform.machine().lower() not in ('x86_64', 'amd64'):
    raise SystemExit('Linux currently supports x86_64 only')
arch = "x86_64"
files = [build / name for name in ("asterion-node-agent", "asterion-trading", "asterion-market-data", "asterion-task-service", "asterion-backtest", "asterion-factor", "asterion-data-pipeline", "asterion-strategy")]
if arch == "x86_64": files.append(build / "ctp-md.so")
product_version = json.loads((Path(__file__).resolve().parents[1] / "apps/terminal/src-tauri/tauri.conf.json").read_text())["version"]
manifest = {"version": 1, "product_version": product_version, "os": os_name, "arch": arch, "files": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
initializer = Path(__file__).parent / "node/initialize-linux.py"
manifest["files"]["initialize-linux.py"] = hashlib.sha256(initializer.read_bytes()).hexdigest()
output = build / "deployment"
output.mkdir(exist_ok=True)
archive = output / f"asterion-services-{os_name}-{arch}.zip"
with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
    for path in files:
        bundle.write(path, path.name)
    bundle.write(initializer, "initialize-linux.py")
    bundle.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
print(archive)
