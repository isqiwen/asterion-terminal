"""Run a test with a disposable local Agent, then terminate only its owned PID."""
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time
with tempfile.TemporaryDirectory(prefix="ast-node-test-") as folder:
    env = dict(os.environ, ASTERION_NODE_DIRECTORY=folder)
    try:
        result = subprocess.run([shutil.which(sys.argv[1]) or sys.argv[1], *sys.argv[2:]], env=env)
    finally:
        pidfile = Path(folder) / "agent.pid"
        if pidfile.exists():
            try:
                os.kill(int(pidfile.read_text()), signal.SIGTERM)
            except ProcessLookupError:
                pass
            time.sleep(2)
    sys.exit(result.returncode)
