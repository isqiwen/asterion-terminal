"""Trading startup rejects invalid inputs before opening its runtime."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
from startup import check_startup_options

binary = sys.argv[1]
check_startup_options(binary)
with tempfile.TemporaryDirectory(prefix="asterion-trading-", ignore_cleanup_errors=True) as folder:
    # An installation without the account's configuration: refused, naming it.
    (Path(folder) / "bin").mkdir()
    (Path(folder) / '.program.lock').touch()
    (Path(folder) / "bin/asterion_trading").symlink_to(Path(binary).resolve())
    result = subprocess.run([str(Path(folder) / "bin/asterion_trading"), "--account", "missing"],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode != 0 and "config/accounts/missing.json" in result.stderr, result
    assert sorted(p.name for p in Path(folder).iterdir()) == [".program.lock", "bin"], list(Path(folder).iterdir())
