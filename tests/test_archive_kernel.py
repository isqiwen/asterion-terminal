"""Archive admission and immutable storage use the same fixed native mechanism."""

import hashlib
import io
import json
import struct
import zipfile
from dataclasses import replace
from pathlib import Path

import pytest
from asterion_bindings.artifacts import ArchivePolicy, CheckedArchive
from extension_support import package_content

from asterion.distribution import extension_packages
from asterion_plugin_sdk.packages import checked_package

POLICY = ArchivePolicy(
    compressed_bytes=16_000_000,
    expanded_bytes=32_000_000,
    file_bytes=8_000_000,
    files=500,
    suffixes=(".txt",),
    required=("data.txt",),
)


def archive_bytes(compression=zipfile.ZIP_STORED, content=b"archive data", *, name="data.txt"):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w", compression=compression) as archive:
        archive.writestr(name, content)
    return stream.getvalue()


@pytest.mark.parametrize(
    "compression", [zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED, zipfile.ZIP_BZIP2, zipfile.ZIP_LZMA]
)
def test_python_zip_codecs_preserve_original_archive_and_extracted_bytes(tmp_path, compression):
    content = archive_bytes(compression)
    archive = CheckedArchive(content, POLICY)
    assert archive.digest == hashlib.sha256(content).hexdigest()
    archive.publish(tmp_path)
    assert (tmp_path / archive.digest / ".archive").read_bytes() == content
    restored = CheckedArchive.load(tmp_path, archive.digest, POLICY)
    assert restored.names == ["data.txt"]
    assert restored.read("data.txt") == b"archive data"


@pytest.mark.parametrize("field,value", [("files", 0), ("file_bytes", 5), ("compressed_bytes", 5)])
def test_native_archive_policy_limits_are_enforced(field, value):
    with pytest.raises(ValueError):
        CheckedArchive(archive_bytes(), replace(POLICY, **{field: value}))


def test_duplicate_zip_entry_is_rejected_before_any_object_write(tmp_path):
    content = io.BytesIO()
    with zipfile.ZipFile(content, "w") as archive:
        archive.writestr("data.txt", "original")
        with pytest.warns(UserWarning, match="Duplicate"):
            archive.writestr("data.txt", "replacement")
    with pytest.raises(ValueError, match="duplicate"):
        CheckedArchive(content.getvalue(), POLICY).publish(tmp_path)
    assert list(tmp_path.iterdir()) == []


def test_lzma_dictionary_allocation_is_bounded_independently_of_expanded_size():
    content = bytearray(archive_bytes(zipfile.ZIP_LZMA))
    name_length, extra_length = struct.unpack_from("<HH", content, 26)
    data_start = 30 + name_length + extra_length
    struct.pack_into("<I", content, data_start + 5, 1 << 30)
    with pytest.raises(ValueError, match="decoder memory"):
        CheckedArchive(bytes(content), POLICY)


def test_sdk_and_host_validate_manifest_separately_from_generic_archive(tmp_path):
    content = package_content()
    manifest, archive = checked_package(content)
    assert manifest.layer == "L3"
    # The native mechanism only sees paths and bytes, including arbitrary data.
    generic = CheckedArchive(archive_bytes(content=b"not an extension manifest"), POLICY)
    generic.publish(tmp_path)
    assert generic.read("data.txt") == b"not an extension manifest"
    assert archive.digest == hashlib.sha256(content).hexdigest()


def test_existing_object_corruption_is_not_repaired_or_registered(tmp_path):
    packages = extension_packages(tmp_path)
    content = package_content()
    _, archive = checked_package(content)
    objects = packages.root / "objects"
    objects.mkdir(parents=True)
    archive.publish(objects)
    target = objects / archive.digest
    (target / "plugin.py").write_bytes(b"changed historical content")
    with pytest.raises(ValueError, match="变化"):
        packages.install(content)
    assert not (packages.root / "installed.json").exists()
    assert (target / "plugin.py").read_bytes() == b"changed historical content"
    assert (target / ".archive").read_bytes() == content


def test_native_publication_failure_does_not_commit_install_receipt(tmp_path, monkeypatch):
    packages = extension_packages(tmp_path)
    original = packages.install(package_content())
    previous = (packages.root / "installed.json").read_bytes()

    def fail(_archive, _root):
        raise OSError("publication unavailable")

    monkeypatch.setattr(CheckedArchive, "publish", fail)
    with pytest.raises(OSError, match="unavailable"):
        packages.install(package_content(identifier="test.other"))
    assert (packages.root / "installed.json").read_bytes() == previous
    assert json.loads(previous)["packages"]["test.strategy"]["digest"] == original["digest"]


def test_object_symlink_never_redirects_immutable_publication(tmp_path):
    archive = CheckedArchive(archive_bytes(), POLICY)
    outside = tmp_path / "outside"
    outside.mkdir()
    objects = tmp_path / "objects"
    objects.mkdir()
    (objects / archive.digest).symlink_to(outside, target_is_directory=True)
    with pytest.raises(ValueError, match="links"):
        archive.publish(objects)
    assert list(outside.iterdir()) == []
    assert Path(objects / archive.digest).is_symlink()
