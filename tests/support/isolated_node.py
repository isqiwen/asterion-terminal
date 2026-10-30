"""Run a test against a disposable ASTERION_ROOT.

Services started by a Terminal under test exit with it (--owner-pid), so the
directory is removed once the test and its Terminals have gone.
"""
import os
import shutil
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix="ast-node-test-", ignore_cleanup_errors=True) as folder:
    env = dict(os.environ, ASTERION_ROOT=folder, ASTERION_TEST_NODE_ISOLATED="1")
    result = subprocess.run([shutil.which(sys.argv[1]) or sys.argv[1], *sys.argv[2:]], env=env)
    sys.exit(result.returncode)
