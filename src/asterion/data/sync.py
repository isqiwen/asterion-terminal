"""Generic provider orchestration and atomic catalogue publication."""

import hashlib
import json
import time
from datetime import date, datetime
from pathlib import Path
from uuid import uuid4

import pyarrow as pa
import pyarrow.parquet as pq
from asterion_bindings import data_sync
from asterion_bindings.catalog import SourceIdentity
from asterion_bindings.data_partitions import cumulative_series
from asterion_bindings.data_sources import SourceCredentials
from asterion_bindings.task_repository import Conflict
from sqlalchemy import select

from asterion.data.catalog import snapshots
from asterion.data.connections import TABLES
from asterion.data.coverage import DailyCoverage
from asterion.data.ingestion import IngestionEvidence
from asterion.data.library import DataLibrary, collections, stable_id, versions
from asterion.data.partitions import prepare
from asterion.data.preparation import Preparations
from asterion.data.providers import builtin_registry
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.sources import DataSources, refused
from asterion.data.sync_identity import task_identity
from asterion.platform.store import jobs


def fixed_configuration(credentials: SourceCredentials, payload: dict, provider) -> dict:
    """The runnable values a sync task was fixed to; never current settings."""
    request = SyncRequest.model_validate(payload["request"])
    try:
        return credentials.resolve(
            payload.get("configuration"),
            request.connection_id or request.provider,
            provider.manifest.configuration.model_dump(mode="json"),
        )
    except ValueError as error:
        raise ProviderError(str(error)) from None


class DataSync:
    def __init__(
        self,
        engine,
        tasks,
        root: Path,
        credentials: SourceCredentials,
        registry=None,
        published=None,
    ):
        self.published = published
        self.engine, self.tasks, self.root = engine, tasks, root
        self.registry = registry or builtin_registry()
        self.library = DataLibrary(engine, root)
        self.evidence = IngestionEvidence(engine, root)
        self.credentials = credentials
        self.sources = DataSources(engine, self.registry, credentials)
        for table in TABLES:
            engine.initialize(table)
        self.coverage = DailyCoverage(self)
        self.preparations = Preparations(self)

    def providers(self):
        return self.sources.listing()

    def retry(self, job_id: str, command_id: str, resume: bool = False):
        with self.engine.connect() as conn:
            job = conn.execute(select(jobs).where(jobs.c.id == job_id)).mappings().first()
        if not job or job["kind"] != "data.sync" or job["state"] not in {"FAILED", "CANCELLED"}:
            raise Conflict("只能重新提交失败或已取消的数据同步任务")
        if job["payload"].get("preparation_id"):
            return self.preparations.retry(job, command_id, resume)
        request = SyncRequest.model_validate(job["payload"]["request"])
        return self.submit(
            request.model_copy(update={"command_id": command_id}),
            resume_from=job_id if resume else None,
            coverage_report_id=job["payload"].get("coverage_report_id"),
            retry_of=job_id,
            minute_context=job["payload"].get("minute_context"),
            preparation_id=job["payload"].get("preparation_id"),
            contract_identity=job["payload"].get("contract_identity"),
        )

    def submission_payload(self, request: SyncRequest):
        with self.engine.connect() as conn:
            return self.submission_payload_in(conn, request)

    def submission_payload_in(self, conn, request: SyncRequest):
        with refused():
            return data_sync.payload(conn, self.credentials, request.model_dump(mode="json"))

    def submit(
        self,
        request: SyncRequest,
        resume_from=None,
        coverage_report_id=None,
        retry_of=None,
        preparation_id=None,
        contract_identity=None,
        minute_context=None,
    ):
        admission = {
            "resume_from": resume_from,
            "coverage_report_id": coverage_report_id,
            "retry_of": retry_of,
            "preparation_id": preparation_id,
            "contract_identity": contract_identity,
            "minute_context": minute_context,
        }
        with self.engine.begin() as conn, refused():
            return data_sync.submit(
                conn, self.credentials, self.root, request.model_dump(mode="json"), admission
            )

    def admit(self, submission: dict):
        """Admit a workbench sync request: minute sessions and contract identity."""
        with self.engine.begin() as conn, refused():
            return data_sync.admit(conn, self.credentials, self.root, submission)

    def validate_identity(self, conn, payload):
        with refused():
            return SourceIdentity.model_validate(
                data_sync.validate_identity(conn, self.root, payload)
            )

    def publication_identity(self, conn, payload, rows):
        identity = self.validate_identity(conn, payload)
        return identity.validate_rows(rows, payload["request"]["exchange"])

    def publish(self, job_id, token, content: bytes):
        digest = hashlib.sha256(content).hexdigest()
        with self.engine.begin() as conn:
            job = (
                conn.execute(select(jobs).where(jobs.c.id == job_id).with_for_update())
                .mappings()
                .first()
            )
            if job and job["state"] == "SUCCEEDED" and job["token"] == token:
                old = (
                    conn.execute(
                        select(versions)
                        .join(collections)
                        .where(versions.c.job_id == job_id, collections.c.layer == "STANDARD")
                    )
                    .mappings()
                    .one()
                )
                if old["manifest"]["evidence_checksum"] != digest:
                    raise Conflict("Publication retry changed content")
                return dict(old)
            job = self.tasks.require_lease(conn, job_id, token)
            if job["kind"] != "data.sync":
                raise Conflict("Not a data sync job")
            request = SyncRequest.model_validate(job["payload"]["request"])
            provider = self.registry.get(request.provider)
            if provider.manifest.version != job["payload"]["plugin_version"]:
                raise ProviderError("插件版本已变化，请重新提交同步")
            task_identity(job["payload"])
            fixed_configuration(self.credentials, job["payload"], provider)
            envelope = json.loads(content)
            plan = provider.plan(request)
            if not isinstance(envelope, list) or len(envelope) != len(plan):
                raise ProviderError("同步分段不完整，未发布")
            self.evidence.verify_publication(conn, job, envelope)
            raw_rows, observations, empty = [], [], []
            normalized_observations = []
            for part, evidence in zip(plan, envelope, strict=True):
                if evidence["partition"] != part.model_dump():
                    raise ProviderError("同步结果与已接收的请求不一致")
                observed = datetime.fromisoformat(evidence["observed_at"])
                if not observed.tzinfo or not (
                    (evidence.get("reused_from") or job["created_at"] <= observed.timestamp())
                    and observed.timestamp() <= time.time() + 5
                ):
                    raise ProviderError("采集时间证据无效")
                rows = evidence["rows"]
                if not isinstance(rows, list) or len(rows) >= part.limit:
                    raise ProviderError("分段结果不完整或触及接口上限")
                if any(not isinstance(r, dict) or set(r) != set(part.fields) for r in rows):
                    raise ProviderError("采集字段与插件声明不一致")
                partition_request = request
                if request.start:
                    if not part.start or not part.end:
                        raise ProviderError("采集分段日期缺失")
                    partition_request = request.model_copy(
                        update={
                            "start": date.fromisoformat(part.start),
                            "end": date.fromisoformat(part.end),
                        }
                    )
                normalized_observations.extend(
                    (row, observed.isoformat())
                    for row in provider.normalize(partition_request, rows)
                )
                observations.append(observed.isoformat())
                if not rows:
                    empty.append(part.params)
                raw_rows.extend(rows)
            rows = provider.normalize(request, raw_rows)
            minute_context = None
            if job["payload"]["type_id"] == "futures.minute":
                from asterion.data.minute import MinuteContext, bind_rows

                minute_context = MinuteContext.model_validate(job["payload"].get("minute_context"))
                rows = bind_rows(
                    rows, minute_context, max(observations, key=datetime.fromisoformat)
                )
                for row, observed in normalized_observations:
                    bind_rows([row], minute_context, observed)
            data_type = self.library.types.get(job["payload"]["type_id"])
            if data_type.manifest.schema_version != job["payload"]["type_schema_version"]:
                raise ProviderError("数据类型版本已变化，请重新提交同步")
            data_type.validate(rows)
            if not rows:
                raise ProviderError(
                    "EMPTY_UNCONFIRMED：接口返回空数据，不能确认是休市、权限或缺失；未发布"
                )
            scope: dict = {"exchange": request.exchange, "symbol": request.symbol} | (
                {"connection_id": request.connection_id} if request.connection_id else {}
            )
            if minute_context is not None:
                scope.update(
                    frequency=minute_context.frequency,
                    trading_time_id=minute_context.trading_time.id,
                    timestamp_semantics=minute_context.timestamp_semantics,
                )
            bound_contract = data_type.manifest.id in {
                "futures.daily",
                "futures.settlement",
                "futures.minute",
            }
            if bound_contract:
                scope["contract_ids"] = self.publication_identity(conn, job["payload"], rows)
            coverage = data_type.coverage(rows, request.start, request.end)
            artifacts = self.library.artifacts
            # Addressed by content: an expired attempt cannot rewrite bytes that a
            # committed version of this job already references.
            evidence = artifacts.put_addressed(f"datasets/{job_id}/evidence", ".json", content)
            standard = None
            chart_rows = rows
            if cumulative_series(data_type.manifest.id):
                observed_by_key = {
                    tuple(row[field] for field in data_type.manifest.primary_key): observed
                    for row, observed in normalized_observations
                }
                chart_rows, stored, standard = prepare(
                    self.library,
                    conn,
                    job_id=job_id,
                    type_id=data_type.manifest.id,
                    source=request.provider,
                    scope=scope,
                    rows=rows,
                    observed_by_key=observed_by_key,
                )
            else:
                output = pa.BufferOutputStream()
                pq.write_table(pa.Table.from_pylist(rows), output)
                stored = artifacts.put_addressed(
                    f"datasets/{job_id}/data", ".parquet", output.getvalue().to_pybytes()
                )
            if bound_contract:
                self.publication_identity(conn, job["payload"], chart_rows)
            snapshot_id = None
            if data_type.chart:
                # Validate chart semantics on changed months without rebuilding old history.
                bar_content, chart_manifest = data_type.chart(
                    chart_rows, max(observations, key=datetime.fromisoformat)
                )
                snapshot_id = str(uuid4())
                if standard:
                    dataset_id = stable_id(
                        self.library.identity(
                            data_type.manifest.id,
                            request.provider,
                            scope,
                            "STANDARD",
                            standard["series"],
                        )
                    )
                    bar_manifest = {
                        "schema_version": 1,
                        "storage": "version",
                        "version_id": stable_id({"dataset_id": dataset_id, "job_id": job_id}),
                        "checksum": stored.sha256,
                        "rows": standard["rows"],
                        "contracts": sorted({row["contract"] for row in chart_rows}),
                        "start": standard["detail"]["first"] + "T00:00:00+00:00",
                        "end": standard["detail"]["last"] + "T00:00:00+00:00",
                        "frequency": "1d",
                        "time_semantics": "trading_day_label",
                    }
                else:
                    bar_manifest = chart_manifest
                    artifacts.put(f"published/{snapshot_id}.parquet", bar_content)
                bar_manifest |= {
                    "uri": f"asterion://local/published/{snapshot_id}",
                    "source": provider.manifest.name
                    if provider.manifest.demo
                    else request.provider,
                    "demo": provider.manifest.demo,
                    "state": "PUBLISHED",
                    "type_id": data_type.manifest.id,
                }
                conn.execute(
                    snapshots.insert().values(id=snapshot_id, job_id=job_id, manifest=bar_manifest)
                )
            observed_at = max(observations, key=datetime.fromisoformat)
            detail = {
                "request": request.model_dump(mode="json", exclude={"command_id"}),
                "plugin_digest": None,
                "origin": {
                    "method": "api",
                    "provider": request.provider,
                    "connection_id": request.connection_id,
                    "name": job["payload"].get("connection_name", request.provider),
                },
                "observed_at": observed_at,
                "available_at": observed_at,
                "coverage": coverage,
                "empty_partitions": empty,
                "first": rows[0].get(data_type.manifest.time_field),
                "last": rows[-1].get(data_type.manifest.time_field),
                "evidence_checksum": digest,
                "coverage_report_id": job["payload"].get("coverage_report_id"),
                "contract_identity": job["payload"].get("contract_identity"),
                "minute_context": job["payload"].get("minute_context"),
                "demo": provider.manifest.demo,
                "configuration": job["payload"]["configuration"],
            }
            self.tasks.require_lease(conn, job_id, token)
            _raw_id, version_id = self.library.publish_pair(
                conn,
                job_id=job_id,
                type_id=data_type.manifest.id,
                source=request.provider,
                scope=scope,
                raw=evidence,
                raw_format="provider_evidence",
                standard=stored,
                cumulative=standard,
                row_count=len(rows),
                snapshot_id=snapshot_id,
                plugin_version=provider.manifest.version,
                detail=detail,
            )
            record = dict(
                conn.execute(select(versions).where(versions.c.id == version_id)).mappings().one()
            )
            if job["payload"].get("preparation_id"):
                self.preparations.reference_published(conn, job, record, rows)
            self.tasks.require_lease(conn, job_id, token)
            self.tasks.complete(
                conn,
                job_id,
                token,
                {
                    "dataset_id": record["dataset_id"],
                    "version_id": version_id,
                    "type_id": data_type.manifest.id,
                    "snapshot_id": snapshot_id,
                    "completed": len(plan),
                    "total": len(plan),
                },
            )
            if self.published is not None:
                self.published(conn, job_id)
            return record
