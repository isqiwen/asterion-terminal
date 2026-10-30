"""Browser integration against an isolated root, the test keychain and the test CTP SDKs."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
TESTS = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(TESTS / "terminal")]
from keychain_fixture import install as install_keychain
BUILD = Path(os.environ.get("ASTERION_CPP_BUILD", str(TESTS.parent / "build/Debug")))
with tempfile.TemporaryDirectory(prefix="asterion-e2e-", ignore_cleanup_errors=True) as folder:
    root = Path(folder)
    env = dict(os.environ, ASTERION_TABLE_FIXTURE_CONTROL=str(root / "table-source-control"),
               ASTERION_KEYCHAIN_EXECUTABLE=install_keychain(root / "keychain"))
    # Editors such as VS Code export this; Electron would then start as plain Node.
    env.pop("ELECTRON_RUN_AS_NODE", None)
    sdk = "libasterion_test_ctp.dylib" if sys.platform == "darwin" else "libasterion_test_ctp.so"
    env["ASTERION_CTP_LIBRARY"] = str(BUILD / sdk)
    env["ASTERION_CTP_CATALOG_LIBRARY"] = str(BUILD / sdk.replace("asterion_test_ctp", "asterion_test_ctp_trader"))
    result = subprocess.run([sys.executable, str(Path(__file__).with_name("isolated_node.py")), *sys.argv[1:]], env=env)
    sys.exit(result.returncode)
