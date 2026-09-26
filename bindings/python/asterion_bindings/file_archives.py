"""Value conversion and scoped ownership for Rust streaming file archives."""

import json
from dataclasses import asdict, dataclass
from pathlib import Path

from asterion_bindings import _native


@dataclass(frozen=True)
class ArchiveLimits:
    bytes: int
    files: int
    directories: int
    metadata_bytes: int


class StagedDirectory:
    def __init__(self, destination: Path, *, max_entries: int):
        self._handle = _native.StagedDirectory(destination, max_entries)

    @property
    def path(self) -> Path:
        return self._handle.path

    def commit(self) -> None:
        self._handle.commit()

    def preserve(self) -> Path:
        return self._handle.preserve()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self._handle.close()


class FileArchiveWriter:
    def __init__(self, source, destination, *, roots, excludes, limits, metadata_name):
        self._handle = _native.FileArchiveWriter(
            source, destination, roots, excludes, json.dumps(asdict(limits)), metadata_name
        )

    def catalog(self) -> dict:
        return json.loads(self._handle.catalog())

    def commit(self, metadata: bytes) -> dict:
        return json.loads(self._handle.commit(metadata))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self._handle.close()


class FileArchiveReader:
    def __init__(self, path: Path, *, limits: ArchiveLimits, metadata_name: str):
        self._handle = _native.FileArchiveReader(path, json.dumps(asdict(limits)), metadata_name)

    def metadata(self) -> bytes:
        return self._handle.metadata()

    def digest(self) -> str:
        return self._handle.digest()

    def extract(self, stage: StagedDirectory, files: dict, directories: list[str]) -> None:
        self._handle.extract(stage._handle, json.dumps(files), json.dumps(directories))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self._handle.close()
