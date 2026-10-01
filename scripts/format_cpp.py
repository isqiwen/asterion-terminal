"""Format current C++ sources, including unstaged renames and new files."""
import argparse
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true")
options = parser.parse_args()
tracked = subprocess.check_output(
    ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=root
).decode().split("\0")
files = sorted({
    name for name in tracked
    if Path(name).suffix in {".cpp", ".hpp", ".h"} and (root / name).is_file()
})
for offset in range(0, len(files), 100):
    subprocess.run(
        ["clang-format", *(["--dry-run", "-Werror"] if options.check else ["-i"]),
         *files[offset:offset + 100]],
        cwd=root, check=True,
    )
