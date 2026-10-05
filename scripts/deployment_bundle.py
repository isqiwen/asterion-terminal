"""Package native binaries and host preparation tools for Terminal deployment."""
import hashlib
import json
from pathlib import Path
import platform
import sys
import zipfile
import subprocess
from service_fingerprint import fingerprint

if sys.platform != "linux":
    raise SystemExit("Remote service bundles target Linux only; desktop bundles include local native services separately")
build = Path(sys.argv[1]).resolve()
subprocess.run(["cmake", "--build", str(build), "--target", "asterion-node-agent", "asterion-trading",
    "asterion-market-data", "asterion-data-service", "asterion-task-service", "asterion-backtest", "asterion-factor",
    "asterion-data-pipeline"], check=True)
source_hash = fingerprint()
if (build / "service-source.sha256").read_text().strip() != source_hash:
    raise SystemExit("Build directory service source does not match this checkout")
os_name = "linux"
if platform.machine().lower() not in ('x86_64', 'amd64'):
    raise SystemExit('Linux currently supports x86_64 only')
arch = "x86_64"
files = [build / name for name in ("asterion-node-agent", "asterion-trading", "asterion-market-data", "asterion-data-service", "asterion-task-service", "asterion-backtest", "asterion-factor", "asterion-data-pipeline")]
if arch == "x86_64": files.extend([build / "ctp-md.so", build / "ctp-trader.so"])
files.append(build / "plugins/asterion-tushare.so")
files.append(build / "plugins/asterion-order-limits.so")
product_version = json.loads((Path(__file__).resolve().parents[1] / "apps/clients/terminal/electron/package.json").read_text())["version"]
manifest = {"version": 2, "source_sha256": source_hash, "product_version": product_version, "os": os_name, "arch": arch, "files": {p.relative_to(build).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
initializer = Path(__file__).parent / "node/initialize-linux.py"
manifest["files"]["initialize-linux.py"] = hashlib.sha256(initializer.read_bytes()).hexdigest()
output = build / "deployment"
output.mkdir(exist_ok=True)
archive = output / f"asterion-services-{os_name}-{arch}.zip"
with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
    for path in files:
        bundle.write(path, path.relative_to(build).as_posix())
    bundle.write(initializer, "initialize-linux.py")
    bundle.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
print(archive)
