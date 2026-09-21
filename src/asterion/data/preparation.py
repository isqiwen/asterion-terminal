"""Atomic, durable preparation of same-connection daily research inputs."""

import time
from typing import Literal

from pydantic import BaseModel, ConfigDict, model_validator
from sqlalchemy import JSON, Column, Float, String, Table, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert
from sqlalchemy.exc import IntegrityError

from asterion.data.library import stable_id
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.reference import SourceIdentity
from asterion.data.reference_source import SourceCatalogRequest, source_catalog
from asterion.data.reference_store import catalog_digest
from asterion.platform.store import jobs, metadata
from asterion.platform.tasks.public import Job
from asterion.platform.tasks.service import Conflict

TYPES = ("futures.daily", "futures.calendar", "futures.contracts")
batches = Table(
    "data_preparations",
    metadata,
    Column("id", String, primary_key=True),
    Column("created_at", Float, nullable=False),
    Column("request", JSON, nullable=False),
    Column("daily_payload", JSON, nullable=False),
    Column("daily_state", String, nullable=False),
    Column("identity", JSON),
    Column("identity_error", String),
    Column("connection_name", String, nullable=False, default=""),
)


class PreparationTask(BaseModel):
    type_id: str
    job: Job


class Preparation(BaseModel):
    daily_state: Literal["WAITING_REFERENCE", "SUBMITTED", "IDENTITY_REJECTED"]
    identity: SourceIdentity | None
    identity_error: str | None
    id: str
    created_at: float
    request: SyncRequest
    connection_name: str
    tasks: list[PreparationTask]
    truncated: bool = False


class PreparationJournal(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str
    created_at: float
    request: SyncRequest
    connection_name: str
    daily_payload: dict
    daily_state: Literal["WAITING_REFERENCE", "SUBMITTED", "IDENTITY_REJECTED"]
    identity: SourceIdentity | None
    identity_error: str | None

    @model_validator(mode="after")
    def consistent(self):
        payload = self.daily_payload
        expected = self.request.model_dump(mode="json") | {
            "command_id": stable_id({"preparation": self.id, "type": "futures.daily"})
        }
        if (
            payload.get("request") != expected
            or payload.get("type_id") != "futures.daily"
            or payload.get("preparation_id") != self.id
        ):
            raise ValueError("准备批次的待提交日线与已接受请求不一致")
        if self.daily_state == "SUBMITTED":
            if self.identity is None or self.identity_error is not None:
                raise ValueError("准备批次缺少固定来源身份")
            if (
                self.identity.source != self.request.provider
                or self.identity.symbol != self.request.symbol
            ):
                raise ValueError("准备批次的固定来源身份与请求不一致")
        elif self.identity is not None or (
            bool(self.identity_error) != (self.daily_state == "IDENTITY_REJECTED")
        ):
            raise ValueError("准备批次依赖状态不一致")
        return self


class Preparations:
    def __init__(self, sync):
        self.sync = sync
        sync.engine.initialize(batches)

    def submit(self, request: SyncRequest):
        try:
            return self._submit(request)
        except IntegrityError:
            raise Conflict("准备任务与已有命令冲突，未提交本批数据") from None

    def _submit(self, request: SyncRequest):
        body = request.model_dump(mode="json")
        with self.sync.engine.begin() as conn:
            insert = pg_insert if conn.dialect.name == "postgresql" else sqlite_insert
            inserted = conn.execute(
                insert(batches)
                .values(
                    id=request.command_id,
                    created_at=time.time(),
                    request=body,
                    daily_payload={},
                    daily_state="WAITING_REFERENCE",
                )
                .on_conflict_do_nothing(index_elements=["id"])
                .returning(batches.c.id)
            ).scalar_one_or_none()
            stored = (
                conn.execute(select(batches).where(batches.c.id == request.command_id))
                .mappings()
                .one()
            )
            if stored["request"] != body:
                raise Conflict("研究数据准备命令已用于其他输入")
            if inserted:
                plugin = self.sync.registry.get(request.provider)
                if plugin.manifest.demo:
                    raise ProviderError("研究数据准备不支持开发示例")
                capabilities = []
                for type_id in TYPES:
                    matches = [
                        c
                        for c in plugin.manifest.capabilities
                        if c.type_id == type_id and request.exchange in c.exchanges
                    ]
                    if len(matches) != 1:
                        raise ProviderError(
                            "此连接无法唯一提供该交易所的日线、日历和合约资料，请分别同步"
                        )
                    capabilities.append(matches[0])
                if (
                    request.dataset != capabilities[0].id
                    or not capabilities[0].date_range
                    or not capabilities[0].symbol_required
                    or not capabilities[1].date_range
                    or not request.symbol.strip()
                    or not request.start
                    or not request.end
                ):
                    raise ProviderError("请选择实际合约日线及完整日期范围")
                requests = [
                    SyncRequest.model_validate(
                        body
                        | {
                            "command_id": stable_id(
                                {"preparation": request.command_id, "type": c.type_id}
                            ),
                            "dataset": c.id,
                            "symbol": request.symbol if c.symbol_required else "",
                            "start": body["start"] if c.date_range else None,
                            "end": body["end"] if c.date_range else None,
                        }
                    )
                    for c in capabilities
                ]
                for item in requests:
                    plugin.plan(item)
                # Freeze once: every job in this batch uses exactly the same configuration.
                base = self.sync.submission_payload(requests[0])
                commands = []
                for item, capability in zip(requests, capabilities, strict=True):
                    definition = self.sync.library.types.get(capability.type_id).manifest
                    payload = base | {
                        "request": item.model_dump(mode="json"),
                        "type_id": definition.id,
                        "type_schema_version": definition.schema_version,
                        "preparation_id": request.command_id,
                    }
                    if capability.type_id == "futures.daily":
                        conn.execute(
                            batches.update()
                            .where(batches.c.id == request.command_id)
                            .values(daily_payload=payload)
                        )
                    else:
                        commands.append((item.command_id, "data.sync", payload))
                self.sync.tasks.submit_batch(conn, commands)
                conn.execute(
                    batches.update()
                    .where(batches.c.id == request.command_id)
                    .values(connection_name=base["connection_name"])
                )
        return self.get(request.command_id)

    def reference_published(self, conn, job, version, rows):
        """Publish the dependency and queue its consumer in the same fenced transaction."""
        if job["payload"]["type_id"] != "futures.contracts":
            return
        batch = (
            conn.execute(
                select(batches)
                .where(batches.c.id == job["payload"]["preparation_id"])
                .with_for_update()
            )
            .mappings()
            .one()
        )
        PreparationJournal.model_validate(dict(batch))
        if batch["daily_state"] != "WAITING_REFERENCE":
            raise Conflict("准备批次已固定合约依据，不能替换")
        payload = batch["daily_payload"]
        request = SyncRequest.model_validate(payload["request"])
        try:
            catalog = source_catalog(
                lambda identifier, limit: {"version": version, "rows": rows, "total": len(rows)},
                SourceCatalogRequest(version_id=version["id"], symbols=[request.symbol]),
            )
            identity = SourceIdentity(
                catalog_id=catalog_digest(catalog),
                catalog=catalog,
                source=request.provider,
                symbol=request.symbol,
                information_at=max(item.provenance.available_at for item in catalog.contracts),
            )
            from asterion.data.sync_identity import task_identity

            task_identity(
                self.sync.registry.get(request.provider),
                payload | {"contract_identity": identity.model_dump(mode="json")},
            )
        except ValueError:
            # The observation is valid as source data, but cannot authorize market identity.
            conn.execute(
                batches.update()
                .where(batches.c.id == batch["id"])
                .values(
                    daily_state="IDENTITY_REJECTED",
                    identity_error="固定合约资料无法确认完整且唯一的身份；请检查代码、交割年月和生命周期后新建准备批次。",
                )
            )
            return
        evidence = identity.model_dump(mode="json")
        self.sync.tasks.submit_batch(
            conn, [(request.command_id, "data.sync", payload | {"contract_identity": evidence})]
        )
        conn.execute(
            batches.update()
            .where(batches.c.id == batch["id"])
            .values(
                daily_state="SUBMITTED",
                identity=evidence,
            )
        )

    def retry(self, original, command_id, resume):
        """One retry branch per prepared input, retaining the batch's accepted configuration."""
        with self.sync.engine.begin() as conn:
            conn.execute(
                select(batches.c.id)
                .where(batches.c.id == original["payload"]["preparation_id"])
                .with_for_update()
            ).one()
            payload = original["payload"] | {
                "request": original["payload"]["request"] | {"command_id": command_id},
                "retry_of": original["id"],
            }
            payload.pop("resume_from", None)
            if resume:
                payload["resume_from"] = original["id"]
            from asterion.data.sync_identity import task_identity

            provider = self.sync.registry.get(payload["request"]["provider"])
            if task_identity(provider, payload) is not None:
                self.sync.validate_identity(conn, payload)
            existing = (
                conn.execute(select(jobs).where(jobs.c.command_id == command_id)).mappings().first()
            )
            if existing is not None:
                if existing["kind"] != "data.sync" or existing["payload"] != payload:
                    raise Conflict("重试命令已用于其他输入")
                return dict(existing)
            child = conn.execute(
                select(jobs.c.id).where(jobs.c.payload["retry_of"].as_string() == original["id"])
            ).first()
            if child is not None:
                raise Conflict("该准备任务已有重试，请继续当前尝试，不能创建并行分支")
            return self.sync.tasks.submit_batch(conn, [(command_id, "data.sync", payload)])[0]

    def get(self, identifier):
        with self.sync.engine.connect() as conn:
            batch = (
                conn.execute(select(batches).where(batches.c.id == identifier)).mappings().first()
            )
            if batch is None:
                raise KeyError(identifier)
            records = (
                conn.execute(
                    select(jobs)
                    .where(
                        jobs.c.kind == "data.sync",
                        jobs.c.payload["preparation_id"].as_string() == identifier,
                    )
                    .order_by(jobs.c.created_at.desc(), jobs.c.id)
                    .limit(201)
                )
                .mappings()
                .all()
            )
        PreparationJournal.model_validate(dict(batch))
        parents = {row["payload"].get("retry_of") for row in records}
        return Preparation(
            **{key: value for key, value in batch.items() if key != "daily_payload"},
            truncated=len(records) > 200,
            tasks=[
                PreparationTask(type_id=row["payload"]["type_id"], job=Job.model_validate(row))
                for row in reversed(records[:200])
                if row["id"] not in parents
            ],
        )

    def list(self):
        with self.sync.engine.connect() as conn:
            ids = (
                conn.execute(select(batches.c.id).order_by(batches.c.created_at.desc()).limit(10))
                .scalars()
                .all()
            )
        return [self.get(identifier) for identifier in ids]
