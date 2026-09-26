"""Opaque host-created file capabilities; path checks and I/O execute in Rust."""

from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

from . import _native
from ._native import FilePublicationError

__all__ = [
    "FilePublicationError",
    "ReadFiles",
    "atomic_write",
    "file_digest",
    "file_lock",
    "read_files",
    "trusted_tree_digest",
]


@dataclass(frozen=True)
class ReadFiles:
    read: Callable[[str], bytes]
    digest: Callable[[str], str]
    scan: Callable[[str, str], tuple[str, ...]]


def read_files(
    root: Path, *, max_read_bytes: int = 1024**3, max_scan_entries: int = 1_000_000
) -> ReadFiles:
    handle = _native.ReadFilesHandle(root, max_read_bytes, max_scan_entries)
    return ReadFiles(
        handle.read,
        handle.digest,
        lambda name, suffix: tuple(handle.scan(name, suffix)),
    )


def file_digest(path: Path) -> str:
    return _native.file_digest(path)


def trusted_tree_digest(
    roots: tuple[Path, ...],
    *,
    excluded_components: tuple[str, ...] = (),
    excluded_suffixes: tuple[str, ...] = (),
    max_entries: int = 1_000_000,
    max_bytes: int = 32 * 1024**3,
) -> str:
    """Hash trusted product trees; native code owns traversal and stability checks."""
    return _native.trusted_tree_digest(
        list(roots), list(excluded_components), list(excluded_suffixes), max_entries, max_bytes
    )


def atomic_write(path: Path, content: bytes, *, replace: bool = True) -> None:
    _native.file_atomic_write(path, content, replace)


@contextmanager
def file_lock(path: Path, *, blocking: bool = True, timeout: float | None = None) -> Iterator[None]:
    handle = _native.FileLockHandle(path, blocking, timeout)
    try:
        yield
    finally:
        handle.close()
