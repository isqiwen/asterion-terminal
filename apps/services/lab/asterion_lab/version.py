# Replaced with build metadata when packaging the runtime.
from pathlib import Path
import subprocess

_root = Path(__file__).resolve().parents[4]
VERSION = (_root / "VERSION").read_text().strip()
COMMIT = subprocess.check_output(["git", "-C", str(_root), "rev-parse", "--short=12", "HEAD"], text=True).strip()
