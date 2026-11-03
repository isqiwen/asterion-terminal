"""Browser integration against an isolated root and the test CTP SDKs."""
import os
import shutil
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
TESTS = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(TESTS / "terminal")]
from market_installation import market_host
from market_tls import fingerprint
# timeout and kill send SIGTERM; exiting through the with blocks stops the Market.
signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
BUILD = Path(os.environ.get("ASTERION_CPP_BUILD", str(TESTS.parent / "build/Debug")))
with tempfile.TemporaryDirectory(prefix="asterion-e2e-", ignore_cleanup_errors=True) as folder:
    root = Path(folder)
    env = dict(os.environ, ASTERION_TABLE_FIXTURE_CONTROL=str(root / "table-source-control"))
    # Editors such as VS Code export this; Electron would then start as plain Node.
    env.pop("ELECTRON_RUN_AS_NODE", None)
    # The fixture strategies in a work tree of their own, never the project's research/.
    research = root / "research"
    shutil.copytree(TESTS / "lab/strategies", research)
    for arguments in (["init", "-q"], ["add", "."], ["commit", "-qm", "fixture"]):
        subprocess.run(["git", "-C", str(research), "-c", "user.name=test",
                        "-c", "user.email=test@invalid", *arguments], check=True)
    env["ASTERION_RESEARCH_DIRECTORY"] = str(research)
    with market_host(BUILD, root / "market", exchanges=("SHFE", "GFEX")) as port:
        env["ASTERION_TEST_MARKET_PORT"] = str(port)
        env["ASTERION_TEST_MARKET_TLS"] = str(root / "market/config/tls")
        env["ASTERION_TEST_MARKET_FINGERPRINT"] = fingerprint(root / "market/config/tls")
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("isolated_node.py")), *sys.argv[1:]], env=env)
    sys.exit(result.returncode)
