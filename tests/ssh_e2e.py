"""Isolated SSH orchestration tools for browser integration; no remote OS mutation."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import signal
import time
from bundle_fixture import make_bundle
BUILD = Path(os.environ.get("ASTERION_CPP_BUILD", str(Path(__file__).resolve().parents[1] / "build/Debug")))
with tempfile.TemporaryDirectory(prefix="asterion-ssh-ui-", ignore_cleanup_errors=True) as folder:
    root = Path(folder); tools = root / "tools"; tools.mkdir(); remote = root / "remote"; remote.mkdir()
    resources=make_bundle(root / "resources", BUILD)
    env = dict(os.environ, ASTERION_REMOTE_RESOURCES=str(resources))
    # Editors such as VS Code export this; Electron would then start as plain Node.
    env.pop("ELECTRON_RUN_AS_NODE", None)
    sdk = "asterion_test_ctp.dll" if sys.platform == "win32" else "libasterion_test_ctp.dylib" if sys.platform == "darwin" else "libasterion_test_ctp.so"
    env["ASTERION_CTP_LIBRARY"] = str(BUILD / sdk)
    env["ASTERION_CTP_CATALOG_LIBRARY"] = str(BUILD / sdk.replace("asterion_test_ctp", "asterion_test_ctp_trader"))
    if os.name != "nt":
        for name in ("ssh", "sftp"):
            shutil.copyfile(Path(__file__).with_name("ssh_fixture.py"), tools / name)
            (tools / name).chmod(0o700)
        (tools / "ssh-keygen").symlink_to("/usr/bin/ssh-keygen")
        env.update(ASTERION_SSH_TOOL_DIRECTORY=str(tools), ASTERION_SSH_FIXTURE=str(remote))
    try:
        result = subprocess.run([sys.executable, str(Path(__file__).with_name("isolated_node.py")), *sys.argv[1:]], env=env)
    finally:
        if (remote / "pid").exists():
            try: os.kill(int((remote / "pid").read_text()), signal.SIGTERM)
            except ProcessLookupError: pass
            time.sleep(2)
    sys.exit(result.returncode)
