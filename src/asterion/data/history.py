"""Bounded multi-contract history plans using the existing sync task lifecycle."""

from calendar import monthrange
from collections import Counter
from datetime import date, timedelta
from uuid import UUID

from asterion_bindings.catalog import SourceIdentity, catalog_digest
from asterion_bindings.task_models import Job
from asterion_bindings.task_repository import Conflict
from pydantic import BaseModel, ConfigDict, Field, model_validator
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError

from asterion.data.coverage import CoverageReport, CoverageRequest, today
from asterion.data.library import versions
from asterion.data.providers.public import SyncRequest
from asterion.data.reference_source import SourceCatalogRequest, source_catalog
from asterion.platform.store import jobs


class HistoryRequest(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    command_id: UUID
    provider: str
    connection_id: str | None = None
    exchange: str
    contracts_version_id: str = Field(min_length=1, max_length=100)
    calendar_version_id: str = Field(min_length=1, max_length=100)
    symbols: tuple[str, ...] = Field(min_length=1, max_length=100)
    start: date
    end: date

    @model_validator(mode="after")
    def valid(self):
        if self.start > self.end or (self.end - self.start).days > 3660 or self.end > today():
            raise ValueError("历史计划区间须有序、不超过十年且不能晚于今天")
        if len(set(self.symbols)) != len(self.symbols):
            raise ValueError("历史计划合约重复")
        return self


class HistorySlice(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    contract_id: str
    request: SyncRequest
    identity: SourceIdentity
    expected_days: int = Field(ge=1, le=31)


class HistoryPlan(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    request: HistoryRequest
    slices: list[HistorySlice] = Field(min_length=1, max_length=1200)
    expected_days: int

    @model_validator(mode="after")
    def consistent(self):
        body = self.request
        if {s.request.symbol for s in self.slices} != set(body.symbols) or sum(
            s.expected_days for s in self.slices
        ) != self.expected_days:
            raise ValueError("历史计划任务范围或预期交易日不一致")
        for index, part in enumerate(self.slices):
            req, identity = part.request, part.identity
            if (
                req.command_id != f"history:{body.command_id}:{index:04}"
                or req.provider != body.provider
                or req.connection_id != body.connection_id
                or req.exchange != body.exchange
                or identity.source != body.provider
                or identity.symbol != req.symbol
                or req.start is None
                or req.end is None
                or not body.start <= req.start <= req.end <= body.end
                or req.start.replace(day=1) != req.end.replace(day=1)
                or identity.resolve(req.start).id != part.contract_id
                or identity.resolve(req.end).id != part.contract_id
                or len(identity.catalog.inputs) != 1
                or identity.catalog.inputs[0].version_id != body.contracts_version_id
            ):
                raise ValueError("历史计划任务与固定请求或实际合约不一致")
        return self


class HistorySummary(BaseModel):
    request: HistoryRequest
    task_count: int
    created_at: float


class HistoryBatch(BaseModel):
    plan: HistoryPlan
    tasks: list[Job]


class HistoryCoverageItem(BaseModel):
    symbol: str
    contract_id: str
    report: CoverageReport | None
    error: str | None = None


class HistoryCoverage(BaseModel):
    command_id: UUID
    complete: bool
    counts: dict[str, int]
    items: list[HistoryCoverageItem]


class HistoryDownloads:
    def __init__(self, sync):
        self.sync = sync

    def plan(self, body: HistoryRequest) -> HistoryPlan:
        sync = self.sync
        provider = sync.registry.get(body.provider)
        capabilities = [
            c
            for c in provider.manifest.capabilities
            if c.type_id == "futures.daily" and body.exchange in c.exchanges
        ]
        if provider.manifest.demo or len(capabilities) != 1:
            raise ValueError("历史计划需要唯一的真实日线采集能力")
        # Validate complete fixed evidence, including connection ownership, before admission.
        with sync.engine.connect() as conn:
            basis = {
                "manifest": {
                    "source": body.provider,
                    "scope": {"exchange": body.exchange, "connection_id": body.connection_id},
                }
            }
            for type_id, identifier in (
                ("futures.contracts", body.contracts_version_id),
                ("futures.calendar", body.calendar_version_id),
            ):
                _, rows = sync.coverage._reference(conn, basis, type_id, identifier, resolve=False)
                record = sync.coverage._record(conn, identifier, type_id)
                if record["manifest"].get("demo"):
                    raise ValueError("历史计划不能使用演示依据")
                if type_id == "futures.calendar":
                    calendar = {row["date"]: row["is_open"] for row in rows}
        evidence = sync.library.preview(body.contracts_version_id, limit=10001)
        slices = []
        for symbol in sorted(body.symbols):
            before = len(slices)
            catalog = source_catalog(
                lambda identifier, *, limit: evidence,
                SourceCatalogRequest(version_id=body.contracts_version_id, symbols=[symbol]),
            )
            identity = SourceIdentity(
                catalog_id=catalog_digest(catalog),
                catalog=catalog,
                source=body.provider,
                symbol=symbol,
                information_at=max(c.provenance.available_at for c in catalog.contracts),
            )
            matches = [
                c
                for c in catalog.contracts
                if any(s.symbol == symbol and s.contract_id == c.id for s in catalog.symbols)
                and c.listed_on <= body.end
                and c.last_trade_on is not None
                and c.last_trade_on >= body.start
            ]
            if len(matches) != 1:
                raise ValueError(f"{symbol} 在计划区间须对应唯一实际合约")
            contract = matches[0]
            assert contract.last_trade_on is not None
            start, end = max(body.start, contract.listed_on), min(body.end, contract.last_trade_on)
            day = start
            while day <= end:
                last = min(end, date(day.year, day.month, monthrange(day.year, day.month)[1]))
                days = [day + timedelta(days=i) for i in range((last - day).days + 1)]
                if any(d.isoformat() not in calendar for d in days):
                    raise ValueError(f"固定日历未完整覆盖 {symbol} 的 {day} 至 {last}")
                expected = sum(calendar[d.isoformat()] for d in days)
                if expected:
                    request = SyncRequest(
                        command_id=f"history:{body.command_id}:{len(slices):04}",
                        provider=body.provider,
                        connection_id=body.connection_id,
                        dataset=capabilities[0].id,
                        exchange=body.exchange,
                        symbol=symbol,
                        start=day,
                        end=last,
                    )
                    provider.plan(request)
                    # The same lifecycle check is enforced again by sync admission/worker.
                    if (
                        identity.resolve(day).id != contract.id
                        or identity.resolve(last).id != contract.id
                    ):
                        raise ValueError("历史计划跨越实际合约身份")
                    slices.append(
                        HistorySlice(
                            contract_id=contract.id,
                            request=request,
                            identity=identity,
                            expected_days=expected,
                        )
                    )
                    if len(slices) > 1200:
                        raise ValueError("历史计划最多 1200 个合约月份，请缩小范围")
                day = last + timedelta(days=1)
            if len(slices) == before:
                raise ValueError(f"{symbol} 所选范围没有需采集的交易日")
        return HistoryPlan(
            request=body, slices=slices, expected_days=sum(s.expected_days for s in slices)
        )

    def recent(self) -> list[HistorySummary]:
        with self.sync.engine.connect() as conn:
            rows = conn.execute(
                select(jobs)
                .where(
                    jobs.c.kind == "data.sync",
                    jobs.c.payload["history_plan"].as_string().is_not(None),
                )
                .order_by(jobs.c.created_at.desc(), jobs.c.id.desc())
                .limit(20)
            ).mappings()
            result = []
            for row in rows:
                plan = HistoryPlan.model_validate(row["payload"]["history_plan"])
                result.append(
                    HistorySummary(
                        request=plan.request,
                        task_count=len(plan.slices),
                        created_at=row["created_at"],
                    )
                )
            return result

    def _originals(self, conn, identifier):
        return [
            dict(r)
            for r in conn.execute(
                select(jobs)
                .where(jobs.c.command_id.like(f"history:{identifier}:%"))
                .order_by(jobs.c.command_id)
            ).mappings()
        ]

    def get(self, identifier: UUID) -> HistoryBatch:
        with self.sync.engine.connect() as conn:
            rows = self._originals(conn, identifier)
            if not rows:
                raise KeyError(str(identifier))
            if rows[0]["kind"] != "data.sync" or "history_plan" not in rows[0]["payload"]:
                raise Conflict("历史计划命令与已有任务冲突")
            plan = HistoryPlan.model_validate(rows[0]["payload"]["history_plan"])
            if plan.request.command_id != identifier or [r["command_id"] for r in rows] != [
                s.request.command_id for s in plan.slices
            ]:
                raise ValueError("历史计划任务记录不完整或不匹配")
            frontier = [r["id"] for r in rows]
            while frontier:
                children = [
                    dict(r)
                    for r in conn.execute(
                        select(jobs).where(
                            jobs.c.kind == "data.sync",
                            jobs.c.payload["retry_of"].as_string().in_(frontier),
                        )
                    ).mappings()
                ]
                rows.extend(children)
                frontier = [r["id"] for r in children]
        return HistoryBatch(plan=plan, tasks=[Job.model_validate(r) for r in rows])

    def submit(self, body: HistoryRequest) -> HistoryBatch:
        # Response-loss retry returns the original frozen configuration even if settings changed.
        try:
            old = self.get(body.command_id)
        except KeyError:
            old = None
        if old is not None:
            if old.plan.request != body:
                raise Conflict("历史计划命令已用于其他输入")
            return old
        plan = self.plan(body)
        commands = []
        fixed = self.sync.submission_payload(plan.slices[0].request)
        try:
            with self.sync.engine.begin() as conn:
                for index, part in enumerate(plan.slices):
                    payload = fixed | {
                        "request": part.request.model_dump(mode="json"),
                        "contract_identity": part.identity.model_dump(mode="json"),
                    }
                    if index == 0:
                        payload["history_plan"] = plan.model_dump(mode="json")
                    self.sync.validate_identity(conn, payload)
                    commands.append((part.request.command_id, "data.sync", payload))
                self.sync.tasks.submit_batch(conn, commands)
        except IntegrityError:
            old = self.get(body.command_id)
            if old.plan.request != body:
                raise Conflict("历史计划命令已用于其他输入") from None
            return old
        return self.get(body.command_id)

    def coverage(self, identifier: UUID) -> HistoryCoverage:
        batch = self.get(identifier)
        body = batch.plan.request
        with self.sync.engine.connect() as conn:
            published = [
                dict(r)
                for r in conn.execute(
                    select(versions).where(
                        versions.c.job_id.in_([t.id for t in batch.tasks if t.state == "SUCCEEDED"])
                    )
                ).mappings()
                if r["manifest"]["layer"] == "STANDARD"
            ]
        items = []
        counts = Counter()
        for symbol in sorted(body.symbols):
            parts = [s for s in batch.plan.slices if s.request.symbol == symbol]
            if not parts:
                raise ValueError("历史计划缺少所选合约任务")
            actual = parts[0].contract_id
            candidates = [r for r in published if r["manifest"]["scope"]["symbol"] == symbol]
            if not candidates:
                items.append(
                    HistoryCoverageItem(
                        symbol=symbol,
                        contract_id=actual,
                        report=None,
                        error="尚无成功发布的日线版本",
                    )
                )
                continue
            version = max(candidates, key=lambda r: r["manifest"]["revision"])
            try:
                report = CoverageReport.model_validate(
                    self.sync.coverage.check(
                        version["id"],
                        CoverageRequest(
                            start=body.start,
                            end=body.end,
                            calendar_version_id=body.calendar_version_id,
                            contracts_version_id=body.contracts_version_id,
                        ),
                    )
                )
                counts.update(report.counts)
                items.append(HistoryCoverageItem(symbol=symbol, contract_id=actual, report=report))
            except (ValueError, KeyError) as exc:
                items.append(
                    HistoryCoverageItem(
                        symbol=symbol, contract_id=actual, report=None, error=str(exc)
                    )
                )
        return HistoryCoverage(
            command_id=identifier,
            complete=all(i.report is not None and i.report.status == "COVERED" for i in items),
            counts=dict(counts),
            items=items,
        )
