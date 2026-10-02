"""Run a test with a disposable local Agent, then terminate only its owned PID."""
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time
with tempfile.TemporaryDirectory(prefix="ast-node-test-", ignore_cleanup_errors=True) as folder:
    # The Agent exits when this process disappears, even if CTest kills it on
    # timeout before the cleanup below runs.
    env = dict(os.environ, ASTERION_NODE_DIRECTORY=folder, ASTERION_TEST_NODE_ISOLATED="1",
               ASTERION_TEST_OWNER_PID=str(os.getpid()))
    try:
        result = subprocess.run([shutil.which(sys.argv[1]) or sys.argv[1], *sys.argv[2:]], env=env)
    finally:
        pidfile = Path(folder) / "agent.pid"
        if pidfile.exists():
            try:
                os.kill(int(pidfile.read_text()), signal.SIGTERM)
            except OSError:
                # Already exited: ProcessLookupError on POSIX, WinError 87 on Windows.
                pass
            time.sleep(2)
    sys.exit(result.returncode)
