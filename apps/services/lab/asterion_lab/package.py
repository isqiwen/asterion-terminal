"""A strategy of the strategy directory as a code-only package for Trading.

A package holds every Python file of the directory, as a run copies them, so
the strategy imports there what it imported in research. Building one never
imports strategy code. Trading is the authority for package admission and
immutable publication.
"""
import time
from pathlib import Path

from asterion.strategy_pb2 import Manifest, Package
from asterion_lab.repository import checked, python_files

RUNTIME = "python3.12-numpy2.3.5"


def build(request):
    root = checked(Path(request.repository)).resolve()
    module, _, name = request.strategy.partition(".")
    files = python_files(root)
    if not name or f"strategies/{module}.py" not in files:
        raise ValueError(f"no strategy {request.strategy} in {root}")
    # Each publication is a new version; Trading refuses to overwrite one.
    manifest = Manifest(name=request.strategy, version=time.strftime("%Y%m%d-%H%M%S"),
                        entry=f"strategies.{module}:{name}", runtime=RUNTIME, files=files)
    result = Package(manifest=manifest)
    for file in files:
        result.files.add(path=file, content=(root / file).read_bytes())
    return result
