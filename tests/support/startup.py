"""Common executable help, version and invalid-option checks."""
from pathlib import Path
import json
import subprocess


def check_startup_options(binary):
    product = json.loads((Path(binary).parent / "product.json").read_text())
    for option in ("--help", "--version"):
        result = subprocess.run([binary, option], capture_output=True, text=True, timeout=10)
        assert result.returncode == 0 and result.stdout, (binary, option, result)
        if option == "--version":
            assert f"{product['version']} ({product['commit']})" in result.stdout, result.stdout
    result = subprocess.run([binary, "--not-an-option"], capture_output=True, text=True, timeout=10)
    assert result.returncode != 0 and result.stderr, binary
