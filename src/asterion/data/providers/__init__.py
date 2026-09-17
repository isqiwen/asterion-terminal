"""Explicit trusted-plugin registration, compatible with the frozen desktop runtime."""

from asterion.data.providers.public import Provider, ProviderError
from asterion.data.providers.tushare import Tushare


class ProviderRegistry:
    def __init__(self, providers: tuple[Provider, ...] = ()):
        self._providers: dict[str, Provider] = {}
        for provider in providers:
            self.register(provider)

    def register(self, provider: Provider):
        manifest = provider.manifest
        if manifest.api_version != 1 or manifest.id in self._providers:
            raise ValueError("Incompatible or duplicate provider plugin")
        self._providers[manifest.id] = provider

    def get(self, identifier: str) -> Provider:
        if identifier not in self._providers:
            raise ProviderError("未安装该数据源插件")
        return self._providers[identifier]

    def all(self):
        return list(self._providers.values())


def builtin_registry():
    return ProviderRegistry((Tushare(),))
