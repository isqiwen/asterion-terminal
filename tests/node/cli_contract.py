"""CLI11 must reject malformed input before opening runtimes or account stores."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

for binary in sys.argv[1:]:
    for option in ("--help", "--version"):
        result = subprocess.run([binary, option], capture_output=True, text=True, timeout=10)
        assert result.returncode == 0 and result.stdout, (binary, option, result)
    result = subprocess.run([binary, "--not-an-option"], capture_output=True, text=True, timeout=10)
    assert result.returncode != 0 and result.stderr, binary
with tempfile.TemporaryDirectory(prefix="asterion-trading-", ignore_cleanup_errors=True) as folder:
    # An installation without the account's configuration: refused, naming it.
    (Path(folder) / "bin").mkdir()
    (Path(folder) / '.program.lock').touch()
    (Path(folder) / "bin/asterion_trading").symlink_to(Path(sys.argv[2]).resolve())
    result = subprocess.run([str(Path(folder) / "bin/asterion_trading"), "--account", "missing"],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode != 0 and "config/accounts/missing.json" in result.stderr, result
    assert sorted(p.name for p in Path(folder).iterdir()) == [".program.lock", "bin"], list(Path(folder).iterdir())
with tempfile.TemporaryDirectory(prefix="asterion-market-") as folder:
    root = Path(folder)
    (root / "bin").mkdir()
    (root / "bin/asterion_market").symlink_to(Path(sys.argv[3]).resolve())
    result = subprocess.run([str(root / "bin/asterion_market")],
                            cwd="/", capture_output=True, text=True, timeout=10)
    assert result.returncode != 0 and str(root / "config/market.json") in result.stderr, result
result = subprocess.run([sys.argv[1]], env={**os.environ, "ASTERION_ROOT": "relative"},
                        capture_output=True, text=True, timeout=10)
assert result.returncode != 0, result
assert json.loads(result.stderr) == {"error": {"code": "invalid_request",
                                             "message": "invalid Asterion root directory"}}, result

print("CLI11 help, version, invalid arguments and trading without its account configuration verified")
