# Replaced with build metadata when packaging the runtime.
import hashlib
from pathlib import Path
import subprocess

_root = Path(__file__).resolve().parents[4]
VERSION = (_root / "VERSION").read_text().strip()
COMMIT = subprocess.check_output(["git", "-C", str(_root), "rev-parse", "--short=12", "HEAD"], text=True).strip()
_proto = _root / "protocol/proto/asterion"
WAREHOUSE_PROTOCOL = hashlib.sha256(
    (_proto / "common.proto").read_bytes() + (_proto / "warehouse.proto").read_bytes()
).hexdigest()[:12]
