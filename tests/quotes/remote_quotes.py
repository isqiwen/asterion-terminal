"""The native read-only client attaches to an independently installed History."""
from pathlib import Path
import subprocess
import sys
build = Path(sys.argv[1]).resolve()
suffix = ".dylib" if sys.platform == "darwin" else ".so"
subprocess.run([sys.executable, str(Path(__file__).parents[1] / "quotes/live_process.py"),
    str(build / "asterion_quotes"), str(build / ("libasterion_test_ctp" + suffix)),
    str(build / ("libasterion_test_ctp_trader" + suffix)), str(build / "asterion_test_quotes_remote")], check=True)
