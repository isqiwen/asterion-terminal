"""Application orchestration of cumulative versions; merge and verification are Rust L2."""

import time

from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.data_partitions import (
    cumulative_series,
    merge,
    read_index,
    verify_version,
)
from asterion_bindings.data_partitions import read_partition as native_partition
from asterion_bindings.data_store import MergeRequest, ParentVersion, Partition, PartitionedVersion
from pydantic import ValidationError
from sqlalchemy import select

from asterion.data.library import collections, stable_id, versions
from asterion.data.providers.public import ProviderError


def _partitioned(manifest) -> PartitionedVersion:
    return PartitionedVersion(
        path=manifest["path"],
        checksum=manifest["checksum"],
        bytes=manifest["bytes"],
        partitions=manifest["partitions"],
    )


def version_partitions(store: ArtifactStore, manifest) -> tuple[Partition, ...]:
    """Partitions of a fixed version, verified against its recorded index artifact."""
    try:
        return read_index(store, _partitioned(manifest))
    except ValidationError:
        raise ProviderError("版本分区记录不符合当前契约") from None
    except ValueError as error:
        raise ProviderError(str(error)) from None


def verified_partitions(store: ArtifactStore, manifest) -> tuple[Partition, ...]:
    """Verify a fixed version's index and every partition's bytes without decoding."""
    try:
        return verify_version(store, _partitioned(manifest))
    except ValidationError:
        raise ProviderError("版本分区记录不符合当前契约") from None
    except ValueError as error:
        raise ProviderError(str(error)) from None


def read_partition(store: ArtifactStore, part: Partition) -> list[dict]:
    try:
        return native_partition(store, part).to_pylist()
    except ValueError as error:
        raise ProviderError(str(error)) from None


def prepare(library, conn, *, job_id, type_id, source, scope, rows, observed_by_key):
    """Hold the collection lock until the caller commits its fenced publication."""
    definition = library.types.get(type_id).manifest
    series = cumulative_series(type_id)
    if series is None or definition.time_field is None:
        raise ProviderError("该类型尚不支持累积发布")
    if not rows:
        raise ProviderError("累积发布记录为空")
    dataset_id = library.ensure_collection(conn, type_id, source, scope, "STANDARD", series)
    conn.execute(
        select(collections.c.id).where(collections.c.id == dataset_id).with_for_update()
    ).one()
    parent = (
        conn.execute(
            select(versions)
            .where(versions.c.dataset_id == dataset_id)
            .order_by(versions.c.created_at.desc(), versions.c.id.desc())
            .limit(1)
        )
        .mappings()
        .first()
    )
    raw_dataset_id = stable_id(library.identity(type_id, source, scope, "RAW"))
    raw_id = stable_id({"dataset_id": raw_dataset_id, "job_id": job_id})
    columns = tuple(rows[0])
    if any(row.keys() != rows[0].keys() for row in rows):
        raise ProviderError("采集记录字段不一致")
    try:
        previous = None
        if parent:
            manifest = parent["manifest"]
            previous = ParentVersion(
                id=parent["id"],
                revision=manifest["revision"],
                observed_at=manifest["observed_at"],
                coverage_gaps=manifest["coverage_gaps"],
                first=manifest["first"],
                last=manifest["last"],
                index=_partitioned(manifest),
            )
        request = MergeRequest(
            job_id=job_id,
            type_id=type_id,
            time_field=definition.time_field,
            primary_key=tuple(definition.primary_key),
            raw_version_id=raw_id,
            parent=previous,
            columns=columns,
            rows=tuple(tuple(row[column] for column in columns) for row in rows),
            observed_at=tuple(
                observed_by_key[tuple(row[field] for field in definition.primary_key)]
                for row in rows
            ),
        )
        outcome = merge(library.artifacts, request)
    except ValidationError:
        raise ProviderError("累积发布输入或父版本记录不符合当前契约") from None
    except ValueError as error:
        raise ProviderError(str(error)) from None
    return (
        outcome["touched"],
        outcome["index"],
        {
            "series": outcome["series"],
            "semantics": "CUMULATIVE",
            "format": "partition_manifest",
            "rows": outcome["rows"],
            "parent_inputs": [parent["id"]] if parent else [],
            "created_at": max(time.time(), parent["created_at"] + 0.000001)
            if parent
            else time.time(),
            "detail": outcome["detail"],
            "metrics": outcome["metrics"],
        },
    )
