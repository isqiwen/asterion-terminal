"""Packaging from reused directories must contain exactly the current resources."""
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts/desktop"))
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
    (build / "asterion_trading").unlink()
    before = (destination / "asterion_trading").read_bytes()
    try:
        desktop.stage_native_resources(build, destination)
    except ValueError:
        pass
    else:
        raise AssertionError("missing current executable accepted")
    assert (destination / "asterion_trading").read_bytes() == before
    outside = root / "outside"
    outside.write_bytes(b"preserve")
    (build / "asterion_trading").symlink_to(outside)
    try:
        desktop.stage_native_resources(build, destination)
    except ValueError:
        pass
    else:
        raise AssertionError("symlink executable accepted")
    assert outside.read_bytes() == b"preserve"
print("Fresh native staging excludes obsolete plugins and rejects missing, extra and symlink resources")

# Exercise real bundle selection/staging, replacing only the expensive container build.
import json
import subprocess
from unittest.mock import patch
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "node"))
from bundle_fixture import make_bundle

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    fixture = make_bundle(root / "fixture") / "x86_64"
    archives, destination = root / "custom-bundles", root / "remote"
    archives.mkdir()
    archive = archives / "asterion-services-linux-x86_64.zip"

    def write_bundle(overrides=None, corrupt=False):
        manifest = json.loads((fixture / "manifest.json").read_text())
        manifest.update(overrides or {})
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("manifest.json", json.dumps(manifest))
            for name in manifest["files"]:
                data = (fixture / name).read_bytes()
                output.writestr(name, b"corrupt" if corrupt and name == "asterion_node_agent" else data)

    def build(arguments, *, environment):
        assert arguments == ["bash", str(desktop.ROOT / "scripts/services/build_linux_services.sh")]
        assert environment["ASTERION_LINUX_BUNDLES"] == str(archives)
        write_bundle()

    write_bundle()
    with patch.object(desktop, "run") as builder:
        desktop.prepare_installer_services(archives, destination)
        builder.assert_not_called()
    for outdated in (None, {"product_version": "0.0.0"}):
        if outdated is None:
            archive.unlink()
        else:
            write_bundle(outdated)
        with patch.object(desktop, "run", side_effect=build) as builder:
            desktop.prepare_installer_services(archives, destination)
            builder.assert_called_once()
        assert (destination / "x86_64/asterion_node_agent").read_bytes() == (fixture / "asterion_node_agent").read_bytes()
    write_bundle(corrupt=True)
    with patch.object(desktop, "run") as builder:
        try:
            desktop.prepare_installer_services(archives, destination)
        except ValueError as error:
            assert "checksum mismatch" in str(error)
        else:
            raise AssertionError("corrupt bundle accepted")
        builder.assert_not_called()
    archive.unlink()
    with patch.object(desktop, "run", side_effect=subprocess.CalledProcessError(1, "container build")) as builder:
        try:
            desktop.prepare_installer_services(archives, destination)
        except subprocess.CalledProcessError:
            pass
        else:
            raise AssertionError("failed build accepted")
        builder.assert_called_once()
print("Installer reuses matching services, builds missing/outdated bundles once, and reports corruption/build failures")
