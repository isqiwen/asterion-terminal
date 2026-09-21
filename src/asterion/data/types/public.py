"""Source-independent data semantics. Transport adapters map into these contracts."""

from collections.abc import Callable
from datetime import date
from typing import Literal

from pydantic import BaseModel, Field

from asterion.data.providers.public import ProviderError
from asterion.platform.registry import Registry


class DataField(BaseModel):
    name: str
    label: str
    unit: str | None = None


class TypeManifest(BaseModel):
    id: str
    label: str
    domain: str
    domain_label: str
    shape: Literal["table", "timeseries", "document", "graph", "artifact"]
    schema_version: int = 1
    api_version: int = 1
    frequency: str
    primary_key: list[str]
    time_field: str | None = None
    time_semantics: str
    fields: list[DataField] = Field(default_factory=list)
    description: str


class DataType:
    def __init__(
        self,
        manifest: TypeManifest,
        validate: Callable[[list[dict]], None],
        coverage: Callable | None = None,
        chart: Callable | None = None,
    ):
        self.manifest = manifest
        self._validate = validate
        self._coverage = coverage
        self.chart = chart

    def coverage(self, rows: list[dict], start: date | None, end: date | None):
        return self._coverage(rows, start, end) if self._coverage else "RETURNED_ROWS_ONLY"

    def validate(self, rows: list[dict]):
        self._validate(rows)
        keys = [tuple(str(row[k]) for k in self.manifest.primary_key) for row in rows]
        if len(keys) != len(set(keys)):
            raise ProviderError("返回了重复数据键，未发布")


class TypeRegistry:
    def __init__(self, plugins: tuple[DataType, ...] = ()):
        self._plugins: Registry[DataType] = Registry()
        for plugin in plugins:
            self.register(plugin)

    def register(self, plugin: DataType):
        if plugin.manifest.api_version != 1:
            raise ValueError("Unsupported data type contract")
        self._plugins.register(plugin.manifest.id, plugin)

    def get(self, identifier: str) -> DataType:
        try:
            return self._plugins.get(identifier)
        except KeyError:
            raise ProviderError("未安装该数据类型插件") from None

    def all(self):
        return list(self._plugins.all())
