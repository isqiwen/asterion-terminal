"""Relocate PostgreSQL 17 and its non-glibc ELF dependencies for Linux bundles."""

import argparse
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

# The loader and glibc must come from the target OS as a matching set.
SYSTEM_LIBRARY = re.compile(
    r"^(ld-linux.*|ld64\.so.*|lib(c|m|pthread|dl|rt|resolv|util|anl)\.so\..*)$"
)


def output(*args: str) -> str:
    return subprocess.check_output(args, text=True).strip()


def dependencies(path: Path) -> dict[str, Path]:
    listing = output("ldd", str(path))
    if "not found" in listing:
        raise RuntimeError(f"Missing ELF dependency for {path}:\n{listing}")
    result = {}
    for line in listing.splitlines():
        match = re.match(r"\s*(\S+) => (/\S+) \(", line)
        if match and not SYSTEM_LIBRARY.match(match[1]):
            result[match[1]] = Path(match[2]).resolve()
    return result


def bundle(pg_config: str, destination: Path, sysroot: Path = Path("/")):
    if not output(pg_config, "--version").startswith("PostgreSQL 17."):
        raise RuntimeError("Desktop bundles require PostgreSQL 17 (set --pg-config)")
    paths = {
        key: Path(output(pg_config, f"--{key}"))
        for key in (
            "bindir",
            "sharedir",
            "pkglibdir",
        )
    }
    # Preserve compiled path suffixes. PostgreSQL uses these to relocate $libdir
    # (including plpgsql during initdb), not just to locate its share directory.
    prefix = Path(os.path.commonpath(list(paths.values())))
    layout = {key: str(path.relative_to(prefix)) for key, path in paths.items()}
    sources = {key: sysroot / path.relative_to("/") for key, path in paths.items()}
    for name in ("postgres", "initdb", "pg_ctl"):
        if not (sources["bindir"] / name).is_file():
            raise RuntimeError(f"Missing PostgreSQL server executable: {sources['bindir'] / name}")
    if destination.exists():
        shutil.rmtree(destination)
    library_dir = destination / "lib/deps"
    library_dir.mkdir(parents=True)
    shutil.copytree(sources["sharedir"], destination / layout["sharedir"])
    pending = []
    for key, names in (
        ("bindir", ("postgres", "initdb", "pg_ctl")),
        ("pkglibdir", ("plpgsql.so", "dict_snowball.so")),
    ):
        target_dir = destination / layout[key]
        target_dir.mkdir(parents=True, exist_ok=True)
        for name in names:
            source, target = sources[key] / name, target_dir / name
            shutil.copy2(source, target)
            pending.append((source, target))
    copied = {}
    bundled = []
    while pending:
        original, target = pending.pop(0)
        for name, source in dependencies(original).items():
            if name in copied:
                if copied[name] != source:
                    raise RuntimeError(f"Conflicting ELF dependency: {name}")
                continue
            copied[name] = source
            dependency = library_dir / name
            shutil.copy2(source, dependency)
            pending.append((source, dependency))
        target.chmod(target.stat().st_mode | 0o200)
        relative = os.path.relpath(library_dir, target.parent)
        subprocess.run(
            ["patchelf", "--set-rpath", f"$ORIGIN/{relative}", str(target)],
            check=True,
        )
        bundled.append(target)
    for target in bundled:
        for dependency in dependencies(target).values():
            if not dependency.is_relative_to(destination.resolve()):
                raise RuntimeError(f"Non-portable ELF dependency: {target}: {dependency}")
    (destination / "layout.json").write_text(json.dumps(layout, indent=2) + "\n")
    (destination / "RUNTIME.txt").write_text(
        output(pg_config, "--version") + "\n"
        "ELF dependencies bundled except glibc and the system loader.\n"
        "Requires a compatible Linux distribution/architecture and glibc.\n"
        "PostgreSQL: https://www.postgresql.org/about/licence/\n"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--pg-config", default=os.environ.get("PG_CONFIG", "pg_config"))
    parser.add_argument("--sysroot", type=Path, default=Path("/"))
    parser.add_argument(
        "--destination",
        type=Path,
        default=Path("apps/terminal/src-tauri/runtime/postgres"),
    )
    args = parser.parse_args()
    bundle(args.pg_config, args.destination.resolve(), args.sysroot)
