"""CLI11 must reject malformed input before opening runtimes or account stores."""
from pathlib import Path
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
    (Path(folder) / "bin/asterion_trading").symlink_to(Path(sys.argv[2]).resolve())
    result = subprocess.run([str(Path(folder) / "bin/asterion_trading"), "--account", "missing"],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode != 0 and "config/accounts/missing.json" in result.stderr, result
    assert sorted(p.name for p in Path(folder).iterdir()) == ["bin"], list(Path(folder).iterdir())
print("CLI11 help, version, invalid arguments and trading without its account configuration verified")

# Every independent application requires an explicit operation/configuration.
for binary in sys.argv[3:]:
    with tempfile.TemporaryDirectory(prefix="asterion-cli-empty-", ignore_cleanup_errors=True) as folder:
        result = subprocess.run([str(Path(binary).resolve())], cwd=folder,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and result.stderr, (binary, result)
        assert not result.stdout and list(Path(folder).iterdir()) == [], binary
print("Application entry points reject missing configuration without creating state")
