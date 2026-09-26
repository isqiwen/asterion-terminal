"""Create deterministic source packages using the public package contract."""

import argparse
import io
import zipfile
from pathlib import Path

from .packages import checked_package


def main():
    parser = argparse.ArgumentParser(prog="python -m asterion_plugin_sdk")
    parser.add_argument("command", choices=["pack"])
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    root = args.source.resolve()
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(root.rglob("*")):
            if path.is_symlink():
                parser.error("Symbolic links are not allowed")
            if not path.is_file():
                continue
            info = zipfile.ZipInfo(path.relative_to(root).as_posix())
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100600 << 16
            archive.writestr(info, path.read_bytes())
    content = stream.getvalue()
    try:
        _, checked = checked_package(content)
    except ValueError as error:
        parser.error(f"Invalid plugin package: {error}")
    with args.output.open("xb") as target:
        target.write(content)
    print(checked.digest)


if __name__ == "__main__":
    main()
