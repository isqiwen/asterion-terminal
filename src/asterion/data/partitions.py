"""Copy-on-write monthly series with row-level observation provenance."""

import hashlib
import json
import time
from collections import defaultdict
from datetime import date, datetime, timedelta

import pyarrow as pa
import pyarrow.parquet as pq
from sqlalchemy import select

from asterion.data.artifacts import atomic_write
from asterion.data.library import collections, stable_id, versions
from asterion.data.providers.public import ProviderError
from asterion.platform.serialization import canonical

SERIES = "monthly-observed-v1"
SUPPORTED = {"futures.daily", "futures.calendar"}


def read_partition(root, part):
    path = root / "artifacts" / f"{part['checksum']}.parquet"
    try:
        content = path.read_bytes()
    except FileNotFoundError:
        raise ProviderError("分区文件缺失") from None
    if hashlib.sha256(content).hexdigest() != part["checksum"]:
        raise ProviderError("分区文件校验和不一致")
    return pq.ParquetFile(pa.BufferReader(content)).read(use_threads=False).to_pylist()


def calendar_gaps(rows, time_field):
    days = {row[time_field] for row in rows}
    cursor, end = date.fromisoformat(min(days)), date.fromisoformat(max(days))
    gaps, start = [], None
    while cursor <= end + timedelta(days=1):
        missing = cursor <= end and cursor.isoformat() not in days
        if missing and start is None:
            start = cursor
        if not missing and start is not None:
            gaps.append(
                {"start": start.isoformat(), "end": (cursor - timedelta(days=1)).isoformat()}
            )
            start = None
        cursor += timedelta(days=1)
    return gaps


def prepare(library, conn, *, job_id, type_id, source, scope, rows, observed_by_key):
    """Hold the collection lock until the caller commits its fenced publication."""
    data_type = library.types.get(type_id)
    definition = data_type.manifest
    time_field = definition.time_field
    if type_id not in SUPPORTED or time_field is None:
        raise ProviderError("该类型尚不支持累积发布")
    dataset_id = library.ensure_collection(conn, type_id, source, scope, "STANDARD", SERIES)
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
    previous = {}
    if parent:
        manifest = parent["manifest"]
        try:
            content = (library.root / manifest["path"]).read_bytes()
        except FileNotFoundError:
            raise ProviderError("父版本分区清单缺失") from None
        if hashlib.sha256(content).hexdigest() != manifest["checksum"]:
            raise ProviderError("父版本清单校验和不一致")
        previous = {part["key"]: part for part in json.loads(content)["partitions"]}

    incoming = defaultdict(list)
    for row in rows:
        incoming[row[time_field][:7]].append(row)
    changes = {"added": 0, "revised": 0, "refreshed": 0, "unchanged": 0, "stale_ignored": 0}
    partitions, cumulative = [], []

    def primary_key(row):
        return tuple(row[field] for field in definition.primary_key)

    for month in sorted(previous.keys() | incoming.keys()):
        old = read_partition(library.root, previous[month]) if month in previous else []
        merged = {primary_key(row): row for row in old}
        changed = False
        for row in incoming[month]:
            key = primary_key(row)
            observed_at = observed_by_key[key]
            current = merged.get(key)
            if current:
                same = row == {k: v for k, v in current.items() if not k.startswith("_")}
                incoming_time = datetime.fromisoformat(observed_at)
                current_time = datetime.fromisoformat(current["_observed_at"])
                if incoming_time < current_time:
                    changes["stale_ignored"] += 1
                    continue
                if incoming_time == current_time:
                    if not same:
                        raise ProviderError("相同采集时间出现不同记录，拒绝自动裁决")
                    changes["unchanged"] += 1
                    continue
                changes["refreshed" if same else "revised"] += 1
            else:
                changes["added"] += 1
            merged[key] = row | {"_observed_at": observed_at, "_raw_version_id": raw_id}
            changed = True
        values = sorted(merged.values(), key=lambda row: (row[time_field], primary_key(row)))
        if changed:
            output = pa.BufferOutputStream()
            pq.write_table(pa.Table.from_pylist(values), output)
            content = output.getvalue().to_pybytes()
            digest = hashlib.sha256(content).hexdigest()
            path = library.root / "artifacts" / f"{digest}.parquet"
            if path.exists():
                if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                    raise ProviderError("已存在的分区文件校验和不一致")
            else:
                atomic_write(path, content)
            part = {
                "key": month,
                "checksum": digest,
                "rows": len(values),
                "bytes": len(content),
                "first": values[0][time_field],
                "last": values[-1][time_field],
                "inputs": sorted({row["_raw_version_id"] for row in values}),
                "replaces": previous[month]["checksum"] if month in previous else None,
            }
        else:
            part = previous[month]
        partitions.append(part)
        cumulative.extend(values)
    data_type.validate(cumulative)
    coverage = "RETURNED_ROWS_ONLY"
    gaps = None
    if type_id == "futures.calendar":
        gaps = calendar_gaps(cumulative, time_field)
        coverage = "CALENDAR_GAPS" if gaps else "CALENDAR_COMPLETE"
    path = library.root / "datasets" / job_id / "partitions.json"
    atomic_write(path, canonical({"schema_version": 1, "partitions": partitions}))
    detail = {
        "observed_at": max((row["_observed_at"] for row in cumulative), key=datetime.fromisoformat),
        "available_at": max(
            (row["_observed_at"] for row in cumulative), key=datetime.fromisoformat
        ),
        "parent_version_id": parent["id"] if parent else None,
        "revision": parent["manifest"]["revision"] + 1 if parent else 1,
        "merge_policy": "LATEST_OBSERVED_ROW_NO_DELETIONS",
        "changes": changes,
        "partitions": partitions,
        "logical_bytes": sum(part["bytes"] for part in partitions),
        "first": cumulative[0][time_field],
        "last": cumulative[-1][time_field],
        "coverage": coverage,
        "coverage_gaps": gaps,
        "acquired_rows": len(rows),
    }
    return (
        cumulative,
        path,
        {
            "series": SERIES,
            "semantics": "CUMULATIVE",
            "format": "partition_manifest",
            "rows": len(cumulative),
            "parent_inputs": [parent["id"]] if parent else [],
            "created_at": max(time.time(), parent["created_at"] + 0.000001)
            if parent
            else time.time(),
            "detail": detail,
        },
    )
