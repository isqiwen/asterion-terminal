"""Trusted child bootstrap, entered before runtime configuration is loaded."""

import runpy
import sys
from pathlib import Path


def run(target: Path):
    import resource

    resource.setrlimit(resource.RLIMIT_FSIZE, (8_000_000, 8_000_000))
    resource.setrlimit(resource.RLIMIT_CPU, (30, 30))
    sys.dont_write_bytecode = True
    target = target.resolve()
    sys.path.insert(0, str(target))
    runpy.run_path(str(target / "plugin.py"), run_name="__main__")
