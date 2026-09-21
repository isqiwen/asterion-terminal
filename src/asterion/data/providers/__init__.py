"""Explicit registration of trusted provider contributions."""

from datetime import date, timedelta

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
        if not isinstance(parts, list) or not 1 <= len(parts) <= 3661:
            raise ProviderError("采集计划为空或超出限制")
        if request.start:
            cursor = request.start
            for part in parts:
                if not part.start or not part.end or date.fromisoformat(part.start) != cursor:
                    raise ProviderError("采集分段日期不连续或不符合请求")
                end = date.fromisoformat(part.end)
                if end < cursor or end > request.end:
                    raise ProviderError("采集分段日期超出请求")
                cursor = end + timedelta(days=1)
            if cursor != request.end + timedelta(days=1):
                raise ProviderError("采集计划未覆盖完整请求范围")
        elif any(part.start or part.end for part in parts):
            raise ProviderError("快照请求不接受日期分段")
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
            raise ProviderError("未安装该数据源插件") from None

    def all(self):
        return list(self._providers.all())


def builtin_registry(root=None):
    from asterion.data.providers.tushare import Tushare

    if root is None:
        return ProviderRegistry((Tushare(),))
    from asterion.data.providers.external import ExternalProvider, validate_contribution
    from asterion.platform.extensions.packages import Packages
    from asterion.platform.extensions.views import validate_view

    packages = Packages(
        root / ".extensions", {"data.provider": validate_contribution, "ui.table": validate_view}
    )

    class InstalledProviders(ProviderRegistry):
        def all(self):
            result = super().all()
            for record in packages.list():
                if record["enabled"] and "data.provider" in record["manifest"]["contributions"]:
                    provider = ExternalProvider(packages, record)
                    if provider.manifest.id in {p.manifest.id for p in result}:
                        raise ProviderError("数据源标识冲突，请停用冲突插件")
                    result.append(ValidatedProvider(provider))
            return result

        def get(self, identifier):
            for provider in self.all():
                if provider.manifest.id == identifier:
                    return provider
            raise ProviderError("未安装或已停用该数据源插件")

    return InstalledProviders((Tushare(),))
