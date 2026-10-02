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
        # The test may have reset into nested node directories, each with its
        # own Agent; stop them all, then let their services exit before cleanup.
        stopped = False
        for pidfile in Path(folder).rglob("agent.pid"):
            try:
                os.kill(int(pidfile.read_text()), signal.SIGTERM)
                stopped = True
            except (OSError, ValueError):
                # Already exited: ProcessLookupError on POSIX, WinError 87 on Windows.
                pass
        if stopped:
            time.sleep(3)
    sys.exit(result.returncode)
