"""Bundle the installed PostgreSQL runtime, relocating all non-system Mach-O libraries."""

import argparse
import os
import shutil
import subprocess
from pathlib import Path


def dependencies(path: Path) -> list[str]:
    output = subprocess.check_output(["/usr/bin/otool", "-L", str(path)], text=True)
    return [line.strip().split(" (", 1)[0] for line in output.splitlines()[1:]]


def bundle(source: Path, destination: Path):
    if destination.exists():
        shutil.rmtree(destination)
    (destination / "bin").mkdir(parents=True)
    (destination / "lib" / "deps").mkdir(parents=True)
    (destination / "lib" / "postgresql@17").mkdir()
    shutil.copytree(source / "share" / "postgresql", destination / "share" / "postgresql@17")
    pending: list[tuple[Path, Path]] = []
    for name in ("postgres", "initdb", "pg_ctl"):
        target = destination / "bin" / name
        shutil.copy2(source / "bin" / name, target)
        pending.append((source / "bin" / name, target))
    for name in ("plpgsql.dylib", "dict_snowball.dylib"):
        target = destination / "lib" / "postgresql@17" / name
        original = source / "lib" / "postgresql" / name
        shutil.copy2(original, target)
        pending.append((original, target))
    copied: dict[str, Path] = {}
    while pending:
        original, target = pending.pop(0)
        target.chmod(target.stat().st_mode | 0o200)
        for reference in dependencies(original):
            if reference.startswith(("/usr/lib/", "/System/Library/")):
                continue
            resolved = Path(reference)
            if reference.startswith("@loader_path/"):
                resolved = original.parent / reference.removeprefix("@loader_path/")
            if not resolved.is_absolute() or not resolved.exists():
                raise RuntimeError(f"Unresolved PostgreSQL dependency: {original}: {reference}")
            # A dylib's own install ID is the first otool entry, not a dependency.
            if resolved.resolve() == original.resolve():
                continue
            name = resolved.name
            bundled = destination / "lib" / "deps" / name
            if name not in copied:
                copied[name] = resolved.resolve()
                shutil.copy2(resolved, bundled)
                pending.append((resolved, bundled))
            elif copied[name] != resolved.resolve():
                raise RuntimeError(f"Conflicting runtime libraries: {name}")
            relative = os.path.relpath(bundled, target.parent)
            subprocess.run(
                [
                    "/usr/bin/install_name_tool",
                    "-change",
                    reference,
                    f"@loader_path/{relative}",
                    str(target),
                ],
                check=True,
                stdout=subprocess.DEVNULL,
            )
        if target.suffix == ".dylib":
            subprocess.run(
                ["/usr/bin/install_name_tool", "-id", f"@rpath/{target.name}", str(target)],
                check=True,
            )
        subprocess.run(
            ["/usr/bin/codesign", "--force", "--sign", "-", str(target)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    (destination / "RUNTIME.txt").write_text(
        "PostgreSQL 17 runtime, built from the local Homebrew distribution.\n"
        "Third-party Mach-O libraries are copied and linked relative to this bundle.\n"
        "PostgreSQL: https://www.postgresql.org/about/licence/\n"
    )
    # Reject build-machine dependencies after relocation.
    for path in destination.rglob("*"):
        if path.is_file() and (path.suffix in {".dylib", ".so"} or path.parent.name == "bin"):
            for dependency in dependencies(path):
                if dependency.startswith("/opt/"):
                    raise RuntimeError(f"Non-portable library reference: {path}: {dependency}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path("/opt/homebrew/opt/postgresql@17"))
    parser.add_argument(
        "--destination", type=Path, default=Path("apps/terminal/src-tauri/runtime/postgres")
    )
    args = parser.parse_args()
    bundle(args.source, args.destination)
