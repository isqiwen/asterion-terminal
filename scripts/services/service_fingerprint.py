"""Deterministic identity of service, protocol, dependency and build inputs.

Independent of Git cleanliness, absolute checkout path and client-only UI edits.
This is a build consistency check, not an artifact signature.
"""
import hashlib
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
TREES = ("core", "protocol", "plugins", "bindings", "apps/services", "conan", "scripts/node")
SUFFIXES = {".cpp", ".hpp", ".h", ".c", ".proto", ".cmake", ".py", ".json"}
FILES = ("CMakeLists.txt", "conanfile.py", "conan.lock", "scripts/services/service_fingerprint.py",
         "scripts/services/deployment_bundle.py", "scripts/prepare_ctp.py")


def inputs(root=ROOT):
    paths = {root / name for name in FILES}
    for tree in TREES:
        paths.update(path for path in (root / tree).rglob("*")
                     if path.is_file() and (path.suffix in SUFFIXES or path.name == "CMakeLists.txt"))
    for path in paths:
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Missing or symbolic service input: {path}")
    return sorted(paths, key=lambda path: path.relative_to(root).as_posix())


def fingerprint(root=ROOT):
    digest = hashlib.sha256(b"asterion.service-source.v1\0")
    for path in inputs(root):
        name = path.relative_to(root).as_posix().encode()
        data = path.read_bytes()
        digest.update(len(name).to_bytes(8, "big"))
        digest.update(name)
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)
    return digest.hexdigest()


if __name__ == "__main__":
    print(";".join(str(path) for path in inputs()) if "--files" in sys.argv else fingerprint())
