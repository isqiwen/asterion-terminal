"""Reversible version archiving. No physical deletion or partition garbage collection."""

import time
from collections import Counter

from pydantic import BaseModel, ConfigDict, Field
from sqlalchemy import select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert

from asterion.data.coverage import reports
from asterion.data.library import versions
from asterion.data.version_state import version_states
from asterion.platform.tasks.service import Conflict


class ArchiveRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    archived: bool
    expected_revision: int = Field(ge=0)


class VersionLifecycle:
    def __init__(self, engine, readers=()):
        self.engine, self.readers = engine, readers
        engine.initialize(version_states)

    def _inspect(self, conn, version_id):
        record = (
            conn.execute(select(versions).where(versions.c.id == version_id)).mappings().first()
        )
        if record is None:
            raise KeyError(version_id)
        state = (
            conn.execute(select(version_states).where(version_states.c.version_id == version_id))
            .mappings()
            .first()
        )
        counts = Counter()
        for manifest in conn.execute(select(versions.c.manifest)).scalars():
            if (
                version_id in manifest.get("inputs", [])
                or manifest.get("parent_version_id") == version_id
            ):
                counts["data_lineage"] += 1
            if manifest.get("source") == "local_file":
                from asterion.data.public import ImportOptions

                identity = ImportOptions.model_validate(manifest["import_options"]).identity
                if any(item.version_id == version_id for item in identity.catalog.inputs):
                    counts["import_identity"] += 1
            if manifest.get("contract_identity"):
                from asterion.data.reference import SourceIdentity

                identity = SourceIdentity.model_validate(manifest["contract_identity"])
                if any(item.version_id == version_id for item in identity.catalog.inputs):
                    counts["sync_identity"] += 1
        for report in conn.execute(select(reports.c.report)).scalars():
            if version_id in {
                report.get("daily_version_id"),
                report.get("calendar_version_id"),
                report.get("contracts_version_id"),
            }:
                counts["coverage_reports"] += 1
        for reader in self.readers:
            counts.update(reader(conn, version_id))
        latest = conn.execute(
            select(versions.c.id)
            .where(versions.c.dataset_id == record["dataset_id"])
            .order_by(versions.c.created_at.desc(), versions.c.id.desc())
            .limit(1)
        ).scalar_one()
        return {
            "version_id": version_id,
            "archived": bool(state and state["archived"]),
            "revision": state["revision"] if state else 0,
            "updated_at": state["updated_at"] if state else None,
            "is_latest": latest == version_id,
            "references": dict(counts),
            "reference_count": sum(counts.values()),
            "protected": True,
            "can_delete": False,
            "protection_reason": "所有版本保留文件与固定引用；暂不提供永久删除或分区回收。",
        }

    def inspect(self, version_id):
        with self.engine.connect() as conn:
            return self._inspect(conn, version_id)

    def archive(self, version_id, body: ArchiveRequest):
        with self.engine.begin() as conn:
            if (
                conn.execute(
                    select(versions.c.id).where(versions.c.id == version_id).with_for_update()
                ).scalar_one_or_none()
                is None
            ):
                raise KeyError(version_id)
            insert = pg_insert if conn.dialect.name == "postgresql" else sqlite_insert
            conn.execute(
                insert(version_states)
                .values(version_id=version_id, archived=False, revision=0, updated_at=time.time())
                .on_conflict_do_nothing(index_elements=["version_id"])
            )
            state = (
                conn.execute(
                    select(version_states)
                    .where(version_states.c.version_id == version_id)
                    .with_for_update()
                )
                .mappings()
                .one()
            )
            if state["revision"] != body.expected_revision:
                if (
                    state["revision"] == body.expected_revision + 1
                    and state["archived"] == body.archived
                ):
                    return self._inspect(conn, version_id)
                raise Conflict("版本归档状态已被另一窗口修改，请刷新后重试")
            result = conn.execute(
                version_states.update()
                .where(
                    version_states.c.version_id == version_id,
                    version_states.c.revision == body.expected_revision,
                )
                .values(
                    archived=body.archived,
                    revision=body.expected_revision + 1,
                    updated_at=time.time(),
                )
            )
            if result.rowcount != 1:
                raise Conflict("版本归档状态已改变，请刷新后重试")
            return self._inspect(conn, version_id)
