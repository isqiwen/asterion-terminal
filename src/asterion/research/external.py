"""Research-owned external strategy binding; no future bars cross the process boundary."""

from typing import Literal

from pydantic import BaseModel, ConfigDict

from asterion.platform.extensions.process import PackageSession, call_package
from asterion.research.parameters import Parameter
from asterion.research.strategies import (
    ClosedBar,
    Strategy,
    StrategyInfo,
    StrategyRef,
    StrategyUnavailable,
)


class Declaration(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str
    name: str
    description: str
    data_type: Literal["futures.daily"]
    execution: Literal["long-flat-next-open"]
    dependencies: Literal["stdlib-and-sdk"]
    parameters: tuple[Parameter, ...]


def validate_contribution(package, value):
    declaration = Declaration.model_validate(value)
    if declaration.id != package.id or package.id.startswith("builtin."):
        raise ValueError("策略标识必须等于插件标识，且不得占用内置命名空间")
    if package.requires:
        raise ValueError("策略包仅支持标准库、公共 SDK 和包内源码依赖，不支持其他已安装插件依赖")
    keys = [p.key for p in declaration.parameters]
    if len(keys) != len(set(keys)) or len(keys) > 32:
        raise ValueError("策略参数重复或超出 32 项")


def artifact(packages, identity):
    manifest, path = packages.resolve(identity.digest)
    if "research.strategy" not in manifest.contributions:
        raise StrategyUnavailable("固定工件未声明策略")
    value = manifest.contributions["research.strategy"]
    validate_contribution(manifest, value)
    declaration = Declaration.model_validate(value)
    if manifest.id != identity.id or manifest.version != identity.version:
        raise StrategyUnavailable("策略与固定插件工件身份不一致")
    info = StrategyInfo(identity=identity, **declaration.model_dump(exclude={"id", "dependencies"}))
    return info, path


class ExternalSession:
    def __init__(self, owner, parameters):
        self.owner = owner
        self.session = PackageSession(owner.path(), authorized=owner.authorized)
        try:
            if self.session.call("strategy.open", {"parameters": parameters}) is not None:
                raise ValueError("策略会话初始化响应无效")
        except BaseException:
            self.session.close()
            raise

    def on_close(self, bar: ClosedBar) -> bool:
        value = self.session.call(
            "strategy.close", {"trading_day": bar.trading_day.isoformat(), "close": str(bar.close)}
        )
        if type(value) is not bool:
            raise ValueError("策略必须返回做多或空仓意图")
        return value

    def close(self):
        self.session.close()
        self.owner.path()  # Publication/replay must not accept a revoked or altered artifact.


class ExternalStrategy:
    def __init__(self, packages, record):
        self.packages = packages
        manifest = record["manifest"]
        self.identity = StrategyRef(
            id=manifest["id"], version=manifest["version"], digest=record["digest"]
        )
        self.info, _ = artifact(packages, self.identity)
        self.prepared = None

    def authorized(self):
        return self.packages.enabled(self.identity.id, self.identity.digest)

    def path(self):
        if not self.authorized():
            raise StrategyUnavailable("策略插件已停用、移除或工件已变化")
        _, path = artifact(self.packages, self.identity)
        return path

    def prepare(self, parameters):
        self.path()
        if self.prepared is None or self.prepared[0] != parameters:
            count = call_package(
                self.path(),
                "strategy.prepare",
                {"parameters": parameters},
                authorized=self.authorized,
            )
            if type(count) is not int or not 1 <= count <= 4999:
                raise ValueError("策略预热声明无效")
            self.prepared = (dict(parameters), count)
        return self.prepared[1]

    def validate(self, parameters):
        self.prepare(parameters)

    def contribution(self):
        return Strategy(self.info, self.validate, self.prepare, lambda p: ExternalSession(self, p))


def installed_strategies(packages):
    return tuple(
        ExternalStrategy(packages, record).contribution()
        for record in packages.list()
        if record["enabled"] and "research.strategy" in record["manifest"]["contributions"]
    )
