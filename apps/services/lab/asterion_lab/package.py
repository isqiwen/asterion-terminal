"""A strategy of the strategy directory as a code-only package for Trading.

A package holds the strategy's module and the modules of the directory it
imports (repository.closure), the same files a run copies, so the strategy
imports there what it imported in research. Building one never imports
strategy code. Trading is the authority for package admission and immutable
publication.
"""
import time
from pathlib import Path

from asterion.strategy_pb2 import Manifest, Package
from asterion_lab.repository import checked, closure

RUNTIME = "python3.12-numpy2.3.5"


def build(request):
    root = checked(Path(request.repository)).resolve()
    module, _, name = request.strategy.partition(".")
    entry = f"strategies/{module}.py"
    if not name or not (root / entry).is_file():
        raise ValueError(f"no strategy {request.strategy} in {root}")
    files = closure(root, entry)
    # Each publication is a new version; Trading refuses to overwrite one.
    manifest = Manifest(name=request.strategy, version=time.strftime("%Y%m%d-%H%M%S"),
                        entry=f"strategies.{module}:{name}", runtime=RUNTIME, files=files)
    result = Package(manifest=manifest)
    for file in files:
        result.files.add(path=file, content=(root / file).read_bytes())
    return result
