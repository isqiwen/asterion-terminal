"""Account-scoped editable research documents; never executable task input."""

import time
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field
from sqlalchemy import JSON, Boolean, Column, Float, Integer, String, Table, select
from sqlalchemy.exc import IntegrityError

from asterion.contract_rules.public import RuleVersion
from asterion.platform.store import metadata
from asterion.platform.tasks.service import Conflict
from asterion.research.parameters import ParameterValue
from asterion.research.strategies import StrategyRef

workspaces = Table(
    "research_documents",
    metadata,
    Column("owner", String, primary_key=True),
    Column("id", String, primary_key=True),
    Column("name", String, nullable=False),
    Column("revision", Integer, nullable=False),
    Column("content", JSON, nullable=False),
    Column("updated_at", Float, nullable=False),
    Column("deleted", Boolean, nullable=False, default=False),
)


class DraftConfig(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)
    version_id: str = Field(default="", max_length=100)
    start: str = Field(default="", max_length=32)
    end: str = Field(default="", max_length=32)
    strategy: StrategyRef | None = None
    parameters: dict[str, ParameterValue | None] = Field(default_factory=dict)
    assumption: Literal["historical-close-unverified-calendar"] = (
        "historical-close-unverified-calendar"
    )
    lots: float | None = 1
    slippage_ticks: float | None = 1
    capital: str = Field(default="100000", max_length=100)
    rules: RuleVersion | None = None
    coverage_policy: Literal["require_complete", "allow_incomplete"] = "require_complete"
    coverage_note: str = Field(default="", max_length=500)
    coverage_report_id: str | None = Field(default=None, max_length=100)


class DraftContent(BaseModel):
    model_config = ConfigDict(extra="forbid")
    schema_version: Literal[1] = 1
    config: DraftConfig = Field(default_factory=DraftConfig)
    selected_run_id: str = Field(default="", max_length=100)


class DocumentUpdate(BaseModel):
    model_config = ConfigDict(extra="forbid")
    expected_revision: int = Field(ge=0)
    name: str = Field(default="", max_length=80)
    content: DraftContent


class DeleteDocument(BaseModel):
    expected_revision: int = Field(ge=1)


class ResearchWorkspace:
    def __init__(self, engine):
        self.engine = engine
        engine.initialize(workspaces)

    @staticmethod
    def public(row):
        DraftContent.model_validate(row["content"])
        return {k: row[k] for k in ("id", "name", "revision", "content", "updated_at")}

    def read(self, owner):
        with self.engine.connect() as conn:
            rows = (
                conn.execute(
                    select(workspaces)
                    .where(workspaces.c.owner == owner, ~workspaces.c.deleted)
                    .order_by(workspaces.c.updated_at.desc())
                )
                .mappings()
                .all()
            )
        return {
            "draft": next((self.public(r) for r in rows if r["id"] == "draft"), None),
            "templates": [self.public(r) for r in rows if r["id"] != "draft"],
        }

    def save(self, owner, document_id, body: DocumentUpdate):
        name = "研究草稿" if document_id == "draft" else body.name.strip()
        if not name:
            raise ValueError("请填写模板名称")
        content = body.content.model_dump(mode="json")
        try:
            with self.engine.begin() as conn:
                old = (
                    conn.execute(
                        select(workspaces)
                        .where(workspaces.c.owner == owner, workspaces.c.id == document_id)
                        .with_for_update()
                    )
                    .mappings()
                    .first()
                )
                if (
                    old
                    and not old["deleted"]
                    and old["revision"] == body.expected_revision + 1
                    and old["content"] == content
                    and old["name"] == name
                ):
                    return self.public(old)  # Response-lost retry.
                if (
                    old
                    and (old["deleted"] or old["revision"] != body.expected_revision)
                    or not old
                    and body.expected_revision != 0
                ):
                    raise Conflict("另一窗口已修改此草稿或模板，请加载服务器版本后再保存")
                values = {
                    "name": name,
                    "revision": body.expected_revision + 1,
                    "content": content,
                    "updated_at": time.time(),
                    "deleted": False,
                }
                if old:
                    changed = conn.execute(
                        workspaces.update()
                        .where(
                            workspaces.c.owner == owner,
                            workspaces.c.id == document_id,
                            workspaces.c.revision == body.expected_revision,
                            ~workspaces.c.deleted,
                        )
                        .values(**values)
                    )
                    if changed.rowcount != 1:
                        raise Conflict("草稿或模板已被另一窗口修改")
                else:
                    conn.execute(workspaces.insert().values(owner=owner, id=document_id, **values))
                return {"id": document_id, **{k: v for k, v in values.items() if k != "deleted"}}
        except IntegrityError:
            raise Conflict("草稿或模板已被另一窗口创建，请重新加载") from None

    def delete(self, owner, document_id, revision):
        with self.engine.begin() as conn:
            changed = conn.execute(
                workspaces.update()
                .where(
                    workspaces.c.owner == owner,
                    workspaces.c.id == document_id,
                    workspaces.c.revision == revision,
                    ~workspaces.c.deleted,
                )
                .values(deleted=True, revision=revision + 1, updated_at=time.time())
            )
            if changed.rowcount != 1:
                old = (
                    conn.execute(
                        select(workspaces).where(
                            workspaces.c.owner == owner, workspaces.c.id == document_id
                        )
                    )
                    .mappings()
                    .first()
                )
                if not old or not old["deleted"] or old["revision"] != revision + 1:
                    raise Conflict("模板已修改或删除，请刷新模板列表")
        return {"status": "deleted"}
