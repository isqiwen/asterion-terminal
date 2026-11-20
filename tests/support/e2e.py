"""Browser integration against an isolated root and the test CTP SDKs."""
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
TESTS = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(TESTS / "terminal")]
from warehouse_installation import warehouse_host, market_host
from warehouse_tls import fingerprint
# timeout and kill send SIGTERM; exiting through the with blocks stops the services.
signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
BUILD = Path(os.environ.get("ASTERION_CPP_BUILD", str(TESTS.parent / "build/Debug")))
with tempfile.TemporaryDirectory(prefix="asterion-e2e-", ignore_cleanup_errors=True) as folder:
    root = Path(folder)
    env = dict(os.environ, ASTERION_TABLE_FIXTURE_CONTROL=str(root / "table-source-control"))
    # Editors such as VS Code export this; Electron would then start as plain Node.
    env.pop("ELECTRON_RUN_AS_NODE", None)
    env["ASTERION_BUILTIN_RESEARCH"] = str(TESTS.parent / "research")
    with warehouse_host(BUILD, root / "warehouse", exchanges=("SHFE", "GFEX")) as warehouse, market_host(BUILD, root / "market", also_trust=root / "warehouse/config/tls") as market:
        env["ASTERION_TEST_WAREHOUSE_PORT"] = str(warehouse)
        env["ASTERION_TEST_WAREHOUSE_TLS"] = str(root / "warehouse/config/tls")
        env["ASTERION_TEST_WAREHOUSE_FINGERPRINT"] = fingerprint(root / "warehouse/config/tls")
        env["ASTERION_TEST_MARKET_PORT"] = str(market)
        env["ASTERION_TEST_MARKET_TLS"] = str(root / "market/config/tls")
        env["ASTERION_TEST_MARKET_FINGERPRINT"] = fingerprint(root / "market/config/tls")
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("isolated_node.py")), *sys.argv[1:]], env=env)
    sys.exit(result.returncode)
