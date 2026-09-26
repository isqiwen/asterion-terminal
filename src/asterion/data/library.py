"""Unified catalogue of immutable acquisition and cumulative versions."""

import json
import time
from pathlib import Path
from uuid import NAMESPACE_URL, uuid5

from asterion_bindings.artifacts import ArtifactRef, ArtifactStore
from asterion_bindings.data_catalog import (
    catalog_hierarchy,
    catalog_history,
    catalog_list,
    version_preview,
)
from sqlalchemy import JSON, Column, Float, ForeignKey, Integer, String, Table
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert

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


def read_artifact(store: ArtifactStore, name, sha256, size, *, missing, changed) -> bytes:
    """Verified read of a recorded artifact, reported in the owner's terms."""
    try:
        return store.read(name, sha256, size)
    except FileNotFoundError:
        raise ProviderError(missing) from None
    except ValueError:
        raise ProviderError(changed) from None


class DataLibrary:
    def __init__(self, engine, root: Path):
        self.engine, self.root = engine, root
        self.artifacts = ArtifactStore(root)
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
        raw: ArtifactRef,
        raw_format,
        standard: ArtifactRef,
        row_count,
        detail,
        snapshot_id=None,
        plugin_version=None,
        cumulative=None,
    ):
        """Called in the publisher's fenced transaction with its written artifacts."""
        definition = self.types.get(type_id).manifest.model_dump()
        cumulative = cumulative or {}
        inputs = []
        ids = []
        for layer, artifact, format_ in [
            ("RAW", raw, raw_format),
            ("STANDARD", standard, cumulative.get("format", "parquet")),
        ]:
            override = cumulative if layer == "STANDARD" else {}
            dataset_id = self.ensure_collection(
                conn, type_id, source, scope, layer, override.get("series")
            )
            version_id = stable_id({"dataset_id": dataset_id, "job_id": job_id})
            manifest = dict(
                type=definition
                | ({"frequency": scope["frequency"]} if scope.get("frequency") else {}),
                source=source,
                layer=layer,
                scope=scope,
                format=format_,
                checksum=artifact.sha256,
                bytes=artifact.bytes,
                path=artifact.name,
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
        directory="",
    ):
        """Latest version of each collection; the Rust data service's catalogue query."""
        query = {
            "directory": directory,
            "include_archived": include_archived,
            "domain": domain,
            "type_id": type_id,
            "source": source,
            "layer": layer,
            "search": search,
            "offset": offset,
            "limit": limit,
        }
        with self.engine.connect() as transaction:
            return catalog_list(transaction, query)

    def hierarchy(self, include_archived=False):
        with self.engine.connect() as transaction:
            return catalog_hierarchy(transaction, include_archived)

    @staticmethod
    def public(record):
        manifest = dict(record["manifest"])
        manifest.pop("path", None)
        return dict(record) | {"manifest": manifest}

    def history(self, dataset_id, offset=0, limit=50):
        with self.engine.connect() as transaction:
            return catalog_history(transaction, dataset_id, offset, limit)

    def preview(self, version_id, offset=0, limit=100):
        with self.engine.connect() as transaction:
            return self._preview(transaction, version_id, offset, limit)

    def preview_in(self, transaction, version_id, offset=0, limit=100):
        with self.engine.borrow(transaction) as borrowed:
            return self._preview(borrowed, version_id, offset, limit)

    def _preview(self, transaction, version_id, offset, limit):
        """Rows of a fixed version, read by the Rust data service."""
        try:
            return version_preview(transaction, self.root, version_id, offset, limit)
        except KeyError:
            raise
        except ValueError as error:
            raise ProviderError(str(error)) from None
