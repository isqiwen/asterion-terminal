"""Explicit macOS user-service integration; leaves an existing user service untouched."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
if sys.platform != "darwin":
    raise SystemExit("Run this launchd integration on macOS only")
label = f"gui/{os.getuid()}/me.asterion.node-agent"
if subprocess.run(["/bin/launchctl", "print", label], capture_output=True).returncode == 0:
    raise SystemExit("An existing Asterion service is registered; refusing to modify it")
with tempfile.TemporaryDirectory(prefix="asterion-launchd-", ignore_cleanup_errors=True) as folder:
    env = dict(os.environ, HOME=folder)
    env.pop("ASTERION_NODE_DIRECTORY", None)
    host = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=env)
    try:
        host.stdin.write(json.dumps({"version": 1, "method": "node.local", "params": {}}) + "\n"); host.stdin.flush()
        response = json.loads(host.stdout.readline())
        assert response.get("result", {}).get("nodes", [{}])[0].get("state") == "online", response
        pidfile = Path(folder) / "Library/Application Support/Asterion/node/agent.pid"
        pid = int(pidfile.read_text())
        host.kill(); host.communicate(timeout=10)
        os.kill(pid, 0)
        os.kill(pid, signal.SIGKILL)
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            time.sleep(0.5)
            if int(pidfile.read_text()) != pid:
                os.kill(int(pidfile.read_text()), 0)
                break
        else:
            raise AssertionError("launchd did not restart Agent")
    finally:
        if host.poll() is None:
            host.kill(); host.communicate(timeout=10)
        subprocess.run(["/bin/launchctl", "bootout", label], capture_output=True)
        time.sleep(2)
print("Isolated launchd install, Terminal-independent lifetime and Agent crash restart verified")
