"""Partitioned versions through the sole Rust L2 implementation; no merge rules here."""

import json

import pyarrow as pa

from . import _native
from ._call import invoke
from .artifacts import ArtifactRef, ArtifactStore
from .data_store import MergeRequest, Partition, PartitionedVersion
from .local import current_lifetimes

__all__ = ["cumulative_series", "merge", "read_index", "read_partition", "verify_version"]


def cumulative_series(type_id: str) -> str | None:
    """The cumulative version series of a data type, or None when unsupported."""
    return invoke("data_store", "cumulative_series", {"type_id": type_id})


def merge(store: ArtifactStore, request: MergeRequest) -> dict:
    """Write changed partitions and the index; return detail and touched rows.

    Raises ValueError with the domain reason when inputs, the parent version or
    stored partitions violate the current contract.
    """
    outcome = json.loads(
        _native.data_store_merge(
            store._handle, list(current_lifetimes()), request.model_dump_json()
        )
    )
    outcome["index"] = ArtifactRef(**outcome["index"])
    return outcome


def read_index(store: ArtifactStore, version: PartitionedVersion) -> tuple[Partition, ...]:
    """Verify the recorded index artifact equals the version's partitions."""
    value = _native.data_store_read_index(
        store._handle, list(current_lifetimes()), version.model_dump_json()
    )
    return tuple(Partition.model_validate(part) for part in json.loads(value))


def verify_version(store: ArtifactStore, version: PartitionedVersion) -> tuple[Partition, ...]:
    """Verify the index and every partition's bytes without decoding rows."""
    value = _native.data_store_verify_version(
        store._handle, list(current_lifetimes()), version.model_dump_json()
    )
    return tuple(Partition.model_validate(part) for part in json.loads(value))


def read_partition(store: ArtifactStore, part: Partition) -> pa.RecordBatch:
    """One complete checksum-verified partition, including provenance columns."""
    return _native.data_store_read_partition(
        store._handle, list(current_lifetimes()), part.model_dump_json()
    )
