"""Read an explicit code-only package from the user's strategy repository.

Building a package never imports strategy code or collects the whole repository.
Trading is the authority for package admission and immutable publication.
"""
from pathlib import Path, PurePosixPath

from google.protobuf.json_format import Parse
from asterion.strategy_pb2 import Manifest, Package
from asterion_lab.repository import checked


def build(request):
    root = checked(Path(request.repository)).resolve()

    def read(name, limit):
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
    result = Package(manifest=manifest)
    remaining = 512 * 1024
    for name in manifest.files:
        if not name.endswith('.py'):
            raise ValueError(f'package files must be Python source: {name}')
        code = read(name, remaining)
        code.decode('utf-8')
        remaining -= len(code)
        result.files.add(path=name, content=code)
    return result
