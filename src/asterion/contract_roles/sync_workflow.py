"""Durable data-publication barrier, owned by the role plugin."""

from datetime import date

from pydantic import Field, model_validator
from sqlalchemy import JSON, Column, String, Table, select

from asterion.contract_roles.computed import ComputedSources
from asterion.contract_roles.computed_public import DailyInput, digest
from asterion.contract_roles.models import Strict
from asterion.contract_roles.sequence import ContinuationRequest, check_published, continuation
from asterion.contract_roles.tasks import KIND, Payload
from asterion.platform.store import metadata

workflows = Table(
    "role_sync_workflows",
    metadata,
    Column("id", String, primary_key=True),
    Column("request", JSON, nullable=False),
    Column("job_id", String),
    Column("error", String),
)


class SyncContinuation(Strict):
    command_id: str = Field(min_length=1, max_length=100)
    previous_version_id: str = Field(pattern=r"^[a-f0-9]{64}$")
    trading_day: date
    sync_job_ids: tuple[str, ...] = Field(min_length=2, max_length=1000)
    explanation: str = Field(min_length=1, max_length=2000)

    @model_validator(mode="after")
    def unique(self):
        if len(set(self.sync_job_ids)) != len(self.sync_job_ids):
            raise ValueError("日线依赖任务重复")
        return self


class WorkflowRecord(Strict):
    id: str = Field(pattern=r"^[a-f0-9]{64}$")
    request: SyncContinuation
    job_id: str | None
    error: str | None


class SyncWorkflow:
    def __init__(self, storage, tasks, data, roles):
        self.storage, self.tasks, self.data, self.roles = storage, tasks, data, roles
        storage.initialize(workflows)

    def validate_dependencies(self, body, rows):
        previous = self.roles.read(body.previous_version_id)
        self.roles.sources.verify(previous.spec)
        check_published(previous)
        days = sorted({s.trading_day for s in previous.spec.request.trading_time.spec.spans()})
        last = previous.spec.input.observations[-1].trading_day
        later = [d for d in days if d > last]
        if not later or body.trading_day != later[0]:
            raise ValueError("同步续算必须是下一个已确认观测交易日")
        active = {
            c.id
            for c in previous.spec.input.catalog.contracts
            if c.id in previous.spec.input.candidates
            and c.listed_on <= body.trading_day <= c.last_trade_on
        }
        symbols = {s.symbol for s in previous.spec.input.catalog.symbols if s.contract_id in active}
        if len(rows) != len(symbols) or {r["symbol"] for r in rows} != symbols:
            raise ValueError("日线依赖须精确覆盖当日全部存续候选")
        source = previous.spec.candidates.catalog.inputs[0].source
        for row in rows:
            if (
                row["provider"] != source
                or row["contracts_version_id"] != previous.spec.request.contracts_version_id
                or row["start"] != body.trading_day.isoformat()
                or row["end"] != body.trading_day.isoformat()
            ):
                raise ValueError("日线依赖来源、资料或观测日期不一致")
        return previous

    def advance(self, conn, row):
        if row["job_id"]:
            return dict(row)
        body = SyncContinuation.model_validate(row["request"])
        dependencies = self.data.inspect(conn, body.sync_job_ids, False)
        error = None
        job_id = None
        try:
            previous = self.validate_dependencies(body, dependencies)
            if any(r["state"] in {"FAILED", "CANCELLED"} for r in dependencies):
                raise ValueError("同步依赖失败或取消；须明确选择成功重试任务建立新工作流")
            if not all(r["state"] == "SUCCEEDED" for r in dependencies):
                return dict(row)
            request = ContinuationRequest(
                previous_version_id=body.previous_version_id,
                daily_inputs=tuple(
                    DailyInput(trading_day=body.trading_day, version_id=r["version_id"])
                    for r in dependencies
                ),
                explanation=body.explanation,
            )
            sources = ComputedSources(self.data.versions(conn))
            spec = continuation(sources, previous, request)
            payload = Payload(previous=previous, request=request, spec=spec)
        except (ValueError, OSError) as exc:
            error = str(exc)[:1000]
        else:
            records = self.tasks.submit_batch(
                conn, [("role-sync:" + row["id"], KIND, payload.model_dump(mode="json"))]
            )
            job_id = records[0]["id"]
        conn.execute(
            workflows.update().where(workflows.c.id == row["id"]).values(job_id=job_id, error=error)
        )
        return dict(row) | {"job_id": job_id, "error": error}

    def submit(self, body):
        identifier = digest({"command_id": body.command_id})
        with self.storage.begin() as conn:
            # Lock dependencies before enrolling, so completion cannot pass an invisible subscriber.
            dependencies = self.data.inspect(conn, body.sync_job_ids, True)
            row = (
                conn.execute(
                    select(workflows).where(workflows.c.id == identifier).with_for_update()
                )
                .mappings()
                .first()
            )
            if row:
                if row["request"] != body.model_dump(mode="json"):
                    raise ValueError("工作流命令标识已用于不同输入")
                return self.advance(conn, row)
            self.validate_dependencies(body, dependencies)
            row = {
                "id": identifier,
                "request": body.model_dump(mode="json"),
                "job_id": None,
                "error": None,
            }
            conn.execute(workflows.insert().values(**row))
            return self.advance(conn, row)

    def get(self, identifier):
        with self.storage.begin() as conn:
            row = (
                conn.execute(
                    select(workflows).where(workflows.c.id == identifier).with_for_update()
                )
                .mappings()
                .first()
            )
            if row is None:
                raise ValueError("同步续算工作流不存在")
            # Read-only status: GET never triggers submission.
            dependencies = self.data.inspect(conn, row["request"]["sync_job_ids"], False)
            return dict(row) | {"dependencies": dependencies}

    def published(self, transaction, job_id):
        with self.storage.join(transaction) as conn:
            pending = list(
                conn.execute(
                    select(workflows)
                    .where(workflows.c.job_id.is_(None))
                    .order_by(workflows.c.id)
                    .with_for_update()
                ).mappings()
            )
            for row in pending:
                if job_id in row["request"]["sync_job_ids"]:
                    self.advance(conn, row)
