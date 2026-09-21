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


def postgres_identity_roots(pg_root: Path) -> tuple[Path, ...]:
    if sys.platform == "linux":
        return (pg_root / "lib/postgresql/17", pg_root / "share/postgresql/17")
    return (pg_root.resolve(),)


def runtime_identity(pg_root: Path) -> str:
    environment = Path(sys.prefix)
    # Installed environments include interpreter, locked dependencies and PostgreSQL.
    # The developer checkout is hashed directly when running development commands.
    if (environment.parent / "ready").is_file():
        return tree_digest(
            (environment, environment.parent / "interpreter", *postgres_identity_roots(pg_root))
        )
    source = Path(__file__).resolve().parents[2]
    return tree_digest((source / "asterion", source / "asterion_plugin_sdk"))
