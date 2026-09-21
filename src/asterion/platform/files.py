import hashlib
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


def file_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


@dataclass(frozen=True)
class ReadFiles:
    read: Callable[[str], bytes]
    digest: Callable[[str], str]
    scan: Callable[[str, str], tuple[str, ...]]


def read_files(root: Path) -> ReadFiles:
    root = root.resolve(strict=True)

    def resolve(name):
        if not isinstance(name, str) or not name or "\\" in name:
            raise ValueError("Invalid backup file path")
        if PurePosixPath(name).is_absolute() or any(
            part in {"", ".", ".."} for part in name.split("/")
        ):
            raise ValueError("Invalid backup file path")
        path = root
        for part in name.split("/"):
            path = path / part
            if path.is_symlink():
                raise ValueError("Backup file links are not allowed")
        if not path.resolve().is_relative_to(root):
            raise ValueError("Backup file is outside granted root")
        return path

    def scan(name, suffix):
        directory = resolve(name)
        if not directory.exists():
            return ()
        if not directory.is_dir():
            raise ValueError("Backup scan requires a directory")
        found = []
        for path in directory.rglob("*"):
            relative = path.relative_to(root).as_posix()
            checked = resolve(relative)
            if checked.is_file() and checked.name.endswith(suffix):
                found.append(relative)
        return tuple(sorted(found))

    return ReadFiles(
        lambda name: resolve(name).read_bytes(), lambda name: file_digest(resolve(name)), scan
    )
