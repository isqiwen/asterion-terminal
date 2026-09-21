"""Create deterministic source packages using only the standard library."""

import argparse
import hashlib
import io
import json
import zipfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(prog="python -m asterion_plugin_sdk")
    parser.add_argument("command", choices=["pack"])
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    root = args.source.resolve()
    manifest = json.loads((root / "manifest.json").read_text())
    if manifest.get("api_version") != 1 or manifest.get("entry") != "plugin.py":
        parser.error("The package must implement manifest protocol 1 and plugin.py")
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(root.rglob("*")):
            if path.is_symlink():
                parser.error("Symbolic links are not allowed")
            if not path.is_file():
                continue
            if path.suffix not in {".py", ".json", ".txt", ".md", ".csv"}:
                parser.error("Only Python sources and text resources can be packaged")
            info = zipfile.ZipInfo(path.relative_to(root).as_posix())
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100600 << 16
            archive.writestr(info, path.read_bytes())
    content = stream.getvalue()
    with args.output.open("xb") as target:
        target.write(content)
    print(hashlib.sha256(content).hexdigest())


if __name__ == "__main__":
    main()
