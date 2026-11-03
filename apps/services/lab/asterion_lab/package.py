"""Read an explicit code-only package from the user's strategy repository.

Building a package never imports strategy code or collects the whole repository.
Given a research run, the listed files come from the copy of the code that run
imported, so the package is exactly what was tested; the manifest still comes
from the repository. Trading is the authority for package admission and
immutable publication.
"""
from pathlib import Path, PurePosixPath

from google.protobuf.json_format import Parse
from asterion.strategy_pb2 import Manifest, Package
from asterion_lab.repository import checked


def build(request, runs):
    root = checked(Path(request.repository)).resolve()
    source = root
    if request.run:
        if not any(row["id"] == request.run and row["state"] == "succeeded" for row in runs.list()):
            raise ValueError(f"no succeeded research run {request.run}")
        tested = runs.record(request.run)["strategy"]
        if "source" not in tested:
            raise ValueError(f"run {request.run} predates recorded source copies; run it again")
        source = Path(tested["source"])

    def read(name, limit, root=root):
        relative = PurePosixPath(name)
        if not name or relative.is_absolute() or any(p in ('', '.', '..') for p in name.split('/')):
            raise ValueError(f"invalid package path: {name}")
        path = root
        for part in relative.parts:
            path = path / part
            if path.is_symlink():
                raise ValueError(f"package files cannot be symbolic links: {name}")
        if not path.is_file() or path.stat().st_size > limit:
            raise ValueError(f"package file missing or too large: {name}")
        with path.open('rb') as source:
            value = source.read(limit + 1)
        if len(value) > limit:
            raise ValueError(f"package file too large: {name}")
        return value

    manifest = Parse(read(request.manifest, 16384).decode('utf-8'), Manifest())
    if not 1 <= len(manifest.files) <= 64 or len(set(manifest.files)) != len(manifest.files):
        raise ValueError('a package lists 1 to 64 distinct Python files')
    if request.run:
        module, _, name = tested["id"].partition(".")
        if manifest.entry != f"strategies.{module}:{name}":
            raise ValueError(f"run {request.run} tested {tested['id']}, not the entry {manifest.entry}")
    manifest.research_run = request.run
    result = Package(manifest=manifest)
    remaining = 512 * 1024
    for name in manifest.files:
        if not name.endswith('.py'):
            raise ValueError(f'package files must be Python source: {name}')
        code = read(name, remaining, source)
        code.decode('utf-8')
        remaining -= len(code)
        result.files.add(path=name, content=code)
    return result
