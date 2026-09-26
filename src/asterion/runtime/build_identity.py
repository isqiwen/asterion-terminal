"""Content identity for the installed runtime, independent of paths and mtimes."""

import sys
from pathlib import Path

from asterion_bindings.files import trusted_tree_digest


def tree_digest(roots: tuple[Path, ...]) -> str:
    try:
        return trusted_tree_digest(
            roots, excluded_components=("__pycache__",), excluded_suffixes=(".pyc",)
        )
    except FileNotFoundError:
        raise ValueError("运行文件缺失，请重新安装完整应用") from None


def postgres_identity_roots(pg_root: Path) -> tuple[Path, ...]:
    if sys.platform == "linux":
        return (pg_root / "lib/postgresql/17", pg_root / "share/postgresql/17")
    return (pg_root.resolve(),)


def runtime_identity(pg_root: Path) -> str:
    import asterion_bindings

    environment = Path(sys.prefix)
    # Installed environments include interpreter, locked dependencies and PostgreSQL.
    # The developer checkout is hashed directly when running development commands.
    if (environment.parent / "ready").is_file():
        return tree_digest(
            (environment, environment.parent / "interpreter", *postgres_identity_roots(pg_root))
        )
    source = Path(__file__).resolve().parents[2]
    bindings = Path(asterion_bindings.__file__).resolve().parent
    return tree_digest((source / "asterion", source / "asterion_plugin_sdk", bindings))
