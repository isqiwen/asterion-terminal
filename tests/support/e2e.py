"""Browser integration against an isolated root and the test CTP SDKs."""
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
TESTS = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(TESTS / "terminal")]
from history_installation import history_host, quotes_host
from history_tls import fingerprint
# timeout and kill send SIGTERM; exiting through the with blocks stops the Market.
signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
BUILD = Path(os.environ.get("ASTERION_CPP_BUILD", str(TESTS.parent / "build/Debug")))
with tempfile.TemporaryDirectory(prefix="asterion-e2e-", ignore_cleanup_errors=True) as folder:
    root = Path(folder)
    env = dict(os.environ, ASTERION_TABLE_FIXTURE_CONTROL=str(root / "table-source-control"))
    # Editors such as VS Code export this; Electron would then start as plain Node.
    env.pop("ELECTRON_RUN_AS_NODE", None)
    # The fixture strategies are the built-in ones, never the project's research/.
    env["ASTERION_BUILTIN_RESEARCH"] = str(TESTS / "lab/strategies")
    with history_host(BUILD, root / "history", exchanges=("SHFE", "GFEX")) as history, quotes_host(BUILD, root / "quotes", also_trust=root / "history/config/tls") as quotes:
        env["ASTERION_TEST_HISTORY_PORT"] = str(history)
        env["ASTERION_TEST_HISTORY_TLS"] = str(root / "history/config/tls")
        env["ASTERION_TEST_HISTORY_FINGERPRINT"] = fingerprint(root / "history/config/tls")
        env["ASTERION_TEST_QUOTES_PORT"] = str(quotes)
        env["ASTERION_TEST_QUOTES_TLS"] = str(root / "quotes/config/tls")
        env["ASTERION_TEST_QUOTES_FINGERPRINT"] = fingerprint(root / "quotes/config/tls")
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("isolated_node.py")), *sys.argv[1:]], env=env)
    sys.exit(result.returncode)
