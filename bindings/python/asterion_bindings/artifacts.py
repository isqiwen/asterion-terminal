"""Fixed L1 bounded ZIP admission, immutable objects and write-once artifacts."""

import json
from dataclasses import asdict, dataclass
from pathlib import Path

from asterion_bindings._native import ArchiveHandle, ArtifactStoreHandle


@dataclass(frozen=True)
class ArchivePolicy:
    compressed_bytes: int
    expanded_bytes: int
    file_bytes: int
    files: int
    suffixes: tuple[str, ...]
    required: tuple[str, ...]


class CheckedArchive:
    def __init__(self, content: bytes, policy: ArchivePolicy):
        self._handle = ArchiveHandle(content, json.dumps(asdict(policy)))

    @classmethod
    def load(cls, root: Path, digest: str, policy: ArchivePolicy) -> "CheckedArchive":
        instance = cls.__new__(cls)
        instance._handle = ArchiveHandle.load(root, digest, json.dumps(asdict(policy)))
        return instance

    @property
    def digest(self) -> str:
        return self._handle.digest

    @property
    def names(self) -> list[str]:
        return self._handle.names

    def read(self, name: str) -> bytes:
        return self._handle.read(name)

    def publish(self, root: Path) -> None:
        self._handle.publish(root)


@dataclass(frozen=True)
class ArtifactRef:
    """Write-once bytes below a granted root; the digest proves content only."""

    name: str
    sha256: str
    bytes: int


class ArtifactStore:
    """Names never change content: identical retries succeed, different bytes fail.

    Reads verify the recorded digest (and size when recorded). A missing file
    raises FileNotFoundError; changed or mismatched content raises ValueError.
    A ``read_only`` grant (for restored or copied state) rejects every write.
    """

    def __init__(self, root: Path, *, max_bytes: int = 1024**3, read_only: bool = False):
        self._handle = ArtifactStoreHandle(root, max_bytes, not read_only)

    def put(self, name: str, content: bytes) -> ArtifactRef:
        return ArtifactRef(*self._handle.put(name, content))

    def put_addressed(self, directory: str, suffix: str, content: bytes) -> ArtifactRef:
        """Store as ``<directory>/<sha256><suffix>``."""
        return ArtifactRef(*self._handle.put_addressed(directory, suffix, content))

    def read(self, name: str, sha256: str, size: int | None = None) -> bytes:
        return self._handle.read(name, sha256, size)

    def verify(self, name: str, sha256: str, size: int | None = None) -> None:
        """Stream-verify recorded bytes without loading them."""
        self._handle.verify(name, sha256, size)
