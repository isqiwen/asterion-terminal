"""Generic provider orchestration and atomic catalogue publication."""

import hashlib
import json
import re
import time
from datetime import UTC, date, datetime
from pathlib import Path
from uuid import uuid4

import pyarrow as pa
import pyarrow.parquet as pq
from cryptography.fernet import InvalidToken
from sqlalchemy import select

from asterion.data.artifacts import atomic_write
from asterion.data.catalog import snapshots
from asterion.data.configuration import (
    ProviderConfigurations,
    configurations,
    validate,
    verification_records,
)
from asterion.data.connections import Connections
from asterion.data.coverage import DailyCoverage
from asterion.data.ingestion import IngestionEvidence
from asterion.data.library import DataLibrary, collections, versions
from asterion.data.partitions import SUPPORTED, prepare
from asterion.data.preparation import Preparations
from asterion.data.providers import builtin_registry
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.sync_identity import task_identity
from asterion.platform.secrets import SecretPort
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs
from asterion.platform.tasks.service import Conflict


class Credentials:
    """Local service-only files, owner read/write, outside published artifacts."""

    def __init__(self, root: Path, secrets: SecretPort):
        self._secrets = secrets
        self.root = root / ".credentials"

    def freeze_configuration(self, provider, schema_version, values, revision=0):
        if not re.fullmatch(r"[a-z][a-z0-9_]{0,40}", provider):
            raise ValueError("Invalid provider identifier")
        content = canonical(
            {
                "provider": provider,
                "schema_version": schema_version,
                "values": values,
                "revision": revision,
            }
        )
        ref = self._secrets.fingerprint(content)
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.root.chmod(0o700)
        folder = self.root / "configurations"
        folder.mkdir(exist_ok=True, mode=0o700)
        folder.chmod(0o700)
        path = folder / f"{ref}.enc"
        if path.exists():
            if self.read_configuration(ref, provider, schema_version, revision) != values:
                raise ProviderError("配置快照校验失败")
        else:
            atomic_write(path, self._secrets.encrypt(content))
        return ref

    def read_configuration(self, ref, provider, schema_version, revision):
        if not isinstance(ref, str) or not re.fullmatch(r"[0-9a-f]{64}", ref):
            raise ProviderError("配置快照引用无效")
        try:
            content = self._secrets.decrypt(
                (self.root / "configurations" / f"{ref}.enc").read_bytes()
            )
            value = json.loads(content)
            if (
                self._secrets.fingerprint(content) != ref
                or value["provider"] != provider
                or value["schema_version"] != schema_version
                or value["revision"] != revision
                or not isinstance(value["values"], dict)
            ):
                raise ValueError()
            return value["values"]
        except (OSError, InvalidToken, ValueError, KeyError, TypeError):
            raise ProviderError("固定配置无法读取或校验失败，请检查本机配置与运行密钥") from None

    def resolve_configuration(self, payload, provider):
        request = SyncRequest.model_validate(payload["request"])
        fixed = payload.get("configuration")
        if (
            not isinstance(fixed, dict)
            or set(fixed) != {"ref", "revision", "schema_version"}
            or type(fixed["revision"]) is not int
            or fixed["revision"] < 0
            or type(fixed["schema_version"]) is not int
        ):
            raise ProviderError("固定配置引用无效，请重新提交同步")
        if fixed["schema_version"] != provider.manifest.configuration.schema_version:
            raise ProviderError("配置版本已变化，请重新提交同步")
        configuration = self.read_configuration(
            fixed["ref"],
            request.connection_id or request.provider,
            fixed["schema_version"],
            fixed["revision"],
        )
        return validate(provider.manifest.configuration, configuration)


class DataSync:
    def __init__(
        self, engine, tasks, root: Path, secrets: SecretPort, registry=None, published=None
    ):
        self.published = published
        self.engine, self.tasks, self.root = engine, tasks, root
        self.registry = registry or builtin_registry(root)
        self.connections = Connections(engine, self.registry)
        self.library = DataLibrary(engine, root)
        self.evidence = IngestionEvidence(engine, tasks, root, self.registry, self.library.types)
        self.credentials = Credentials(root, secrets)
        engine.initialize(configurations)
        engine.initialize(verification_records)
        self.coverage = DailyCoverage(self)
        self.preparations = Preparations(self)

    @property
    def configuration(self):
        return ProviderConfigurations(self.engine, self.connections, self.credentials)

    def providers(self):
        result = []
        for plugin in self.registry.all():
            error = None
            try:
                state = self.configuration.state(plugin.manifest.id)
                configured, error = state.configured, state.error
            except ProviderError as exc:
                configured, error = False, str(exc)
            result.append(
                plugin.manifest.model_dump() | {"configured": configured, "credential_error": error}
            )
        installed = {plugin.manifest.id for plugin in self.registry.all()}
        for instance in self.connections.all():
            if instance["provider"] not in installed:
                continue
            plugin = self.registry.get(instance["provider"])
            state = self.configuration.state(instance["id"])
            result.append(
                plugin.manifest.model_dump()
                | {
                    "id": instance["id"],
                    "name": instance["name"],
                    "plugin_id": instance["provider"],
                    "connection_id": instance["id"],
                    "configured": state.configured,
                    "credential_error": state.error,
                }
            )
        for item in result:
            lifecycle = self.connections.state(item["id"])
            item["name"] = lifecycle.name
            item["lifecycle"] = lifecycle.model_dump()
            item["verification"] = self.configuration.verification(item["id"]).model_dump()
        return result

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
            preparation_id=job["payload"].get("preparation_id"),
            contract_identity=job["payload"].get("contract_identity"),
        )

    def submission_payload(self, request: SyncRequest):
        provider = self.registry.get(request.provider)
        provider.plan(request)
        capability = next(c for c in provider.manifest.capabilities if c.id == request.dataset)
        definition = self.library.types.get(capability.type_id).manifest
        owner = request.connection_id or request.provider
        if self.connections.state(owner).state != "enabled":
            raise ProviderError("连接已停用或归档，不能提交新的同步任务")
        if self.connections.resolve(owner)["provider"] != request.provider:
            raise ProviderError("连接实例与供应商插件不匹配")
        configuration = self.configuration.freeze(owner)
        return {
            "request": request.model_dump(mode="json"),
            "plugin_version": provider.manifest.version,
            "plugin_digest": getattr(provider, "digest", None),
            "type_id": definition.id,
            "type_schema_version": definition.schema_version,
            "configuration": configuration,
            "connection_name": self.connections.state(owner).name,
        }

    def submit(
        self,
        request: SyncRequest,
        resume_from=None,
        coverage_report_id=None,
        retry_of=None,
        preparation_id=None,
        contract_identity=None,
    ):
        provider = self.registry.get(request.provider)
        provider.plan(request)
        capability = next(c for c in provider.manifest.capabilities if c.id == request.dataset)
        admission = {
            "request": request.model_dump(mode="json"),
            "type_id": capability.type_id,
            "contract_identity": contract_identity,
            "preparation_id": preparation_id,
        }
        if task_identity(provider, admission) is not None:
            with self.engine.connect() as conn:
                self.validate_identity(conn, admission)

        def existing():
            with self.engine.connect() as conn:
                old = (
                    conn.execute(select(jobs).where(jobs.c.command_id == request.command_id))
                    .mappings()
                    .first()
                )
            if old:
                payload = old["payload"]
                if (
                    old["kind"] != "data.sync"
                    or SyncRequest.model_validate(payload["request"]) != request
                    or payload.get("resume_from") != resume_from
                    or payload.get("coverage_report_id") != coverage_report_id
                    or payload.get("retry_of") != retry_of
                    or payload.get("preparation_id") != preparation_id
                    or payload.get("contract_identity") != contract_identity
                ):
                    raise Conflict("command_id reused with different input")
                return dict(old)
            return None

        old = existing()
        if old:
            return old
        payload = self.submission_payload(request)
        if contract_identity is not None:
            payload["contract_identity"] = contract_identity
        if resume_from:
            payload["resume_from"] = resume_from
        if coverage_report_id:
            payload["coverage_report_id"] = coverage_report_id
        if retry_of:
            payload["retry_of"] = retry_of
        if preparation_id:
            payload["preparation_id"] = preparation_id
        try:
            return self.tasks.submit(request.command_id, "data.sync", payload)
        except Conflict:
            old = existing()
            if old:
                return old
            raise

    def progress(self, job_id, token, completed: int, total: int):
        with self.engine.begin() as conn:
            job = self.tasks.require_lease(conn, job_id, token)
            if job["kind"] != "data.sync":
                raise Conflict("Not a data sync job")
            count = len(
                self.registry.get(job["payload"]["request"]["provider"]).plan(
                    SyncRequest.model_validate(job["payload"]["request"])
                )
            )
            if total != count or not 0 <= completed <= total:
                raise ValueError("Invalid partition progress")
            self.tasks.require_lease(conn, job_id, token)
            self.tasks.progress(conn, job_id, token, {"completed": completed, "total": total})

    def validate_identity(self, conn, payload):
        from asterion.data.reference import SourceIdentity
        from asterion.data.reference_source import (
            SourceCatalogRequest,
            source_catalog,
            validate_catalog_input,
        )

        identity = SourceIdentity.model_validate(payload.get("contract_identity"))
        request = SyncRequest.model_validate(payload["request"])
        if identity.source != request.provider or identity.symbol != request.symbol:
            raise ProviderError("同步的来源代码与固定身份不一致")
        for item in identity.catalog.inputs:
            manifest = conn.execute(
                select(versions.c.manifest).where(versions.c.id == item.version_id)
            ).scalar_one_or_none()
            if manifest is None:
                raise ProviderError("同步的固定资料缺失")
            validate_catalog_input(item, manifest)
            if manifest["scope"].get("connection_id") != request.connection_id:
                raise ProviderError("同步的固定资料连接不一致")
        expected = source_catalog(
            self.library.preview,
            SourceCatalogRequest(
                version_id=identity.catalog.inputs[0].version_id, symbols=[request.symbol]
            ),
        )
        if expected != identity.catalog:
            raise ProviderError("固定身份目录与资料文件内容不一致")
        return identity

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
            if (
                provider.manifest.version != job["payload"]["plugin_version"]
                or getattr(provider, "digest", None) != job["payload"]["plugin_digest"]
            ):
                raise ProviderError("插件版本已变化，请重新提交同步")
            task_identity(provider, job["payload"])
            self.credentials.resolve_configuration(job["payload"], provider)
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
            bound_contract = data_type.manifest.id in {"futures.daily", "futures.settlement"}
            if bound_contract:
                scope["contract_ids"] = self.publication_identity(conn, job["payload"], rows)
            coverage = data_type.coverage(rows, request.start, request.end)
            folder = self.root / "datasets" / job_id
            atomic_write(folder / "evidence.json", content)
            atomic_write(folder / "rows.json", canonical(rows))
            standard = None
            chart_rows = rows
            standard_path = folder / "data.parquet"
            if data_type.manifest.id in SUPPORTED:
                observed_by_key = {
                    tuple(row[field] for field in data_type.manifest.primary_key): observed
                    for row, observed in normalized_observations
                }
                chart_rows, standard_path, standard = prepare(
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
                atomic_write(standard_path, output.getvalue().to_pybytes())
            if bound_contract:
                self.publication_identity(conn, job["payload"], chart_rows)
            standard_content = standard_path.read_bytes()
            snapshot_id = None
            if data_type.chart:
                bar_content, bar_manifest = data_type.chart(
                    chart_rows, max(observations, key=datetime.fromisoformat)
                )
                snapshot_id = str(uuid4())
                atomic_write(self.root / "published" / f"{snapshot_id}.parquet", bar_content)
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
            manifest = {
                "provider": request.provider,
                "plugin_version": provider.manifest.version,
                "plugin_digest": getattr(provider, "digest", None),
                "demo": provider.manifest.demo,
                "configuration": job["payload"]["configuration"],
                "schema_version": 1,
                "dataset": request.dataset,
                "exchange": request.exchange,
                "symbol": request.symbol,
                "request": request.model_dump(mode="json", exclude={"command_id"}),
                "origin": {
                    "method": "api",
                    "provider": request.provider,
                    "connection_id": request.connection_id,
                    "name": job["payload"].get("connection_name", request.provider),
                },
                "rows": len(rows),
                "observed_at": max(observations, key=datetime.fromisoformat),
                "available_at": max(observations, key=datetime.fromisoformat),
                "checksum": hashlib.sha256(standard_content).hexdigest(),
                "evidence_checksum": digest,
                "contract_identity": job["payload"].get("contract_identity"),
                "coverage_report_id": job["payload"].get("coverage_report_id"),
                "quality": "VALIDATED",
                "coverage": coverage,
                "empty_partitions": empty,
                "snapshot_id": snapshot_id,
                "units": {f.name: f.unit for f in data_type.manifest.fields if f.unit},
                "first": rows[0].get(data_type.manifest.time_field),
                "last": rows[-1].get(data_type.manifest.time_field),
            }
            stored_manifest = (
                manifest
                if not standard
                else manifest
                | standard["detail"]
                | {"rows": standard["rows"], "version_semantics": "CUMULATIVE"}
            )
            atomic_write(folder / "manifest.json", canonical(stored_manifest))
            self.tasks.require_lease(conn, job_id, token)
            _raw_id, version_id = self.library.publish_pair(
                conn,
                job_id=job_id,
                type_id=data_type.manifest.id,
                source=request.provider,
                scope=scope,
                raw_path=str((folder / "evidence.json").relative_to(self.root)),
                raw_format="provider_evidence",
                standard_path=str(standard_path.relative_to(self.root)),
                standard=standard,
                row_count=len(rows),
                snapshot_id=snapshot_id,
                plugin_version=provider.manifest.version,
                detail={
                    key: manifest[key]
                    for key in (
                        "request",
                        "plugin_digest",
                        "origin",
                        "observed_at",
                        "available_at",
                        "coverage",
                        "empty_partitions",
                        "first",
                        "last",
                        "evidence_checksum",
                        "coverage_report_id",
                        "contract_identity",
                        "demo",
                        "configuration",
                    )
                },
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


def collect(
    payload: dict, root: Path, secrets: SecretPort, progress=None, checkpoint=None, reused=None
) -> bytes:
    request = SyncRequest.model_validate(payload["request"])
    provider = builtin_registry(root).get(request.provider)
    if (
        provider.manifest.version != payload["plugin_version"]
        or getattr(provider, "digest", None) != payload["plugin_digest"]
    ):
        raise ProviderError("插件版本已变化，请重新提交同步")
    task_identity(provider, payload)
    credentials = Credentials(root, secrets)
    configuration = credentials.resolve_configuration(payload, provider)
    plan, evidence = provider.plan(request), []
    for index, partition in enumerate(plan):
        if progress:
            progress(index, len(plan))
        cached = (reused or {}).get(str(index))
        if cached:
            evidence.append(cached)
        else:
            try:
                rows = provider.fetch(partition, configuration)
            except ProviderError as exc:
                code = re.split(r"[:：]", str(exc), maxsplit=1)[0]
                if code not in {
                    "AUTH_FAILED",
                    "PERMISSION_DENIED",
                    "RATE_LIMITED",
                    "PROVIDER_REJECTED",
                }:
                    code = "FETCH_FAILED"
                if checkpoint:
                    checkpoint(
                        index,
                        {
                            "partition": partition.model_dump(),
                            "rows": [],
                            "observed_at": datetime.now(UTC).isoformat(),
                            "error_code": code,
                        },
                    )
                raise ProviderError(code + "：采集失败，请检查数据源后重试") from None
            # Only declared fields enter the evidence store.
            rows = [{field: row.get(field) for field in partition.fields} for row in rows]
            evidence.append(
                {
                    "partition": partition.model_dump(),
                    "rows": rows,
                    "observed_at": datetime.now(UTC).isoformat(),
                }
            )
        if checkpoint:
            checkpoint(index, evidence[-1])
        if len(canonical(evidence)) > 7_500_000:
            raise ProviderError("本次同步超过 7.5 MB，请缩小范围")
        if progress:
            progress(index + 1, len(plan))
    return canonical(evidence)
