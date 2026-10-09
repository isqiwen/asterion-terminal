"""A worker's control loop observes parent loss while its claim has no reply."""
from pathlib import Path
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="ast-worker-owner-", dir="/tmp") as folder:
    endpoint = str(Path(folder) / "task.sock")
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(endpoint)
        listener.listen(1)
        listener.settimeout(5)
        parent_code = """
import os, subprocess, sys
child = subprocess.Popen([sys.argv[1], '--endpoint', sys.argv[2], '--session', 'tasks',
                          '--task', 'waiting', '--owner-pid', str(os.getpid())],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(child.pid, flush=True)
sys.stdin.buffer.read(1)
os._exit(0)
"""
        parent = subprocess.Popen([sys.executable, "-c", parent_code, sys.argv[1], endpoint],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        child_pid = int(parent.stdout.readline())
        stopped = False
        try:
            with listener.accept()[0] as channel:
                channel.settimeout(2)
                assert channel.recv(4096), "worker did not submit its claim"
                # Never acknowledge the claim. Its input timeout is much longer
                # than this observation bound, so a blocking client cannot pass.
                parent.stdin.close()
                parent.wait(timeout=2)
                started = time.monotonic()
                while channel.recv(4096):
                    assert time.monotonic() - started < 2
                assert time.monotonic() - started < 2
                stopped = True
        finally:
            if parent.poll() is None:
                parent.kill()
                parent.wait()
            if not stopped:
                try:
                    os.kill(child_pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
print("Worker stopped on parent loss while awaiting an unacknowledged claim")
