"""Fixed-input daily coverage reports and transactional, user-triggered refills."""

import time
from collections import Counter
from datetime import date, datetime, timedelta
from typing import Literal
from zoneinfo import ZoneInfo

from asterion_bindings.catalog import SourceIdentity, catalog_digest
from asterion_bindings.data_partitions import cumulative_series
from asterion_bindings.task_models import Job
from asterion_bindings.task_repository import Conflict
from pydantic import BaseModel, ConfigDict, Field, model_validator
from sqlalchemy import JSON, Column, Float, String, Table, func, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert
from sqlalchemy.exc import IntegrityError

from asterion.data.library import collections, stable_id, versions
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.reference_source import SourceCatalogRequest, source_catalog
from asterion.platform.store import jobs, metadata

CHECKER = "daily-coverage-v2"
reports = Table(
    "data_coverage_reports",
    metadata,
    Column("id", String, primary_key=True),
    Column("daily_version_id", String, nullable=False, index=True),
    Column("created_at", Float, nullable=False),
    Column("report", JSON, nullable=False),
)
refills = Table(
    "data_coverage_refills",
    metadata,
    Column("command_id", String, primary_key=True),
    Column("report_id", String, nullable=False),
    Column("result", JSON),
)


def today():
    return datetime.now(ZoneInfo("Asia/Shanghai")).date()


class CoverageRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    start: date
    end: date
    calendar_version_id: str | None = Field(default=None, min_length=1, max_length=100)
    contracts_version_id: str | None = Field(default=None, min_length=1, max_length=100)
    use_latest_daily: bool = False
    reference_policy: Literal["same_source", "explicit_external"] = "same_source"
    reference_note: str = Field(default="", max_length=500)
    reference_symbol: str | None = Field(default=None, min_length=1, max_length=64)

    @model_validator(mode="after")
    def bounded_dates(self):
        if self.start > self.end or (self.end - self.start).days > 3660 or self.end > today():
            raise ValueError("核对区间须有序、不超过十年，且不能晚于今天")
        if self.reference_policy == "explicit_external" and (
            not self.calendar_version_id
            or not self.contracts_version_id
            or not self.reference_note.strip()
            or not self.reference_symbol
        ):
            raise ValueError("外部依据必须明确指定日历、合约版本、来源代码并填写关联说明")
        if self.reference_policy == "same_source" and self.reference_symbol is not None:
            raise ValueError("同源核对使用行情原始来源代码，不允许覆盖")
        return self


class CoverageDay(BaseModel):
    date: date
    status: Literal[
        "PRESENT",
        "GAP",
        "CLOSED",
        "OUTSIDE_LISTING",
        "UNKNOWN_CONTRACT",
        "UNKNOWN_CALENDAR",
        "PENDING",
        "CONFLICT",
    ]
    has_data: bool
    reason: str


class DateRange(BaseModel):
    start: date
    end: date


class CoverageReport(BaseModel):
    identity: SourceIdentity | None
    reference_symbol: str | None
    id: str
    checker: Literal["daily-coverage-v2"]
    created_at: float
    as_of: date
    daily_version_id: str
    calendar_version_id: str | None
    contracts_version_id: str | None
    source: str
    connection_id: str | None
    exchange: str
    symbol: str
    start: date
    end: date
    status: Literal["COVERED", "GAPS", "UNCONFIRMED", "CONFLICT", "NO_EXPECTED_ROWS"]
    counts: dict[str, int]
    days: list[CoverageDay]
    refill_ranges: list[DateRange]
    notes: list[str]
    reference_policy: Literal["same_source", "explicit_external"]
    reference_note: str
    references: dict
    refill_supported: bool

    @model_validator(mode="after")
    def identity_consistency(self):
        if self.identity is None:
            if self.status in {"COVERED", "GAPS"}:
                raise ValueError("覆盖通过或可补取报告必须包含合约身份目录")
            return self
        evidence = self.identity
        if len(evidence.catalog.inputs) != 1:
            raise ValueError("覆盖目录必须固定唯一合约资料输入")
        basis = evidence.catalog.inputs[0]
        expected_symbol = (
            self.reference_symbol if self.reference_policy == "explicit_external" else self.symbol
        )
        if (
            basis.version_id != self.contracts_version_id
            or basis.source != evidence.source
            or evidence.symbol != expected_symbol
        ):
            raise ValueError("覆盖报告与目录的输入版本或来源代码不一致")
        for day in self.days:
            if day.status in {"PRESENT", "GAP", "CLOSED", "PENDING"}:
                actual = evidence.resolve(day.date)
                if actual.product_id.split(".")[0] != self.exchange:
                    raise ValueError("覆盖目录与行情交易所不一致")
        return self


class RefillRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    command_id: str = Field(min_length=1, max_length=100)


class RefillResult(BaseModel):
    status: Literal["QUEUED", "NO_GAPS", "BLOCKED"]
    report_id: str
    checked_report_id: str
    jobs: list[Job]
    skipped_gap_days: int


class RefillTracking(BaseModel):
    report_id: str
    submitted: bool
    statuses: list[str]
    jobs: list[Job]
    total: int
    truncated: bool


def gap_ranges(days):
    """Bridge only confirmed closed days, never present or uncertain dates."""
    ranges, start, end = [], None, None
    for item in days:
        if item["status"] == "GAP":
            start = start or item["date"]
            end = item["date"]
        elif item["status"] != "CLOSED" and start:
            ranges.append({"start": start, "end": end})
            start, end = None, None
    if start:
        ranges.append({"start": start, "end": end})
    return ranges


class DailyCoverage:
    def __init__(self, sync):
        self.sync, self.engine, self.library = sync, sync.engine, sync.library
        self.engine.initialize(reports)
        self.engine.initialize(refills)

    def _record(self, conn, version_id, expected_type):
        record = (
            conn.execute(select(versions).where(versions.c.id == version_id)).mappings().first()
        )
        if record is None:
            raise KeyError(version_id)
        manifest = record["manifest"]
        if (
            manifest["layer"] != "STANDARD"
            or manifest["type"]["id"] != expected_type
            or manifest["state"] != "PUBLISHED"
        ):
            raise ProviderError("核对输入必须是对应类型的已发布标准版本")
        if (
            manifest["type"]["schema_version"]
            != self.library.types.get(expected_type).manifest.schema_version
        ):
            raise ProviderError("核对输入结构版本不兼容")
        return record

    def _latest(self, conn, dataset_id):
        return conn.execute(
            select(versions.c.id)
            .where(versions.c.dataset_id == dataset_id)
            .order_by(versions.c.created_at.desc(), versions.c.id.desc())
            .limit(1)
        ).scalar_one_or_none()

    def _reference(self, conn, daily, type_id, selected, *, resolve=True, external=False):
        source, exchange = daily["manifest"]["source"], daily["manifest"]["scope"]["exchange"]
        if not selected and resolve and not external:
            cumulative = cumulative_series(type_id) if type_id == "futures.calendar" else None
            for series in [cumulative, None] if cumulative else [None]:
                dataset_id = stable_id(
                    self.library.identity(
                        type_id,
                        source,
                        {"exchange": exchange, "symbol": ""}
                        | (
                            {"connection_id": daily["manifest"]["scope"]["connection_id"]}
                            if daily["manifest"]["scope"].get("connection_id")
                            else {}
                        ),
                        "STANDARD",
                        series,
                    )
                )
                selected = self._latest(conn, dataset_id)
                if selected:
                    break
        if not selected:
            return None, []
        record = self._record(conn, selected, type_id)
        manifest = record["manifest"]
        if manifest["scope"].get("exchange") != exchange or (
            not external
            and (
                manifest["source"] != source
                or manifest["scope"].get("connection_id")
                != daily["manifest"]["scope"].get("connection_id")
            )
        ):
            raise ProviderError("核对依据的来源及交易所必须与日线一致")
        rows = self.library.preview_in(conn, record["id"], limit=record["rows"])["rows"]
        self.library.types.get(type_id).validate(rows)
        if any(row["exchange"] != exchange for row in rows):
            raise ProviderError("核对依据包含其他交易所的数据")
        return record["id"], rows

    def _evaluate(self, conn, version_id, request, *, resolve=True):
        daily = self._record(conn, version_id, "futures.daily")
        if request.use_latest_daily:
            daily = self._record(conn, self._latest(conn, daily["dataset_id"]), "futures.daily")
        manifest = daily["manifest"]
        local = manifest["source"] == "local_file"
        admitted_source = (
            None if local else SourceIdentity.model_validate(manifest.get("contract_identity"))
        )
        external = request.reference_policy == "explicit_external"
        if local and not external:
            raise ProviderError("本地日线必须显式关联日历和合约版本")
        if external and not local:
            raise ProviderError("外部依据关联仅用于本地导入日线")
        if not local and manifest["version_semantics"] != "CUMULATIVE":
            raise ProviderError("请先将日线同步到累积版本，再核对覆盖")
        if local and request.use_latest_daily:
            raise ProviderError("本地导入需选择固定版本重新核对")
        calendar_id, calendar_rows = self._reference(
            conn,
            daily,
            "futures.calendar",
            request.calendar_version_id,
            resolve=resolve,
            external=external,
        )
        contracts_id, contracts = self._reference(
            conn,
            daily,
            "futures.contracts",
            request.contracts_version_id
            or (
                admitted_source.catalog.inputs[0].version_id
                if admitted_source is not None
                else None
            ),
            resolve=resolve,
            external=external,
        )
        exchange, symbol = manifest["scope"]["exchange"], manifest["scope"]["symbol"]
        reference_symbol = request.reference_symbol if local else symbol
        matches = [row for row in contracts if row["symbol"] == reference_symbol]
        identity_evidence = None
        if (
            contracts_id is not None
            and matches
            and all(
                row["delivery_month"] is not None and row["delisted"] is not None for row in matches
            )
        ):
            catalog = source_catalog(
                lambda version_id, **options: self.library.preview_in(conn, version_id, **options),
                SourceCatalogRequest(
                    version_id=contracts_id,
                    symbols=[reference_symbol],
                ),
            )
            identity_evidence = SourceIdentity(
                catalog_id=catalog_digest(catalog),
                catalog=catalog,
                source=catalog.inputs[0].source,
                symbol=reference_symbol,
                information_at=max(c.provenance.available_at for c in catalog.contracts),
            )
        calendar = {row["date"]: row["is_open"] for row in calendar_rows}
        bars = self.library.preview_in(conn, daily["id"], limit=daily["rows"])["rows"]
        self.library.types.get("futures.daily").validate(bars)
        if any(row["symbol"] != symbol or row["exchange"] != exchange for row in bars):
            raise ProviderError("日线必须与所选单合约来源代码一致")
        if local:
            from asterion_bindings.catalog import ImportIdentity

            admitted = ImportIdentity.model_validate(manifest["import_options"]["identity"])
            admitted.validate_rows(bars)
            if identity_evidence is not None:
                for row in bars:
                    day = date.fromisoformat(row["trading_day"])
                    if (
                        admitted.resolve(row["contract"], day).id
                        != identity_evidence.resolve(day).id
                    ):
                        raise ProviderError("覆盖依据与导入时固定的实际合约身份不一致")
        if admitted_source is not None:
            admitted_source.validate_rows(bars, exchange)
            if identity_evidence is not None:
                for row in bars:
                    day = date.fromisoformat(row["trading_day"])
                    if admitted_source.resolve(day).id != identity_evidence.resolve(day).id:
                        raise ProviderError("覆盖依据与同步发布时固定的实际身份不一致")
        present = {row["trading_day"] for row in bars}
        as_of = today()
        days = []
        cursor = request.start
        while cursor <= request.end:
            day = cursor.isoformat()
            has_data = day in present
            actual = None
            if identity_evidence is not None:
                try:
                    actual = identity_evidence.resolve(cursor)
                except ValueError:
                    # A validated catalogue with all evidence available has no mapping outside its lifecycles.
                    pass
            if identity_evidence is None:
                status, reason = "UNKNOWN_CONTRACT", "缺少可验证的合约身份目录或完整生命周期"
            elif actual is None:
                status, reason = "OUTSIDE_LISTING", "固定目录的合约生命周期外"
            elif day not in calendar:
                status, reason = "UNKNOWN_CALENDAR", "所选交易日历缺少该日期"
            elif calendar[day] == 0:
                status, reason = "CLOSED", "所选交易日历标记休市"
            elif cursor >= as_of:
                status, reason = "PENDING", "当日日线尚不按完整历史数据核对"
            else:
                status, reason = (
                    ("PRESENT", "预期交易日已有记录")
                    if has_data
                    else ("GAP", "预期交易日尚无记录，需补取确认")
                )
            if has_data and status in {"CLOSED", "OUTSIDE_LISTING"}:
                status, reason = "CONFLICT", reason + "，但日线已有记录"
            days.append({"date": day, "status": status, "has_data": has_data, "reason": reason})
            cursor += timedelta(days=1)
        counts = dict(Counter(item["status"] for item in days))
        status = "COVERED" if counts.get("PRESENT") else "NO_EXPECTED_ROWS"
        if counts.get("GAP"):
            status = "GAPS"
        if any(counts.get(key) for key in ("UNKNOWN_CONTRACT", "UNKNOWN_CALENDAR", "PENDING")):
            status = "UNCONFIRMED"
        if counts.get("CONFLICT"):
            status = "CONFLICT"
        identity = {
            "checker": CHECKER,
            "daily_version_id": daily["id"],
            "calendar_version_id": calendar_id,
            "contracts_version_id": contracts_id,
            "start": request.start.isoformat(),
            "end": request.end.isoformat(),
            "as_of": as_of.isoformat(),
            "identity": identity_evidence.model_dump(mode="json") if identity_evidence else None,
            "reference_symbol": request.reference_symbol,
        }
        references = {}
        if external:
            for key, ident, type_id in (
                ("calendar", calendar_id, "futures.calendar"),
                ("contracts", contracts_id, "futures.contracts"),
            ):
                ref = self._record(conn, ident, type_id)
                references[key] = {
                    "version_id": ident,
                    "source": ref["manifest"]["source"],
                    "checksum": ref["manifest"]["checksum"],
                    "connection_id": ref["manifest"]["scope"].get("connection_id"),
                }
            identity.update(
                reference_policy=request.reference_policy,
                reference_note=request.reference_note,
                references=references,
            )
        report = identity | {
            "reference_policy": request.reference_policy,
            "reference_note": request.reference_note,
            "references": references,
            "refill_supported": not local,
            "id": stable_id(identity),
            "created_at": time.time(),
            "source": manifest["source"],
            "connection_id": manifest["scope"].get("connection_id"),
            "exchange": exchange,
            "symbol": symbol,
            "status": status,
            "counts": counts,
            "days": days,
            "refill_ranges": [] if status == "CONFLICT" else gap_ranges(days),
            "notes": [
                "结论仅适用于列出的行情、交易日历及合约资料版本。",
                "缺失记录不证明无成交；空响应不会消除缺口。仅补取有完整依据的历史缺口。",
            ],
        }
        if local:
            report.update(refill_supported=False, refill_ranges=[])
            report["notes"].append(
                "本地文件按用户指定的外部依据核对，不认证来源；缺失日期需修正文件、导入新版本并重新核对，不自动跨源补齐。"
            )
        report = CoverageReport.model_validate(report).model_dump(mode="json")
        insert = pg_insert if conn.dialect.name == "postgresql" else sqlite_insert
        conn.execute(
            insert(reports)
            .values(
                id=report["id"],
                daily_version_id=daily["id"],
                created_at=report["created_at"],
                report=report,
            )
            .on_conflict_do_nothing(index_elements=["id"])
        )
        return conn.execute(
            select(reports.c.report).where(reports.c.id == report["id"])
        ).scalar_one()

    def check(self, version_id, request: CoverageRequest):
        with self.engine.begin() as conn:
            return self._evaluate(conn, version_id, request)

    def get(self, report_id):
        with self.engine.connect() as conn:
            report = conn.execute(
                select(reports.c.report).where(reports.c.id == report_id)
            ).scalar_one_or_none()
        if report is None:
            raise KeyError(report_id)
        return CoverageReport.model_validate(report).model_dump(mode="json")

    def latest(self, version_id):
        with self.engine.connect() as conn:
            self._record(conn, version_id, "futures.daily")
            report = conn.execute(
                select(reports.c.report)
                .where(reports.c.daily_version_id == version_id)
                .order_by(reports.c.created_at.desc(), reports.c.id.desc())
                .limit(1)
            ).scalar_one_or_none()

        return CoverageReport.model_validate(report).model_dump(mode="json") if report else None

    def tracking(self, report_id):
        """Read durable submissions and current leaf attempts, never resubmit on reconnect."""
        self.get(report_id)
        with self.engine.connect() as conn:
            batches = (
                conn.execute(select(refills.c.result).where(refills.c.report_id == report_id))
                .scalars()
                .all()
            )
            batches = [batch for batch in batches if batch is not None]
            checked = {batch["checked_report_id"] for batch in batches}
            # Also accept the checked report opened directly from a task's provenance.
            checked.add(report_id)
            condition = (jobs.c.kind == "data.sync") & (
                jobs.c.payload["coverage_report_id"].as_string().in_(checked)
            )
            records = (
                conn.execute(
                    select(jobs, func.count().over().label("tracking_total"))
                    .where(condition)
                    .order_by(jobs.c.created_at.desc(), jobs.c.id)
                    .limit(200)
                )
                .mappings()
                .all()
            )
        total = records[0]["tracking_total"] if records else 0
        parents = {row["payload"].get("retry_of") for row in records}
        return RefillTracking(
            report_id=report_id,
            submitted=bool(batches or total),
            statuses=sorted({batch["status"] for batch in batches}),
            jobs=[Job.model_validate(row) for row in reversed(records) if row["id"] not in parents],
            total=total,
            truncated=total > len(records),
        )

    def refill(self, report_id, command_id):
        try:
            with self.engine.begin() as conn:
                original = conn.execute(
                    select(reports.c.report).where(reports.c.id == report_id)
                ).scalar_one_or_none()
                if original is None:
                    raise KeyError(report_id)
                original = CoverageReport.model_validate(original).model_dump(mode="json")
                if original["refill_supported"] is False or original["source"] == "local_file":
                    raise ProviderError("本地导入数据不支持自动补齐，请修正文件后导入新版本")
                insert = pg_insert if conn.dialect.name == "postgresql" else sqlite_insert
                conn.execute(
                    insert(refills)
                    .values(command_id=command_id, report_id=report_id)
                    .on_conflict_do_nothing(index_elements=["command_id"])
                )
                prior = (
                    conn.execute(
                        select(refills).where(refills.c.command_id == command_id).with_for_update()
                    )
                    .mappings()
                    .one()
                )
                if prior["report_id"] != report_id:
                    raise Conflict("command_id reused with another coverage report")
                if prior["result"] is not None:
                    return prior["result"]
                daily = self._record(conn, original["daily_version_id"], "futures.daily")
                conn.execute(
                    select(collections.c.id)
                    .where(collections.c.id == daily["dataset_id"])
                    .with_for_update()
                ).one()
                request = CoverageRequest(
                    start=original["start"],
                    end=original["end"],
                    calendar_version_id=original["calendar_version_id"],
                    contracts_version_id=original["contracts_version_id"],
                    use_latest_daily=True,
                )
                checked = self._evaluate(conn, daily["id"], request, resolve=False)
                # Only dates that were gaps in the approved report may enter this refill.
                approved = {day["date"] for day in original["days"] if day["status"] == "GAP"}
                days = [
                    item
                    if item["date"] in approved or item["status"] == "CLOSED"
                    else item | {"status": "SKIP"}
                    for item in checked["days"]
                ]
                ranges = [] if checked["status"] == "CONFLICT" else gap_ranges(days)
                commands = []
                for index, interval in enumerate(ranges):
                    req = SyncRequest(
                        command_id=stable_id({"coverage_refill": command_id, "index": index}),
                        provider=checked["source"],
                        connection_id=checked.get("connection_id"),
                        dataset="daily",
                        exchange=checked["exchange"],
                        symbol=checked["symbol"],
                        **interval,
                    )
                    payload = self.sync.submission_payload_in(conn, req) | {
                        "contract_identity": daily["manifest"].get("contract_identity"),
                        "coverage_report_id": checked["id"],
                    }
                    from asterion.data.sync_identity import task_identity

                    if task_identity(payload) is not None:
                        self.sync.validate_identity(conn, payload)
                    commands.append((req.command_id, "data.sync", payload))
                queued = self.sync.tasks.submit_batch(conn, commands)
                result = RefillResult(
                    status="QUEUED"
                    if queued
                    else "BLOCKED"
                    if checked["status"] == "CONFLICT"
                    else "NO_GAPS",
                    report_id=report_id,
                    checked_report_id=checked["id"],
                    jobs=[Job.model_validate(job) for job in queued],
                    skipped_gap_days=len(
                        approved
                        - {item["date"] for item in checked["days"] if item["status"] == "GAP"}
                    ),
                ).model_dump(mode="json")
                conn.execute(
                    refills.update().where(refills.c.command_id == command_id).values(result=result)
                )
                return result
        except IntegrityError:
            raise Conflict("补齐命令与已存在的任务冲突，未提交新任务") from None
