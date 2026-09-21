"""Named connection instances, separate from installed provider implementations."""

from typing import Literal
from uuid import uuid4

from pydantic import BaseModel, Field
from sqlalchemy import Column, Integer, String, Table, select
from sqlalchemy.exc import IntegrityError

from asterion.data.providers.public import ProviderError
from asterion.platform.store import metadata
from asterion.platform.tasks.service import Conflict

connections = Table(
    "data_connections",
    metadata,
    Column("id", String, primary_key=True),
    Column("provider", String, nullable=False),
    Column("name", String, nullable=False),
)


connection_settings = Table(
    "data_connection_settings",
    metadata,
    Column("id", String, primary_key=True),
    Column("name", String, nullable=False),
    Column("state", String, nullable=False),
    Column("revision", Integer, nullable=False),
)


class ConnectionUpdate(BaseModel):
    expected_revision: int = Field(ge=0)
    name: str = Field(min_length=1, max_length=80)
    state: Literal["enabled", "disabled", "archived"]


class ConnectionState(BaseModel):
    id: str
    provider: str
    name: str
    state: Literal["enabled", "disabled", "archived"] = "enabled"
    revision: int = 0


class NewConnection(BaseModel):
    provider: str
    name: str = Field(min_length=1, max_length=80)


class Connections:
    def __init__(self, engine, plugins):
        self.engine, self.plugins = engine, plugins
        engine.initialize(connections)
        engine.initialize(connection_settings)

    def all(self):
        with self.engine.connect() as conn:
            return [dict(row) for row in conn.execute(select(connections)).mappings()]

    def resolve(self, identifier):
        if identifier in {p.manifest.id for p in self.plugins.all()}:
            return {
                "id": identifier,
                "provider": identifier,
                "name": self.plugins.get(identifier).manifest.name,
            }
        with self.engine.connect() as conn:
            row = (
                conn.execute(select(connections).where(connections.c.id == identifier))
                .mappings()
                .first()
            )
        if not row:
            raise ProviderError("连接实例不存在")
        return dict(row)

    def get(self, identifier):
        return self.plugins.get(self.resolve(identifier)["provider"])

    def create(self, body: NewConnection):
        self.plugins.get(body.provider)
        name = body.name.strip()
        if not name:
            raise ProviderError("连接名称不能为空")
        record = {"id": "c_" + uuid4().hex, "provider": body.provider, "name": name}
        with self.engine.begin() as conn:
            conn.execute(connections.insert().values(**record))
        return record

    def state(self, identifier):
        base = self.resolve(identifier)
        with self.engine.connect() as conn:
            saved = (
                conn.execute(
                    select(connection_settings).where(connection_settings.c.id == identifier)
                )
                .mappings()
                .first()
            )
        return ConnectionState(**(base | (dict(saved) if saved else {})))

    def update(self, identifier, body: ConnectionUpdate):
        self.resolve(identifier)
        name = body.name.strip()
        if not name:
            raise ProviderError("连接名称不能为空")
        values = {
            "id": identifier,
            "name": name,
            "state": body.state,
            "revision": body.expected_revision + 1,
        }
        try:
            with self.engine.begin() as conn:
                if body.expected_revision == 0:
                    conn.execute(connection_settings.insert().values(**values))
                else:
                    updated = conn.execute(
                        connection_settings.update()
                        .where(
                            connection_settings.c.id == identifier,
                            connection_settings.c.revision == body.expected_revision,
                        )
                        .values(**values)
                    )
                    if updated.rowcount != 1:
                        raise Conflict("连接已在其他窗口更新，请刷新后重试")
        except IntegrityError:
            raise Conflict("连接已在其他窗口更新，请刷新后重试") from None
        return self.state(identifier)
