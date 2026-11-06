"""Market startup rejects invalid inputs before opening its runtime."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
from cli import check_cli

binary = sys.argv[1]
check_cli(binary)
with tempfile.TemporaryDirectory(prefix="asterion-market-") as folder:
    root = Path(folder)
    (root / "bin").mkdir()
    (root / "bin/asterion_market").symlink_to(Path(binary).resolve())
    result = subprocess.run([str(root / "bin/asterion_market")],
                            cwd="/", capture_output=True, text=True, timeout=10)
    assert result.returncode != 0 and str(root / "config/market.json") in result.stderr, result
