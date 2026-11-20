"""Terminal startup rejects invalid inputs before opening its runtime."""
from pathlib import Path
import subprocess
import sys
import json
import os

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
from startup import check_startup_options

binary = sys.argv[1]
check_startup_options(binary)
result = subprocess.run([binary], env={**os.environ, "ASTERION_ROOT": "relative"},
                        capture_output=True, text=True, timeout=10)
assert result.returncode != 0, result
assert json.loads(result.stderr) == {"error": {"code": "invalid_request",
                                             "message": "invalid Asterion root directory"}}, result
