"""Public package contract shared by SDK packaging and terminal admission."""

from typing import Literal

from asterion_bindings.artifacts import ArchivePolicy, CheckedArchive
from pydantic import BaseModel, ConfigDict, Field

__all__ = ["PackageManifest", "checked_archive", "checked_package", "load_package"]


class PackageManifest(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    api_version: Literal[1]
    id: str = Field(pattern=r"^[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+$", max_length=100)
    version: str = Field(pattern=r"^[0-9]+\.[0-9]+\.[0-9]+$", max_length=40)
    title: str = Field(min_length=1, max_length=80)
    description: str = Field(max_length=1000)
    layer: Literal["L3"]
    runtime: Literal["python"]
    entry: Literal["plugin.py"]
    trust: Literal["local-code"]
    # Strategy packages are the only installable extension; data sources and
    # broker connectors are built-in Rust implementations.
    contributions: dict[Literal["research.strategy"], dict] = Field(min_length=1, max_length=1)


_ARCHIVE_POLICY = ArchivePolicy(
    compressed_bytes=16_000_000,
    expanded_bytes=32_000_000,
    file_bytes=8_000_000,
    files=500,
    suffixes=(".py", ".json", ".txt", ".md", ".csv"),
    required=("manifest.json", "plugin.py"),
)


def checked_package(content: bytes) -> tuple[PackageManifest, CheckedArchive]:
    archive = CheckedArchive(content, _ARCHIVE_POLICY)
    return PackageManifest.model_validate_json(archive.read("manifest.json")), archive


def load_package(root, digest: str) -> tuple[PackageManifest, CheckedArchive]:
    archive = CheckedArchive.load(root, digest, _ARCHIVE_POLICY)
    return PackageManifest.model_validate_json(archive.read("manifest.json")), archive


def checked_archive(content: bytes) -> tuple[PackageManifest, dict[str, bytes]]:
    manifest, archive = checked_package(content)
    return manifest, {name: archive.read(name) for name in archive.names}
