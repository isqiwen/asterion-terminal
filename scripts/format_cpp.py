"""Format owned C/C++ and Protobuf sources, including renames and new files."""

import argparse
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true")
options = parser.parse_args()
version = subprocess.check_output(["clang-format", "--version"], text=True).strip()
if not re.search(r"\bversion 23\.1\.1\b", version):
    parser.error(f"clang-format 23.1.1 is required (same as CI); found: {version}")

tracked = subprocess.check_output(
    ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=root
).decode().split("\0")
files = sorted(
    {
        name
        for name in tracked
        if Path(name).suffix in {".c", ".cpp", ".hpp", ".h", ".proto"}
        and (root / name).is_file()
    }
)
print(f"{'Checking' if options.check else 'Formatting'} {len(files)} C/C++ and Protobuf files", flush=True)
for offset in range(0, len(files), 100):
    subprocess.run(
        [
            "clang-format",
            "--style=file",
            *(["--dry-run", "-Werror"] if options.check else ["-i"]),
            *files[offset : offset + 100],
        ],
        cwd=root,
        check=True,
    )
