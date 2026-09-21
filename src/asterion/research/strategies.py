"""Public, trusted strategy contract: streaming closed bars, long/flat intentions only."""

from collections.abc import Callable
from dataclasses import dataclass
from datetime import date
from decimal import Decimal
from typing import Literal, Protocol

from pydantic import BaseModel, ConfigDict, Field

from asterion.platform.plugins import Activation, Capability, Plugin
from asterion.platform.resource import Resource
from asterion.research.parameters import Parameter, ParameterValue, validate_parameters


class StrategyRef(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    id: str = Field(pattern=r"^[a-z][a-z0-9_.-]+$", max_length=100)
    version: str = Field(min_length=1, max_length=50)
    digest: str = Field(pattern=r"^[a-f0-9]{64}$")


class StrategyInfo(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    identity: StrategyRef
    name: str
    description: str
    data_type: Literal["futures.daily"] = "futures.daily"
    execution: Literal["long-flat-next-open"] = "long-flat-next-open"
    parameters: tuple[Parameter, ...]


@dataclass(frozen=True)
class ClosedBar:
    trading_day: date
    close: Decimal


class StrategySession(Protocol):
    def on_close(self, bar: ClosedBar) -> bool: ...

    def close(self) -> None: ...


@dataclass(frozen=True)
class Strategy:
    info: StrategyInfo
    validate: Callable[[dict[str, ParameterValue]], None]
    warmup: Callable[[dict[str, ParameterValue]], int]
    create: Callable[[dict[str, ParameterValue]], StrategySession]

    def parameters(self, values: dict[str, ParameterValue]) -> dict[str, ParameterValue]:
        validate_parameters(self.info.parameters, values)
        self.validate(dict(values))
        return dict(values)

    def required_bars(self, values: dict[str, ParameterValue]) -> int:
        count = self.warmup(self.parameters(values))
        if type(count) is not int or not 1 <= count <= 4999:
            raise ValueError("策略预热声明无效")
        return count


class StrategyUnavailable(ValueError):
    """The exact declared implementation is not available in this distribution."""


class StrategyCatalog:
    def __init__(self, strategies: tuple[Strategy, ...], installed=lambda: ()):
        self._installed = installed
        self._items = {}
        for strategy in strategies:
            info = strategy.info
            if info.identity.id in self._items:
                raise ValueError("重复策略标识")
            if len({p.key for p in info.parameters}) != len(info.parameters):
                raise ValueError("重复策略参数")
            strategy.required_bars({p.key: p.default for p in info.parameters})
            self._items[info.identity.id] = strategy

    def _all(self):
        items = dict(self._items)
        for strategy in self._installed():
            if strategy.info.identity.id in items:
                raise ValueError("重复策略标识")
            items[strategy.info.identity.id] = strategy
        return items

    def list(self) -> list[StrategyInfo]:
        return [s.info for s in self._all().values()]

    def resolve(self, identity: StrategyRef) -> Strategy:
        item = self._all().get(identity.id)
        if item is None or item.info.identity != identity:
            raise StrategyUnavailable("策略未安装，或策略版本、实现摘要不匹配")
        return item


STRATEGIES = Capability("research.strategies", "asterion.strategy_catalog", StrategyCatalog)
STRATEGY_RESOURCE = Resource("research.strategies", StrategyCatalog)


def catalog_plugin(capabilities: tuple[Capability[Strategy], ...], *, installed=False) -> Plugin:
    from asterion.extensions.public import PACKAGES
    from asterion.research.external import installed_strategies

    def activate(context):
        packages = context.resource(PACKAGES) if installed else None
        return Activation(
            exports={
                STRATEGIES: StrategyCatalog(
                    tuple(context.require(c) for c in capabilities),
                    (lambda: installed_strategies(packages)) if packages else (lambda: ()),
                )
            }
        )

    return Plugin(
        "asterion.strategy_catalog",
        tuple(c.provider for c in capabilities),
        activate,
        resources=(PACKAGES,) if installed else (),
        provides=(STRATEGIES,),
        consumes=capabilities,
    )
