"""Unified catalogue of immutable acquisition and cumulative versions."""

import csv
import hashlib
import io
import json
import time
from pathlib import Path
from uuid import NAMESPACE_URL, uuid5

import pyarrow as pa
import pyarrow.parquet as pq
from sqlalchemy import JSON, Column, Float, ForeignKey, Integer, String, Table, func, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert

from asterion.data.catalog import snapshots
from asterion.data.providers.public import ProviderError
from asterion.data.types import builtin_types
from asterion.data.version_state import version_states
from asterion.platform.store import metadata

collections = Table(
    "data_collections",
    metadata,
    Column("id", String, primary_key=True),
    Column("type_id", String, nullable=False, index=True),
    Column("domain", String, nullable=False, index=True),
    Column("source", String, nullable=False, index=True),
    Column("layer", String, nullable=False, index=True),
    Column("identity", JSON, nullable=False),
)
versions = Table(
    "data_versions",
    metadata,
    Column("id", String, primary_key=True),
    Column("dataset_id", ForeignKey("data_collections.id"), nullable=False, index=True),
    Column("job_id", String, nullable=False, index=True),
    Column("created_at", Float, nullable=False),
    Column("rows", Integer, nullable=False),
    Column("manifest", JSON, nullable=False),
)


def stable_id(value):
    return str(uuid5(NAMESPACE_URL, "asterion:data:v1:" + json.dumps(value, sort_keys=True)))


class DataLibrary:
    def __init__(self, engine, root: Path):
        self.engine, self.root = engine, root
        self.types = builtin_types()
        engine.initialize(collections)
        engine.initialize(versions)
        engine.initialize(version_states)

    def identity(self, type_id, source, scope, layer, series=None):
        definition = self.types.get(type_id).manifest
        identity = {
            "type_id": type_id,
            "source": source,
            "scope": scope,
            "layer": layer,
            "schema_version": definition.schema_version,
            "frequency": scope.get("frequency", definition.frequency),
        }
        if series:
            identity["version_series"] = series
        return identity

    def ensure_collection(self, conn, type_id, source, scope, layer, series=None):
        identity = self.identity(type_id, source, scope, layer, series)
        dataset_id = stable_id(identity)
        insert = pg_insert if conn.dialect.name == "postgresql" else sqlite_insert
        conn.execute(
            insert(collections)
            .values(
                id=dataset_id,
                type_id=type_id,
                domain=self.types.get(type_id).manifest.domain,
                source=source,
                layer=layer,
                identity=identity,
            )
            .on_conflict_do_nothing(index_elements=["id"])
        )
        return dataset_id

    def publish_pair(
        self,
        conn,
        *,
        job_id,
        type_id,
        source,
        scope,
        raw_path,
        raw_format,
        standard_path,
        row_count,
        detail,
        snapshot_id=None,
        plugin_version=None,
        standard=None,
    ):
        """Called in the publisher's fenced transaction, after artifact writes succeed."""
        definition = self.types.get(type_id).manifest.model_dump()
        standard = standard or {}
        inputs = []
        ids = []
        for layer, path, format_ in [
            ("RAW", raw_path, raw_format),
            ("STANDARD", standard_path, standard.get("format", "parquet")),
        ]:
            override = standard if layer == "STANDARD" else {}
            dataset_id = self.ensure_collection(
                conn, type_id, source, scope, layer, override.get("series")
            )
            version_id = stable_id({"dataset_id": dataset_id, "job_id": job_id})
            content = (self.root / path).read_bytes()
            manifest = dict(
                type=definition
                | ({"frequency": scope["frequency"]} if scope.get("frequency") else {}),
                source=source,
                layer=layer,
                scope=scope,
                format=format_,
                checksum=hashlib.sha256(content).hexdigest(),
                bytes=len(content),
                path=path,
                inputs=list(inputs) + override.get("parent_inputs", []),
                state="PUBLISHED",
                quality="CAPTURED" if layer == "RAW" else "VALIDATED",
                snapshot_id=snapshot_id if layer == "STANDARD" else None,
                transform=None
                if layer == "RAW"
                else {
                    "id": "normalize:" + source,
                    "version": plugin_version or "1",
                    "plugin_digest": detail.get("plugin_digest"),
                    "output_schema": definition["schema_version"],
                },
                version_semantics=override.get("semantics", "ACQUISITION_SCOPE"),
                **(detail | override.get("detail", {})),
            )
            conn.execute(
                versions.insert().values(
                    id=version_id,
                    dataset_id=dataset_id,
                    job_id=job_id,
                    created_at=override.get("created_at", time.time()),
                    rows=override.get("rows", row_count),
                    manifest=manifest,
                )
            )
            inputs = [version_id]
            ids.append(version_id)
        return ids

    def list(
        self,
        *,
        domain="",
        type_id="",
        source="",
        layer="",
        search="",
        offset=0,
        limit=50,
        include_archived=False,
    ):
        conditions = []
        for column, value in [
            (collections.c.domain, domain),
            (collections.c.type_id, type_id),
            (collections.c.source, source),
            (collections.c.layer, layer),
        ]:
            if value:
                if column is collections.c.source and value.startswith("c_"):
                    conditions.append(
                        collections.c.identity["scope"]["connection_id"].as_string() == value
                    )
                else:
                    conditions.append(column == value)
        if search:
            # Search only declared catalogue identities, never secrets or raw payloads.
            from sqlalchemy import cast

            conditions.append(
                cast(collections.c.identity, String).contains(search, autoescape=True)
            )
        ranked = (
            select(
                versions,
                func.coalesce(version_states.c.archived, False).label("archived"),
                func.row_number()
                .over(
                    partition_by=versions.c.dataset_id,
                    order_by=(versions.c.created_at.desc(), versions.c.id.desc()),
                )
                .label("rank"),
                func.count().over(partition_by=versions.c.dataset_id).label("version_count"),
            )
            .outerjoin(version_states, version_states.c.version_id == versions.c.id)
            .subquery()
        )
        if not include_archived:
            conditions.append(ranked.c.archived.is_(False))
        query = (
            select(ranked)
            .join(collections, collections.c.id == ranked.c.dataset_id)
            .where(ranked.c.rank == 1, *conditions)
        )
        with self.engine.connect() as conn:
            total = conn.execute(select(func.count()).select_from(query.subquery())).scalar_one()
            rows = conn.execute(
                query.order_by(ranked.c.created_at.desc(), ranked.c.id).offset(offset).limit(limit)
            ).mappings()
            return {"items": [self.public(dict(r)) for r in rows], "total": total, "offset": offset}

    @staticmethod
    def public(record):
        manifest = dict(record["manifest"])
        manifest.pop("path", None)
        return dict(record) | {"manifest": manifest}

    def history(self, dataset_id, offset=0, limit=50):
        with self.engine.connect() as conn:
            query = (
                select(versions, func.coalesce(version_states.c.archived, False).label("archived"))
                .outerjoin(version_states, version_states.c.version_id == versions.c.id)
                .where(versions.c.dataset_id == dataset_id)
            )
            total = conn.execute(select(func.count()).select_from(query.subquery())).scalar_one()
            rows = conn.execute(
                query.order_by(versions.c.created_at.desc(), versions.c.id)
                .offset(offset)
                .limit(limit)
            ).mappings()
            return {"items": [self.public(dict(r)) for r in rows], "total": total, "offset": offset}

    def preview(self, version_id, offset=0, limit=100):
        with self.engine.connect() as conn:
            record = (
                conn.execute(select(versions).where(versions.c.id == version_id)).mappings().first()
            )
            if record is None:
                raise KeyError(version_id)
            manifest = record["manifest"]
            chart = None
            if manifest["snapshot_id"]:
                found = (
                    conn.execute(select(snapshots).where(snapshots.c.id == manifest["snapshot_id"]))
                    .mappings()
                    .first()
                )
                chart = dict(found) if found else None
        try:
            content = (self.root / manifest["path"]).read_bytes()
        except FileNotFoundError:
            raise ProviderError("数据文件缺失") from None
        if hashlib.sha256(content).hexdigest() != manifest["checksum"]:
            raise ProviderError("数据文件校验和不一致")
        provenance = None
        if manifest["format"] == "partition_manifest":
            from asterion.data.partitions import read_partition

            selected, cursor = [], 0
            for part in json.loads(content)["partitions"]:
                if cursor < offset + limit and cursor + part["rows"] > offset:
                    values = read_partition(self.root, part)
                    selected.extend(values[max(0, offset - cursor) : offset + limit - cursor])
                cursor += part["rows"]
            provenance = [
                {"observed_at": row["_observed_at"], "raw_version_id": row["_raw_version_id"]}
                for row in selected
            ]
            rows = [{k: v for k, v in row.items() if not k.startswith("_")} for row in selected]
        elif manifest["format"] == "parquet":
            rows = (
                pq.ParquetFile(pa.BufferReader(content))
                .read(use_threads=False)
                .slice(offset, limit)
                .to_pylist()
            )
        elif manifest["format"] == "provider_evidence":
            rows = [r for part in json.loads(content) for r in part["rows"]][
                offset : offset + limit
            ]
        elif manifest["format"] == "csv":
            rows = list(csv.DictReader(io.StringIO(content.decode())))[offset : offset + limit]
        else:
            raise ProviderError("该数据格式尚未安装预览器")
        return {
            "version": self.public(dict(record)),
            "rows": rows,
            "offset": offset,
            "total": record["rows"],
            "snapshot": chart,
            "row_sources": provenance,
        }
