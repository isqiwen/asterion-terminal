"""One request enrolls all fixed candidates and their continuation barrier."""

from datetime import date

from pydantic import Field
from sqlalchemy import select

from asterion.contract_roles.computed_public import digest
from asterion.contract_roles.models import Strict
from asterion.contract_roles.sequence import check_published
from asterion.contract_roles.sync_workflow import SyncContinuation, workflows
from asterion.data.public import DailySyncBatch


class BatchContinuation(Strict):
    command_id: str = Field(min_length=1, max_length=100)
    previous_version_id: str = Field(pattern=r"^[a-f0-9]{64}$")
    trading_day: date
    connection_id: str | None
    explanation: str = Field(min_length=1, max_length=2000)


def submit(workflow, batch, body: BatchContinuation):
    identifier = digest({"command_id": body.command_id})
    with workflow.storage.begin() as conn:
        # Serialize same-predecessor requests, including concurrent lost-response retries.
        locked = conn.execute(
            select(workflow.roles.table.c.id)
            .where(workflow.roles.table.c.id == body.previous_version_id)
            .with_for_update()
        ).scalar_one_or_none()
        if locked is None:
            raise ValueError("计算前序版本不存在")
        existing = (
            conn.execute(select(workflows).where(workflows.c.id == identifier).with_for_update())
            .mappings()
            .first()
        )
        if existing:
            request = existing["request"]
            if any(
                request[key] != body.model_dump(mode="json")[key]
                for key in ("previous_version_id", "trading_day", "explanation")
            ):
                raise ValueError("批量续算命令标识已用于不同输入")
            dependencies = workflow.data.inspect(conn, request["sync_job_ids"], False)
            if any(row["connection_id"] != body.connection_id for row in dependencies):
                raise ValueError("批量续算不能用同一命令更换连接")
            return dict(existing)
        plan = preview(workflow, body.previous_version_id)
        if body.trading_day != plan.trading_day:
            raise ValueError("批量续算必须采集下一个已确认观测交易日")
        jobs = batch.submit(
            conn,
            DailySyncBatch(
                command_prefix="role-daily:" + identifier,
                provider=plan.provider,
                connection_id=body.connection_id,
                exchange=plan.exchange,
                contracts_version_id=plan.contracts_version_id,
                trading_day=body.trading_day,
                symbols=plan.symbols,
            ),
        )
        request = SyncContinuation(
            command_id=body.command_id,
            previous_version_id=body.previous_version_id,
            trading_day=body.trading_day,
            sync_job_ids=tuple(job.id for job in jobs),
            explanation=body.explanation,
        )
        workflow.validate_dependencies(
            request, workflow.data.inspect(conn, request.sync_job_ids, False)
        )
        row = {
            "id": identifier,
            "request": request.model_dump(mode="json"),
            "job_id": None,
            "error": None,
        }
        conn.execute(workflows.insert().values(**row))
        return row


class BatchPlan(Strict):
    previous_version_id: str
    trading_day: date
    provider: str
    exchange: str
    contracts_version_id: str
    symbols: tuple[str, ...]


def preview(workflow, identifier: str) -> BatchPlan:
    previous = workflow.roles.read(identifier)
    workflow.roles.sources.verify(previous.spec)
    check_published(previous)
    last = previous.spec.input.observations[-1].trading_day
    days = sorted(
        {
            s.trading_day
            for s in previous.spec.request.trading_time.spec.spans()
            if s.trading_day > last
        }
    )
    if not days:
        raise ValueError("固定交易日历未覆盖下一个观测交易日")
    day = days[0]
    active = {
        c.id
        for c in previous.spec.input.catalog.contracts
        if c.id in previous.spec.input.candidates and c.listed_on <= day <= c.last_trade_on
    }
    symbols = tuple(
        sorted(s.symbol for s in previous.spec.input.catalog.symbols if s.contract_id in active)
    )
    if len(symbols) < 2:
        raise ValueError("下一观测交易日不足两个存续候选")
    return BatchPlan(
        previous_version_id=identifier,
        trading_day=day,
        provider=previous.spec.candidates.catalog.inputs[0].source,
        exchange=previous.spec.request.trading_time.spec.exchange,
        contracts_version_id=previous.spec.request.contracts_version_id,
        symbols=symbols,
    )
