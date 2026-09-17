"""Generic provider orchestration and atomic catalogue publication."""

import base64
import hashlib
import hmac
import json
import os
import re
import tempfile
import time
from datetime import UTC, date, datetime
from pathlib import Path
from uuid import uuid4

import pyarrow as pa
import pyarrow.parquet as pq
from cryptography.fernet import Fernet, InvalidToken
from sqlalchemy import select

from asterion.data.catalog import snapshots
from asterion.data.library import DataLibrary, collections, versions
from asterion.data.providers import builtin_registry
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.platform.store import jobs
from asterion.platform.tasks.service import Conflict


def canonical(value):
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode()


def atomic_write(path: Path, content: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(name, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


class Credentials:
    """Local service-only files, owner read/write, outside published artifacts."""

    def __init__(self, root: Path, master_key: str):
        key = hmac.digest(master_key.encode(), b"asterion.provider.credentials.v1", "sha256")
        self.cipher = Fernet(base64.urlsafe_b64encode(key))
        self.root = root / ".credentials"
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.root.chmod(0o700)

    def path(self, provider):
        if not re.fullmatch(r"[a-z][a-z0-9_]{0,40}", provider):
            raise ValueError("Invalid provider identifier")
        return self.root / f"{provider}.enc"

    def read(self, provider):
        path = self.path(provider)
        try:
            return self.cipher.decrypt(path.read_bytes()).decode() if path.exists() else ""
        except InvalidToken:
            raise ProviderError("凭据无法解密，请在设置中重新保存 Token") from None

    def save(self, provider, credential):
        path = self.path(provider)
        if not credential:
            path.unlink(missing_ok=True)
        else:
            atomic_write(path, self.cipher.encrypt(credential.encode()))


class DataSync:
    def __init__(self, engine, tasks, root: Path, master_key: str, registry=None):
        self.engine, self.tasks, self.root = engine, tasks, root
        self.registry = registry or builtin_registry()
        self.library = DataLibrary(engine, root)
        self.credentials = Credentials(root, master_key)

    def providers(self):
        result = []
        for plugin in self.registry.all():
            error = None
            try:
                configured = bool(self.credentials.read(plugin.manifest.id))
            except ProviderError as exc:
                configured, error = False, str(exc)
            result.append(
                plugin.manifest.model_dump() | {"configured": configured, "credential_error": error}
            )
        return result

    def retry(self, job_id: str, command_id: str):
        with self.engine.connect() as conn:
            job = conn.execute(select(jobs).where(jobs.c.id == job_id)).mappings().first()
        if not job or job["kind"] != "data.sync" or job["state"] not in {"FAILED", "CANCELLED"}:
            raise Conflict("只能重新提交失败或已取消的数据同步任务")
        request = SyncRequest.model_validate(job["payload"]["request"])
        return self.submit(request.model_copy(update={"command_id": command_id}))

    def submit(self, request: SyncRequest):
        provider = self.registry.get(request.provider)
        provider.plan(request)
        capability = next(c for c in provider.manifest.capabilities if c.id == request.dataset)
        definition = self.library.types.get(capability.type_id).manifest
        if not self.credentials.read(request.provider):
            raise ProviderError("请先在设置 → 数据源中配置 Token")
        return self.tasks.submit(
            request.command_id,
            "data.sync",
            {
                "request": request.model_dump(mode="json"),
                "plugin_version": provider.manifest.version,
                "type_id": definition.id,
                "type_schema_version": definition.schema_version,
            },
        )

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
            conn.execute(
                jobs.update()
                .where(jobs.c.id == job_id)
                .values(result={"completed": completed, "total": total})
            )

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
            envelope = json.loads(content)
            plan = provider.plan(request)
            if not isinstance(envelope, list) or len(envelope) != len(plan):
                raise ProviderError("同步分段不完整，未发布")
            raw_rows, observations, empty = [], [], []
            for part, evidence in zip(plan, envelope, strict=True):
                if evidence["partition"] != part.model_dump():
                    raise ProviderError("同步结果与已接收的请求不一致")
                observed = datetime.fromisoformat(evidence["observed_at"])
                if (
                    not observed.tzinfo
                    or not job["created_at"] <= observed.timestamp() <= time.time() + 5
                ):
                    raise ProviderError("采集时间证据无效")
                rows = evidence["rows"]
                if not isinstance(rows, list) or len(rows) >= part.limit:
                    raise ProviderError("分段结果不完整或触及接口上限")
                if any(not isinstance(r, dict) or set(r) != set(part.fields) for r in rows):
                    raise ProviderError("采集字段与插件声明不一致")
                partition_request = request
                if request.start:
                    partition_request = request.model_copy(
                        update={
                            "start": date.fromisoformat(part.params["start_date"]),
                            "end": date.fromisoformat(part.params["end_date"]),
                        }
                    )
                provider.normalize(partition_request, rows)
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
            coverage = data_type.coverage(rows, request.start, request.end)
            folder = self.root / "datasets" / job_id
            normalized = canonical(rows)
            output = pa.BufferOutputStream()
            pq.write_table(pa.Table.from_pylist(rows), output)
            parquet = output.getvalue().to_pybytes()
            snapshot_id = None
            if data_type.chart:
                bar_content, bar_manifest = data_type.chart(rows, max(observations))
                snapshot_id = str(uuid4())
                atomic_write(self.root / "published" / f"{snapshot_id}.parquet", bar_content)
                bar_manifest |= {
                    "uri": f"asterion://local/published/{snapshot_id}",
                    "source": request.provider,
                    "state": "PUBLISHED",
                    "type_id": data_type.manifest.id,
                }
                conn.execute(
                    snapshots.insert().values(id=snapshot_id, job_id=job_id, manifest=bar_manifest)
                )
            manifest = {
                "provider": request.provider,
                "plugin_version": provider.manifest.version,
                "schema_version": 1,
                "dataset": request.dataset,
                "exchange": request.exchange,
                "symbol": request.symbol,
                "request": request.model_dump(mode="json", exclude={"command_id"}),
                "rows": len(rows),
                "observed_at": max(observations),
                "available_at": max(observations),
                "checksum": hashlib.sha256(parquet).hexdigest(),
                "evidence_checksum": digest,
                "quality": "VALIDATED",
                "coverage": coverage,
                "empty_partitions": empty,
                "snapshot_id": snapshot_id,
                "units": {f.name: f.unit for f in data_type.manifest.fields if f.unit},
                "first": rows[0].get(data_type.manifest.time_field),
                "last": rows[-1].get(data_type.manifest.time_field),
            }
            atomic_write(folder / "evidence.json", content)
            atomic_write(folder / "rows.json", normalized)
            atomic_write(folder / "data.parquet", parquet)
            atomic_write(folder / "manifest.json", canonical(manifest))
            self.tasks.require_lease(conn, job_id, token)
            _raw_id, version_id = self.library.publish_pair(
                conn,
                job_id=job_id,
                type_id=data_type.manifest.id,
                source=request.provider,
                scope={"exchange": request.exchange, "symbol": request.symbol},
                raw_path=str((folder / "evidence.json").relative_to(self.root)),
                raw_format="provider_evidence",
                standard_path=str((folder / "data.parquet").relative_to(self.root)),
                row_count=len(rows),
                snapshot_id=snapshot_id,
                plugin_version=provider.manifest.version,
                detail={
                    key: manifest[key]
                    for key in (
                        "request",
                        "observed_at",
                        "available_at",
                        "coverage",
                        "empty_partitions",
                        "first",
                        "last",
                        "evidence_checksum",
                    )
                },
            )
            record = dict(
                conn.execute(select(versions).where(versions.c.id == version_id)).mappings().one()
            )
            self.tasks.require_lease(conn, job_id, token)
            conn.execute(
                jobs.update()
                .where(jobs.c.id == job_id)
                .values(
                    state="SUCCEEDED",
                    result={
                        "type_id": data_type.manifest.id,
                        "snapshot_id": snapshot_id,
                        "completed": len(plan),
                        "total": len(plan),
                    },
                )
            )
            return record


def collect(payload: dict, root: Path, master_key: str, progress=None) -> bytes:
    request = SyncRequest.model_validate(payload["request"])
    provider = builtin_registry().get(request.provider)
    if provider.manifest.version != payload["plugin_version"]:
        raise ProviderError("插件版本已变化，请重新提交同步")
    credential = Credentials(root, master_key).read(request.provider)
    plan, evidence = provider.plan(request), []
    for index, partition in enumerate(plan):
        if progress:
            progress(index, len(plan))
        rows = provider.fetch(partition, credential)
        # Only declared fields enter the evidence store.
        rows = [{field: row[field] for field in partition.fields} for row in rows]
        evidence.append(
            {
                "partition": partition.model_dump(),
                "rows": rows,
                "observed_at": datetime.now(UTC).isoformat(),
            }
        )
        if len(canonical(evidence)) > 7_500_000:
            raise ProviderError("本次同步超过 7.5 MB，请缩小范围")
        if progress:
            progress(index + 1, len(plan))
    return canonical(evidence)
