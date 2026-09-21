"""Content identity for the installed runtime, independent of paths and mtimes."""

import hashlib
import sys
from pathlib import Path


def tree_digest(roots: tuple[Path, ...]) -> str:
    digest = hashlib.sha256()
    for index, root in enumerate(roots):
        if not root.exists():
            raise ValueError("运行文件缺失，请重新安装完整应用")
        paths = [root] if root.is_file() else sorted(root.rglob("*"))
        for path in paths:
            relative = path.relative_to(root) if root.is_dir() else Path(root.name)
            if "__pycache__" in relative.parts or path.suffix == ".pyc":
                continue
            if not path.is_file():
                continue
            name = f"{index}/{relative.as_posix()}".encode()
            digest.update(len(name).to_bytes(8, "big"))
            digest.update(name)
            with path.open("rb") as stream:
                digest.update(hashlib.file_digest(stream, "sha256").digest())
    return digest.hexdigest()


def runtime_identity(pg_root: Path) -> str:
    if getattr(sys, "frozen", False):
        # Includes the executable, bundled Python/modules, strategy sources and PostgreSQL.
        return tree_digest((Path(sys.executable).resolve().parent, pg_root.resolve()))
    source = Path(__file__).resolve().parents[2]
    return tree_digest((source / "asterion", source / "asterion_plugin_sdk"))
