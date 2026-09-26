"""Trusted child bootstrap, entered before runtime configuration is loaded."""

import runpy
import sys
from pathlib import Path


def run(target: Path):
    from asterion_bindings.transport import child_limits

    child_limits()
    sys.dont_write_bytecode = True
    target = target.resolve()
    sys.path.insert(0, str(target))
    runpy.run_path(str(target / "plugin.py"), run_name="__main__")
