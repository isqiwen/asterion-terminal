"""Lint owned Python sources with Ruff (matching CI)."""

from pathlib import Path
import shutil
import subprocess

VERSION = "0.17.5"
ROOT = Path(__file__).resolve().parents[1]


def runner() -> list[str]:
    ruff = shutil.which("ruff")
    if ruff:
        found = subprocess.check_output([ruff, "--version"], text=True).strip()
        if found == f"ruff {VERSION}":
            return [ruff]
    return ["uvx", f"ruff=={VERSION}"]


tracked = subprocess.check_output(
    ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT
).decode().split("\0")
files = sorted(
    {
        name
        for name in tracked
        if Path(name).suffix == ".py" and name != "conanfile.py" and (ROOT / name).is_file()
    }
)
print(f"Linting {len(files)} Python files with ruff {VERSION}", flush=True)
subprocess.run([*runner(), "check", "--config", "ruff.toml", *files], cwd=ROOT, check=True)
