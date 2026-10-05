"""Packaging from reused directories must contain exactly the current resources."""
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import desktop

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    build, destination = root / "build", root / "staged"
    for name in desktop.NATIVE_FILES:
        source = build / name
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(("current fixture: " + name).encode())
    # An obsolete plugin in both a reused build and reused output must disappear.
    (build / "plugins/obsolete.dylib").write_bytes(b"old plugin")
    (destination / "plugins").mkdir(parents=True)
    (destination / "plugins/obsolete.dylib").write_bytes(b"old plugin")
    desktop.stage_native_resources(build, destination)
    desktop.verify_native_resources(destination)
    assert not (destination / "plugins/obsolete.dylib").exists()
    for name in desktop.NATIVE_FILES:
        assert (destination / name).read_bytes() == (build / name).read_bytes()
    # Reject corruption of the generated set before invoking the packager.
    (destination / "unlisted").write_bytes(b"unexpected")
    try:
        desktop.verify_native_resources(destination)
    except ValueError:
        pass
    else:
        raise AssertionError("unlisted resource accepted")
    (destination / "unlisted").unlink()
    (build / "asterion-trading").unlink()
    before = (destination / "asterion-trading").read_bytes()
    try:
        desktop.stage_native_resources(build, destination)
    except ValueError:
        pass
    else:
        raise AssertionError("missing current executable accepted")
    assert (destination / "asterion-trading").read_bytes() == before
    outside = root / "outside"
    outside.write_bytes(b"preserve")
    (build / "asterion-trading").symlink_to(outside)
    try:
        desktop.stage_native_resources(build, destination)
    except ValueError:
        pass
    else:
        raise AssertionError("symlink executable accepted")
    assert outside.read_bytes() == b"preserve"
print("Fresh native staging excludes obsolete plugins and rejects missing, extra and symlink resources")
