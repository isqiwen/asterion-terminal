"""Data-owned protocol binding. External results still pass standard publication checks."""

from asterion.data.providers.public import Partition, ProviderError, ProviderManifest, SyncRequest
from asterion.platform.extensions.process import call_package


def validate_contribution(package, value):
    manifest = ProviderManifest.model_validate(value)
    if manifest.id != package.id.replace(".", "_") or len(manifest.id) > 41:
        raise ValueError("数据源 ID 须为插件 ID 将点替换为下划线，且不超过 41 个字符")
    if manifest.api_version != 2 or manifest.demo:
        raise ValueError("正式插件必须使用当前数据源契约，不能注册合成数据")
    if manifest.version != package.version:
        raise ValueError("数据源版本必须与插件工件版本一致")
    if not manifest.capabilities or len({c.id for c in manifest.capabilities}) != len(
        manifest.capabilities
    ):
        raise ValueError("数据源能力为空或重复")
    from asterion.data.types import builtin_types

    types = builtin_types()
    for capability in manifest.capabilities:
        types.get(capability.type_id)


class ExternalProvider:
    def __init__(self, packages, record):
        self.packages = packages
        self.digest = record["digest"]
        self.manifest = ProviderManifest.model_validate(
            record["manifest"]["contributions"]["data.provider"]
        )

    def _call(self, method, params):
        try:
            package, path = self.packages.resolve(self.digest)
            validate_contribution(package, package.contributions["data.provider"])
            if (
                ProviderManifest.model_validate(package.contributions["data.provider"])
                != self.manifest
            ):
                raise ValueError("数据源声明与固定工件不一致")
            return call_package(
                path,
                method,
                params,
                authorized=lambda: self.packages.enabled(package.id, self.digest),
            )
        except (ValueError, OSError):
            raise ProviderError("插件执行失败、超时或工件校验未通过，请在插件设置中检查") from None

    def plan(self, request: SyncRequest):
        value = self._call("plan", {"request": request.model_dump(mode="json")})
        if not isinstance(value, list) or not 1 <= len(value) <= 3661:
            raise ProviderError("插件采集计划格式不正确")
        return [Partition.model_validate(item) for item in value]

    def probe(self, configuration):
        value = self._call("probe", {"configuration": configuration})
        if not isinstance(value, str) or len(value) > 1000:
            raise ProviderError("插件连接检查返回格式不正确")
        # Do not display untrusted provider output that may echo configuration secrets.
        return "插件连接检查通过"

    def fetch(self, partition, configuration):
        return self._rows(
            self._call(
                "fetch",
                {"partition": partition.model_dump(mode="json"), "configuration": configuration},
            )
        )

    def normalize(self, request, rows):
        return self._rows(
            self._call("normalize", {"request": request.model_dump(mode="json"), "rows": rows})
        )

    @staticmethod
    def _rows(value):
        if (
            not isinstance(value, list)
            or len(value) > 100_000
            or any(not isinstance(row, dict) for row in value)
        ):
            raise ProviderError("插件数据返回格式不正确")
        return value
