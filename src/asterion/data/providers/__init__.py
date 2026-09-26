"""Explicit registration of trusted provider contributions."""

from asterion_bindings import data_providers

from asterion.data.providers.public import Provider, ProviderError
from asterion.platform.registry import Registry


class ValidatedProvider:
    """The same admission and plan contract applies to built-in and process-backed providers."""

    def __init__(self, implementation):
        self._implementation = implementation
        self.manifest = implementation.manifest
        self.digest = getattr(implementation, "digest", None)

    def plan(self, request):
        parts = self._implementation.plan(request)
        if not isinstance(parts, list):
            raise ProviderError("采集计划为空或超出限制")
        try:
            data_providers.check_plan(
                request.model_dump_json(), [part.model_dump() for part in parts]
            )
        except ValueError as error:
            raise ProviderError(str(error)) from None
        return parts

    def probe(self, configuration):
        return self._implementation.probe(configuration)

    def fetch(self, partition, configuration):
        return self._implementation.fetch(partition, configuration)

    def normalize(self, request, rows):
        return self._implementation.normalize(request, rows)


class ProviderRegistry:
    def __init__(self, providers: tuple[Provider, ...] = ()):
        self._providers: Registry[Provider] = Registry()
        for provider in providers:
            self.register(provider)

    def register(self, provider: Provider):
        manifest = provider.manifest
        if manifest.api_version != 2:
            raise ValueError("Unsupported provider contract")
        self._providers.register(manifest.id, ValidatedProvider(provider))

    def get(self, identifier: str) -> Provider:
        try:
            return self._providers.get(identifier)
        except KeyError:
            raise ProviderError("不支持该数据源；数据源只能是内置实现") from None

    def all(self):
        return list(self._providers.all())


def builtin_registry():
    """Data sources are built-in implementations only; none are installable."""
    from asterion.data.providers.tushare import Tushare

    return ProviderRegistry((Tushare(),))
