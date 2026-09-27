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
with tempfile.TemporaryDirectory(prefix="asterion-live-denied-", ignore_cleanup_errors=True) as folder:
    result = subprocess.run([sys.argv[3], "--mode", "live", "--session", "live.test",
                             "--endpoint", "asterion.test", "--directory", folder],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 3 and "Live execution unavailable" in result.stderr, result
    assert list(Path(folder).iterdir()) == []
print("CLI11 help, version, invalid arguments and fail-closed live mode verified")

# Every independent application requires an explicit operation/configuration.
for binary in sys.argv[5:]:
    with tempfile.TemporaryDirectory(prefix="asterion-cli-empty-", ignore_cleanup_errors=True) as folder:
        result = subprocess.run([str(Path(binary).resolve())], cwd=folder,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and result.stderr, (binary, result)
        assert not result.stdout and list(Path(folder).iterdir()) == [], binary
print("Application entry points reject missing configuration without creating state")
