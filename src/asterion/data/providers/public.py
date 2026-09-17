"""Versioned provider boundary. Plugins own transport and mapping, never catalogue writes."""

from datetime import date, datetime
from typing import Any, Protocol
from zoneinfo import ZoneInfo

from pydantic import BaseModel, ConfigDict, Field, model_validator


class SyncRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    command_id: str = Field(min_length=1, max_length=100)
    provider: str = Field(pattern=r"^[a-z][a-z0-9_]{0,40}$")
    dataset: str = Field(min_length=1, max_length=50)
    exchange: str = Field(min_length=1, max_length=10)
    symbol: str = Field(default="", max_length=30)
    start: date | None = None
    end: date | None = None

    @model_validator(mode="after")
    def dates(self):
        if (self.start is None) != (self.end is None):
            raise ValueError("开始和结束日期必须同时填写")
        if self.start and self.end:
            if self.start > self.end or (self.end - self.start).days > 3660:
                raise ValueError("日期范围须有序且不超过十年")
            if self.end > datetime.now(ZoneInfo("Asia/Shanghai")).date():
                raise ValueError("历史同步不能请求未来日期")
        return self


class Capability(BaseModel):
    id: str
    label: str
    type_id: str
    exchanges: list[str]
    date_range: bool = False
    symbol_required: bool = False
    description: str


class ProviderManifest(BaseModel):
    id: str
    name: str
    version: str
    api_version: int = 1
    capabilities: list[Capability]


class Partition(BaseModel):
    model_config = ConfigDict(extra="forbid")
    api: str
    params: dict[str, str]
    fields: list[str]
    limit: int


class Provider(Protocol):
    manifest: ProviderManifest

    def plan(self, request: SyncRequest) -> list[Partition]: ...
    def probe(self, credential: str) -> str: ...
    def fetch(self, partition: Partition, credential: str) -> list[dict[str, Any]]: ...
    def normalize(self, request: SyncRequest, rows: list[dict]) -> list[dict]: ...


class ProviderError(ValueError):
    """Safe, user-facing errors; never include upstream bodies or credentials."""
