"""Lease-fenced observations retained independently of standard publication."""

import hashlib
import time
from datetime import date, datetime

from pydantic import BaseModel, ConfigDict, Field
from sqlalchemy import JSON, Column, Integer, String, Table, func, select

from asterion.data.artifacts import atomic_write
from asterion.data.providers.public import Partition, ProviderError, SyncRequest
from asterion.data.types import builtin_types
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs, metadata
from asterion.platform.tasks.service import Conflict

observations = Table(
    "ingestion_observations",
    metadata,
    Column("job_id", String, primary_key=True),
    Column("attempt", Integer, primary_key=True),
    Column("partition_index", Integer, primary_key=True),
    Column("manifest", JSON, nullable=False),
)


class EvidenceSource(BaseModel):
    model_config = ConfigDict(extra="forbid")
    job_id: str
    attempt: int = Field(ge=1)
    partition_index: int = Field(ge=0)
    checksum: str


class Observation(BaseModel):
    model_config = ConfigDict(extra="forbid")
    partition: Partition
    observed_at: datetime
    rows: list[dict] = Field(max_length=10000)
    error_code: str | None = Field(
        default=None,
        pattern=r"^(AUTH_FAILED|PERMISSION_DENIED|RATE_LIMITED|PROVIDER_REJECTED|FETCH_FAILED)$",
    )
    reused_from: EvidenceSource | None = None


class ObservationManifest(BaseModel):
    checksum: str
    uri: str
    bytes: int
    rows: int
    partition: Partition
    observed_at: str
    status: str
    plugin_version: str
    reused_from: EvidenceSource | None = None


class ObservationRecord(BaseModel):
    job_id: str
    attempt: int
    partition_index: int
    manifest: ObservationManifest


class ObservationPage(BaseModel):
    items: list[ObservationRecord]
    total: int


class ObservationPreview(BaseModel):
    manifest: ObservationManifest
    rows: list[dict]
    total: int


class IngestionEvidence:
    def __init__(self, engine, tasks, root, registry, types=None):
        self.engine, self.tasks, self.root, self.registry = engine, tasks, root, registry
        self.types = types or builtin_types()
        engine.initialize(observations)

    def _read(self, job_id, attempt, index, manifest):
        path = (
            self.root
            / "sources"
            / job_id
            / str(attempt)
            / str(index)
            / f"{manifest['checksum']}.json"
        )
        try:
            content = path.read_bytes()
        except FileNotFoundError:
            raise ProviderError("原始证据文件缺失") from None
        if hashlib.sha256(content).hexdigest() != manifest["checksum"]:
            raise ProviderError("原始证据校验和不一致")
        return Observation.model_validate_json(content)

    def _reusable(self, conn, job, source: EvidenceSource):
        if source.job_id != job["payload"].get("resume_from") and not (
            source.job_id == job["id"] and source.attempt < job["attempt"]
        ):
            raise Conflict("Evidence source is not an authorized resume input")
        original = conn.execute(select(jobs).where(jobs.c.id == source.job_id)).mappings().one()
        old_config = original["payload"].get("configuration", {}).get("ref")
        new_config = job["payload"].get("configuration", {}).get("ref")
        if not old_config or not new_config or old_config != new_config:
            raise ProviderError("采集配置缺失或已变化，不能复用分段")
        for key in ("plugin_version", "type_id", "type_schema_version"):
            if original["payload"][key] != job["payload"][key]:
                raise ProviderError("采集版本不兼容，不能复用")
        request = SyncRequest.model_validate(job["payload"]["request"])
        old_request = SyncRequest.model_validate(original["payload"]["request"])
        if request.model_dump(exclude={"command_id"}) != old_request.model_dump(
            exclude={"command_id"}
        ):
            raise Conflict("Resume request changed")
        manifest = conn.execute(
            select(observations.c.manifest).where(
                observations.c.job_id == source.job_id,
                observations.c.attempt == source.attempt,
                observations.c.partition_index == source.partition_index,
            )
        ).scalar_one_or_none()
        if not manifest or manifest["checksum"] != source.checksum:
            raise Conflict("Resume evidence changed or missing")
        value = self._read(source.job_id, source.attempt, source.partition_index, manifest)
        provider = self.registry.get(request.provider)
        data_type = self.types.get(job["payload"]["type_id"])
        if provider.manifest.version != job["payload"]["plugin_version"] or (
            data_type.manifest.schema_version != job["payload"]["type_schema_version"]
        ):
            raise ProviderError("采集版本不兼容，不能复用")
        plan = provider.plan(request)
        if (
            not 0 <= source.partition_index < len(plan)
            or value.partition != plan[source.partition_index]
        ):
            raise ProviderError("采集计划不兼容，不能复用")
        if value.error_code or not value.rows or len(value.rows) >= value.partition.limit:
            raise ProviderError("分段没有可复用的完整响应")
        if request.start:
            if not value.partition.start or not value.partition.end:
                raise ProviderError("采集分段日期缺失")
            request = request.model_copy(
                update={
                    "start": date.fromisoformat(value.partition.start),
                    "end": date.fromisoformat(value.partition.end),
                }
            )
        rows = provider.normalize(request, value.rows)
        data_type.validate(rows)
        data_type.coverage(rows, request.start, request.end)
        return value.model_copy(update={"reused_from": source})

    def resume(self, job_id, token):
        with self.engine.begin() as conn:
            job = self.tasks.require_lease(conn, job_id, token)
            if job["kind"] != "data.sync":
                raise Conflict("Not a data sync job")
            candidates = (
                conn.execute(
                    select(observations)
                    .where(
                        (
                            (observations.c.job_id == job_id)
                            & (observations.c.attempt < job["attempt"])
                        )
                        | (observations.c.job_id == job["payload"].get("resume_from", ""))
                    )
                    .order_by(
                        (observations.c.job_id == job_id).desc(), observations.c.attempt.desc()
                    )
                )
                .mappings()
                .all()
            )
            reused = {}
            retained_bytes = 0
            for candidate in candidates:
                index = candidate["partition_index"]
                if index in reused:
                    continue
                source = EvidenceSource(
                    job_id=candidate["job_id"],
                    attempt=candidate["attempt"],
                    partition_index=index,
                    checksum=candidate["manifest"]["checksum"],
                )
                try:
                    value = self._reusable(conn, job, source)
                    size = len(canonical(value.model_dump(mode="json")))
                    if retained_bytes + size > 7_000_000:
                        continue
                    reused[index] = value
                    retained_bytes += size
                except (ValueError, OSError):
                    # Missing, corrupt, empty or invalid partitions must be fetched again.
                    continue
        for index, value in reused.items():
            self.record(job_id, token, index, value)
        return {index: value.model_dump(mode="json") for index, value in reused.items()}

    def verify_publication(self, conn, job, envelope):
        saved = (
            conn.execute(
                select(observations)
                .where(observations.c.job_id == job["id"], observations.c.attempt == job["attempt"])
                .order_by(observations.c.partition_index)
            )
            .mappings()
            .all()
        )
        if not saved:
            if any(value.get("reused_from") for value in envelope):
                raise Conflict("Reused evidence must be retained before publication")
            return
        if len(saved) != len(envelope):
            raise Conflict("Publication is missing retained observations")
        for index, (record, value) in enumerate(zip(saved, envelope, strict=True)):
            digest = hashlib.sha256(
                canonical(Observation.model_validate(value).model_dump(mode="json"))
            ).hexdigest()
            if record["partition_index"] != index or record["manifest"]["checksum"] != digest:
                raise Conflict("Publication changed retained observation")

    def record(self, job_id, token, index, value: Observation):
        content = canonical(value.model_dump(mode="json"))
        if len(content) > 8_000_000:
            raise ProviderError("分段证据超过 8 MB")
        digest = hashlib.sha256(content).hexdigest()
        with self.engine.begin() as conn:
            job = self.tasks.require_lease(conn, job_id, token)
            if job["kind"] != "data.sync":
                raise Conflict("Not a data sync job")
            request = SyncRequest.model_validate(job["payload"]["request"])
            provider = self.registry.get(request.provider)
            if provider.manifest.version != job["payload"]["plugin_version"]:
                raise Conflict("Provider version changed")
            plan = provider.plan(request)
            if not 0 <= index < len(plan) or value.partition != plan[index]:
                raise ProviderError("分段证据与采集计划不一致")
            if value.reused_from and (
                value.reused_from.partition_index != index
                or self._reusable(conn, job, value.reused_from) != value
            ):
                raise Conflict("Reused observation changed")
            if not value.observed_at.tzinfo or not (
                (
                    value.reused_from is not None
                    or job["created_at"] <= value.observed_at.timestamp()
                )
                and value.observed_at.timestamp() <= time.time() + 5
            ):
                raise ProviderError("采集时间证据无效")
            if value.error_code and value.rows:
                raise ProviderError("失败证据不能包含成功响应")
            if any(set(row) != set(value.partition.fields) for row in value.rows):
                raise ProviderError("采集字段与插件声明不一致")
            key = (
                (observations.c.job_id == job_id)
                & (observations.c.attempt == job["attempt"])
                & (observations.c.partition_index == index)
            )
            old = conn.execute(select(observations.c.manifest).where(key)).scalar_one_or_none()
            if old:
                if old["checksum"] != digest:
                    raise Conflict("Observation retry changed content")
                return old
            path = f"sources/{job_id}/{job['attempt']}/{index}/{digest}.json"
            manifest = {
                "checksum": digest,
                "uri": f"asterion://local/{path}",
                "bytes": len(content),
                "rows": len(value.rows),
                "partition": value.partition.model_dump(),
                "observed_at": value.observed_at.isoformat(),
                "status": value.error_code or ("RECEIVED" if value.rows else "EMPTY_UNCONFIRMED"),
                "plugin_version": provider.manifest.version,
                "reused_from": value.reused_from.model_dump() if value.reused_from else None,
            }
            atomic_write(self.root / path, content)
            self.tasks.require_lease(conn, job_id, token)
            conn.execute(
                observations.insert().values(
                    job_id=job_id, attempt=job["attempt"], partition_index=index, manifest=manifest
                )
            )
            return manifest

    def list(self, job_id, offset=0, limit=50):
        with self.engine.connect() as conn:
            if (
                conn.execute(
                    select(jobs.c.id).where(jobs.c.id == job_id, jobs.c.kind == "data.sync")
                ).scalar_one_or_none()
                is None
            ):
                raise KeyError(job_id)
            query = select(observations).where(observations.c.job_id == job_id)
            total = conn.execute(select(func.count()).select_from(query.subquery())).scalar_one()
            items = conn.execute(
                query.order_by(observations.c.attempt, observations.c.partition_index)
                .offset(offset)
                .limit(limit)
            ).mappings()
            return {"items": [dict(row) for row in items], "total": total}

    def preview(self, job_id, attempt, index, offset=0, limit=100):
        with self.engine.connect() as conn:
            manifest = conn.execute(
                select(observations.c.manifest).where(
                    observations.c.job_id == job_id,
                    observations.c.attempt == attempt,
                    observations.c.partition_index == index,
                )
            ).scalar_one_or_none()
        if manifest is None:
            raise KeyError(job_id)
        value = self._read(job_id, attempt, index, manifest)
        return {
            "manifest": manifest,
            "rows": value.rows[offset : offset + limit],
            "total": len(value.rows),
        }
