"""Data interface v1. CSV imports are immutable; invalid input fails atomically."""

import csv
import hashlib
import io
import json
from collections.abc import Callable
from dataclasses import dataclass
from datetime import date, datetime
from decimal import Decimal
from typing import Annotated, Literal

import pyarrow as pa
import pyarrow.parquet as pq
from pydantic import AwareDatetime, BaseModel, ConfigDict, Field, model_validator

from asterion.data.import_identity import ImportIdentity
from asterion.data.reference import Contract as Contract  # noqa: PLC0414
from asterion.data.reference import ReferenceCatalog as ReferenceCatalog  # noqa: PLC0414
from asterion.data.reference import ResolutionRequest as ResolutionRequest  # noqa: PLC0414
from asterion.data.reference import SourceIdentity as SourceIdentity  # noqa: PLC0414
from asterion.data.reference import validate_market_code as validate_market_code  # noqa: PLC0414
from asterion.platform.plugins import Capability
from asterion.platform.resource import Resource
from asterion.platform.secrets import SecretPort, SecretScope


@dataclass(frozen=True)
class VersionAccess:
    """Read operations only; no catalogue mutation or provider credentials."""

    read: Callable
    coverage: Callable


VERSION_ACCESS = Capability("data.versions", "asterion.data", VersionAccess)


Price = Annotated[Decimal, Field(gt=0, max_digits=20, decimal_places=8)]


class Bar(BaseModel):
    contract: str = Field(
        pattern=r"^(?:(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Za-z]+[0-9]{3,4}|SIM\.DEMO001)$"
    )
    event_time: AwareDatetime
    available_at: AwareDatetime
    trading_day: date
    open: Price
    high: Price
    low: Price
    close: Price
    volume: int = Field(ge=0)

    @model_validator(mode="after")
    def check(self):
        if self.event_time.microsecond:
            raise ValueError("CSV bar v1 requires whole-second event_time")
        if self.low > min(self.open, self.close) or self.high < max(self.open, self.close):
            raise ValueError("OHLC price bounds are inconsistent")
        if self.available_at < self.event_time:
            raise ValueError("available_at must not precede event_time")
        return self


from asterion.trading_time.public import TimeVersion


class ImportOptions(BaseModel):
    identity: ImportIdentity
    trading_time: TimeVersion | None = None
    timestamp_semantics: Literal["bar_start", "bar_end"] | None = None
    model_config = ConfigDict(extra="forbid")
    type_id: Literal["futures.bars", "futures.daily"] = "futures.bars"
    frequency: Literal["unspecified", "1m", "5m", "15m", "30m", "1h", "1d"] = "unspecified"
    source_id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,40}$")
    column_mapping: dict[str, str] = Field(default_factory=dict, max_length=20)

    @model_validator(mode="after")
    def semantics(self):
        if self.type_id == "futures.bars" and (
            self.trading_time is None
            or self.timestamp_semantics is None
            or self.frequency == "unspecified"
        ):
            raise ValueError("日内行情必须选择交易时间版本、明确频率及时间戳起止口径")
        if self.type_id == "futures.daily" and self.frequency != "1d":
            raise ValueError("日线频率必须为 1d")
        if self.type_id == "futures.bars" and self.frequency == "1d":
            raise ValueError("日线请使用期货日线类型")
        return self


class ImportRequest(BaseModel):
    command_id: str = Field(min_length=1, max_length=100)
    source: str = Field(min_length=1, max_length=200)
    csv: str = Field(min_length=1, max_length=2_000_000)
    options: ImportOptions


def encode_csv(content: str) -> tuple[bytes, dict]:
    bars = [Bar.model_validate(row) for row in csv.DictReader(io.StringIO(content))]
    if not bars:
        raise ValueError("Source returned no rows; no snapshot published")
    keys = [(bar.contract, bar.event_time) for bar in bars]
    if len(keys) != len(set(keys)):
        raise ValueError("Duplicate contract/event_time keys")
    bars.sort(key=lambda b: (b.contract, b.event_time))
    schema = pa.schema(
        [
            ("contract", pa.string()),
            ("event_time", pa.timestamp("us", tz="UTC")),
            ("available_at", pa.timestamp("us", tz="UTC")),
            ("trading_day", pa.date32()),
            *[(name, pa.decimal128(20, 8)) for name in ("open", "high", "low", "close")],
            ("volume", pa.int64()),
        ]
    )
    table = pa.Table.from_pylist([b.model_dump() for b in bars], schema=schema)
    output = pa.BufferOutputStream()
    pq.write_table(table, output)
    data = output.getvalue().to_pybytes()
    return data, {
        "schema_version": 1,
        "demo": any(bar.contract == "SIM.DEMO001" for bar in bars),
        "rows": len(bars),
        "checksum": hashlib.sha256(data).hexdigest(),
        "contracts": sorted({b.contract for b in bars}),
        "start": min(b.event_time for b in bars).isoformat(),
        "end": max(b.event_time for b in bars).isoformat(),
    }


def read_bars(path, limit=1000):
    parquet = pq.ParquetFile(path)
    batch = next(parquet.iter_batches(batch_size=limit))
    rows = batch.to_pylist()
    return [
        {k: str(v) if isinstance(v, (Decimal, datetime, date)) else v for k, v in row.items()}
        for row in rows
    ]


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


class Snapshot(BaseModel):
    id: str
    job_id: str
    manifest: Manifest


CREDENTIAL_SCOPE = SecretScope(b"asterion.provider.credentials.v1", b"configuration-snapshot-v1")
CREDENTIALS = Resource("data.credentials", SecretPort)


@dataclass(frozen=True)
class SyncReporter:
    progress: Callable[[int, int], None]
    checkpoint: Callable[[int, dict], None]
    resume: Callable[[], dict]


SYNC_REPORTER = Resource("data.sync_reporter", SyncReporter)


def coverage_contracts(value: dict, days: list[date]) -> list[dict]:
    """Resolve fixed portable coverage evidence; never query latest catalogues."""
    from asterion.data.coverage import CoverageReport

    report = CoverageReport.model_validate(value)
    if report.identity is None:
        raise ValueError("研究需要可验证的固定合约身份目录")
    return [report.identity.resolve(day).model_dump(mode="json") for day in days]


def source_contract(source: dict, contract_id: str):
    """Resolve one canonical lifecycle from verified standard source observations."""
    from asterion.data.reference_source import SourceCatalogRequest, source_catalog
    from asterion.data.types import Contract as ContractRow

    matches = []
    for raw in source["rows"]:
        row = ContractRow.model_validate(raw)
        if row.delivery_month is None:
            continue
        identifier = f"{row.exchange}.{row.product}.{row.delivery_month.replace('-', '')}.{row.listed:%Y%m%d}"
        if identifier == contract_id:
            matches.append(raw)
    if len(matches) != 1:
        raise ValueError("资料中没有唯一匹配的规范合约身份")
    row = matches[0]
    catalog = source_catalog(
        lambda identifier, limit: source,
        SourceCatalogRequest(version_id=source["version"]["id"], symbols=[row["symbol"]]),
    )
    return next(item for item in catalog.contracts if item.id == contract_id), row


def source_contract_catalog(reader, version_id: str, symbols: list[str]) -> ReferenceCatalog:
    """Build immutable catalogue evidence through the data owner's current source contract."""
    from asterion.data.reference_source import SourceCatalogRequest, source_catalog

    return source_catalog(reader, SourceCatalogRequest(version_id=version_id, symbols=symbols))


def snapshot_backup_access(conn, files) -> VersionAccess:
    """Freeze metadata; verify standard snapshots and every cumulative partition read."""
    from sqlalchemy import select

    from asterion.data.library import versions

    records = {r["id"]: dict(r) for r in conn.execute(select(versions)).mappings()}

    def read(identifier, *, limit):
        record = records[identifier]
        manifest = record["manifest"]
        if manifest.get("layer") != "STANDARD" or manifest.get("format") not in {
            "parquet",
            "partition_manifest",
        }:
            raise ValueError("恢复依据必须是标准表格快照")
        content = files.read(manifest["path"])
        if hashlib.sha256(content).hexdigest() != manifest["checksum"]:
            raise ValueError("恢复依据文件校验和不一致")
        provenance = None
        if manifest["format"] == "partition_manifest":
            index = json.loads(content)
            if index.get("schema_version") != 1 or index["partitions"] != manifest["partitions"]:
                raise ValueError("恢复依据分区清单不一致")
            rows = []
            for part in index["partitions"]:
                data = files.read(f"artifacts/{part['checksum']}.parquet")
                if hashlib.sha256(data).hexdigest() != part["checksum"]:
                    raise ValueError("恢复依据分区校验和不一致")
                batch = pq.ParquetFile(pa.BufferReader(data)).read(use_threads=False)
                if batch.num_rows != part["rows"]:
                    raise ValueError("恢复依据分区行数不一致")
                rows.extend(batch.to_pylist())
            if len(rows) != record["rows"]:
                raise ValueError("恢复依据行数不一致")
            selected = rows[:limit]
            provenance = [
                {"observed_at": r["_observed_at"], "raw_version_id": r["_raw_version_id"]}
                for r in selected
            ]
            return {
                "version": record,
                "total": record["rows"],
                "row_sources": provenance,
                "rows": [{k: v for k, v in r.items() if not k.startswith("_")} for r in selected],
            }
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

    return VersionAccess(read, no_coverage)


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
