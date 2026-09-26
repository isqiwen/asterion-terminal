"""Data interface v1. CSV imports are immutable; invalid input fails atomically."""

from collections.abc import Callable
from dataclasses import dataclass
from datetime import date
from typing import Literal

import pyarrow as pa
import pyarrow.parquet as pq
from asterion_bindings.catalog import Contract as Contract  # noqa: PLC0414
from asterion_bindings.catalog import ReferenceCatalog as ReferenceCatalog  # noqa: PLC0414
from asterion_bindings.catalog import ResolutionRequest as ResolutionRequest  # noqa: PLC0414
from asterion_bindings.catalog import SourceIdentity as SourceIdentity  # noqa: PLC0414
from asterion_bindings.catalog import validate_market_code as validate_market_code  # noqa: PLC0414
from asterion_bindings.data_sources import SourceCredentials
from asterion_bindings.plugin_host import Capability
from asterion_bindings.resource import Resource
from pydantic import BaseModel, ConfigDict, Field, model_validator

from asterion.data.scan_public import ScanRequest, ScanResult


@dataclass(frozen=True)
class VersionAccess:
    """Read operations only; no catalogue mutation or provider credentials."""

    read: Callable
    coverage: Callable
    scan: Callable[[ScanRequest], ScanResult]


VERSION_ACCESS = Capability("data.versions", "asterion.data", VersionAccess)


class VersionReader:
    """Read-only cross-module access to checksum-verified immutable versions.

    The reader exposes public version metadata and row provenance, not catalogue
    tables, mutable provider state, or publication operations.
    """

    def __init__(self, engine, root):
        from asterion.data.library import DataLibrary

        self._library = DataLibrary(engine, root)

    def read(self, version_id: str, *, limit: int):
        return self._library.preview(version_id, limit=limit)

    def scan(self, request):
        from asterion.data.scanning import scan

        return scan(self._library, request)

    def coverage(self, report_id: str):
        """Return a stored, validated report without resolving newer reference versions."""
        from sqlalchemy import select

        from asterion.data.coverage import CoverageReport, reports

        with self._library.engine.connect() as conn:
            value = conn.execute(
                select(reports.c.report).where(reports.c.id == report_id)
            ).scalar_one_or_none()
        if value is None:
            raise KeyError(report_id)
        return CoverageReport.model_validate(value).model_dump(mode="json", exclude_unset=True)


def normalize_coverage_report(value):
    """Validate portable evidence structure without certifying its external origin."""
    from asterion.data.coverage import CoverageReport

    return CoverageReport.model_validate(value).model_dump(mode="json", exclude_unset=True)


class Manifest(BaseModel):
    storage: Literal["parquet", "version"]
    version_id: str | None = None
    demo: bool = False
    frequency: str | None = None
    time_semantics: str | None = None
    dataset_id: str | None = None
    schema_version: int
    rows: int
    checksum: str
    contracts: list[str]
    start: str
    end: str
    uri: str
    source: str
    state: Literal["PUBLISHED"]

    @model_validator(mode="after")
    def fixed_storage(self):
        if self.storage == "version" and not self.version_id:
            raise ValueError("图表投影必须固定数据版本")
        if self.storage == "parquet" and self.version_id is not None:
            raise ValueError("独立图表文件不能声明版本投影")
        return self


class Snapshot(BaseModel):
    id: str
    job_id: str
    manifest: Manifest


CREDENTIALS = Resource("data.credentials", SourceCredentials)


def coverage_contracts(value: dict, days: list[date]) -> list[dict]:
    """Resolve fixed portable coverage evidence; never query latest catalogues."""
    from asterion.data.coverage import CoverageReport

    report = CoverageReport.model_validate(value)
    if report.identity is None:
        raise ValueError("研究需要可验证的固定合约身份目录")
    return [report.identity.resolve(day).model_dump(mode="json") for day in days]


def source_contract(source: dict, contract_id: str):
    """Resolve one canonical lifecycle from verified standard source observations."""
    from asterion_bindings import data_identity
    from asterion_bindings.catalog import Contract

    contract, row = data_identity.source_contract(source, contract_id)
    return Contract.model_validate(contract), row


def source_contract_catalog(reader, version_id: str, symbols: list[str]) -> ReferenceCatalog:
    """Build immutable catalogue evidence through the data owner's current source contract."""
    from asterion.data.reference_source import SourceCatalogRequest, source_catalog

    return source_catalog(reader, SourceCatalogRequest(version_id=version_id, symbols=symbols))


def snapshot_backup_access(conn, artifacts) -> VersionAccess:
    """Freeze metadata; verify standard snapshots and every cumulative partition read.

    ``artifacts`` is a read-only artifact grant over the backed-up data root.
    """
    from sqlalchemy import select

    from asterion.data.library import read_artifact, versions
    from asterion.data.partitions import read_partition, version_partitions

    records = {r["id"]: dict(r) for r in conn.execute(select(versions)).mappings()}

    def read(identifier, *, limit):
        record = records[identifier]
        manifest = record["manifest"]
        if manifest.get("layer") != "STANDARD" or manifest.get("format") not in {
            "parquet",
            "partition_manifest",
        }:
            raise ValueError("恢复依据必须是标准表格快照")
        if manifest["format"] == "partition_manifest":
            rows = [
                row
                for part in version_partitions(artifacts, manifest)
                for row in read_partition(artifacts, part)
            ]
            if len(rows) != record["rows"]:
                raise ValueError("恢复依据行数不一致")
            selected = rows[:limit]
            return {
                "version": record,
                "total": record["rows"],
                "row_sources": [
                    {"observed_at": r["_observed_at"], "raw_version_id": r["_raw_version_id"]}
                    for r in selected
                ],
                "rows": [{k: v for k, v in r.items() if not k.startswith("_")} for r in selected],
            }
        content = read_artifact(
            artifacts,
            manifest["path"],
            manifest["checksum"],
            manifest["bytes"],
            missing="恢复依据文件缺失",
            changed="恢复依据文件校验和不一致",
        )
        table = pq.ParquetFile(pa.BufferReader(content)).read(use_threads=False)
        if table.num_rows != record["rows"]:
            raise ValueError("恢复依据行数不一致")
        return {
            "version": record,
            "total": record["rows"],
            "rows": table.slice(0, limit).to_pylist(),
        }

    def no_coverage(_):
        raise ValueError("快照恢复端口不提供覆盖报告")

    def no_scan(_):
        raise ValueError("备份证据端口不提供在线批次扫描")

    return VersionAccess(read, no_coverage, no_scan)


def fixed_daily_evidence(reader, version_id: str):
    """Read validated daily quantities and conservative local availability from fixed sources."""
    from asterion.data.daily_evidence import fixed_daily_evidence as read

    return read(reader, version_id)


def source_product_catalog(reader, version_id: str, product_id: str) -> ReferenceCatalog:
    """Resolve all identities of a product from a complete fixed source observation."""
    from asterion.data.reference_source import product_catalog

    return product_catalog(reader, version_id, product_id)


@dataclass(frozen=True)
class SyncAccess:
    """Transaction-scoped, credential-free sync dependencies and verified version reads."""

    inspect: Callable
    versions: Callable


SYNC_ACCESS = Capability("data.sync_dependencies", "asterion.data", SyncAccess)


@dataclass(frozen=True)
class SyncBatchAccess:
    submit: Callable


SYNC_BATCH_ACCESS = Capability("data.daily_sync_batch", "asterion.data", SyncBatchAccess)


class DailySyncBatch(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    command_prefix: str = Field(min_length=1, max_length=80)
    provider: str
    connection_id: str | None
    exchange: str
    contracts_version_id: str
    trading_day: date
    symbols: tuple[str, ...] = Field(min_length=1, max_length=1000)

    @model_validator(mode="after")
    def unique(self):
        if len(set(self.symbols)) != len(self.symbols):
            raise ValueError("批量日线候选重复")
        return self
