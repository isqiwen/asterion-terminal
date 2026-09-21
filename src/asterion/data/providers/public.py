"""Versioned provider boundary. Plugins own transport and mapping, never catalogue writes."""

from datetime import date, datetime
from typing import Any, Literal, Protocol
from zoneinfo import ZoneInfo

from pydantic import BaseModel, ConfigDict, Field, model_validator


class SyncRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    command_id: str = Field(min_length=1, max_length=100)
    provider: str = Field(pattern=r"^[a-z][a-z0-9_]{0,40}$")
    connection_id: str | None = Field(default=None, pattern=r"^c_[0-9a-f]{32}$")
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
    defaults: dict[str, str] = Field(default_factory=dict)


class ConfigurationField(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,40}$")
    label: str
    type: Literal["string", "integer", "boolean"] = "string"
    secret: bool = False
    required: bool = False
    default: str | int | bool | None = None
    description: str = ""
    placeholder: str = ""
    min_length: int | None = Field(default=None, ge=0)
    max_length: int | None = Field(default=None, ge=1, le=4096)
    minimum: int | None = None
    maximum: int | None = None

    @model_validator(mode="after")
    def safe_schema(self):
        if self.secret and (self.type != "string" or self.default is not None):
            raise ValueError("Secret fields must be strings without defaults")
        if self.minimum is not None and self.maximum is not None and self.minimum > self.maximum:
            raise ValueError("Invalid numeric bounds")
        if (
            self.min_length is not None
            and self.max_length is not None
            and self.min_length > self.max_length
        ):
            raise ValueError("Invalid string bounds")
        return self


class ConfigurationSpec(BaseModel):
    model_config = ConfigDict(extra="forbid")
    schema_version: int = Field(default=1, ge=1)
    fields: list[ConfigurationField] = Field(default_factory=list, max_length=30)

    @model_validator(mode="after")
    def unique_fields(self):
        if len({field.id for field in self.fields}) != len(self.fields):
            raise ValueError("Duplicate configuration field")
        return self


class ProviderManifest(BaseModel):
    id: str
    name: str
    version: str
    api_version: int = 2
    capabilities: list[Capability]
    configuration: ConfigurationSpec = Field(default_factory=ConfigurationSpec)
    description: str = ""
    demo: bool = False


class Partition(BaseModel):
    model_config = ConfigDict(extra="forbid")
    api: str
    params: dict[str, str]
    fields: list[str]
    limit: int = Field(ge=1, le=100_001)
    start: str | None = None
    end: str | None = None

    @model_validator(mode="after")
    def window(self):
        if (self.start is None) != (self.end is None):
            raise ValueError("Partition dates must be paired")
        if (
            self.start is not None
            and self.end is not None
            and date.fromisoformat(self.start) > date.fromisoformat(self.end)
        ):
            raise ValueError("Invalid partition window")
        if not self.fields or len(self.fields) != len(set(self.fields)):
            raise ValueError("Partition fields must be nonempty and unique")
        return self


class Provider(Protocol):
    manifest: ProviderManifest

    def plan(self, request: SyncRequest) -> list[Partition]: ...
    def probe(self, configuration: dict) -> str: ...
    def fetch(self, partition: Partition, configuration: dict) -> list[dict[str, Any]]: ...
    def normalize(self, request: SyncRequest, rows: list[dict]) -> list[dict]: ...


class ProviderError(ValueError):
    """Safe, user-facing errors; never include upstream bodies or credentials."""
