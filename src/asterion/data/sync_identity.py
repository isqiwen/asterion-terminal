"""Data-plugin identity checks shared by task admission and worker execution."""

from asterion.data.providers.public import SyncRequest
from asterion.data.reference import SourceIdentity

IDENTITY_TYPES = frozenset({"futures.daily", "futures.settlement"})


def task_identity(provider, payload):
    request = SyncRequest.model_validate(payload["request"])
    provider.plan(request)
    capability = next(c for c in provider.manifest.capabilities if c.id == request.dataset)
    if payload["type_id"] != capability.type_id:
        raise ValueError("同步任务的数据类型与供应商能力不一致")
    required = capability.type_id in IDENTITY_TYPES
    if not required:
        if payload.get("contract_identity") is not None:
            raise ValueError("此同步数据类型不接受合约身份依据")
        return None
    identity = SourceIdentity.model_validate(payload.get("contract_identity"))
    if identity.source != request.provider or identity.symbol != request.symbol:
        raise ValueError("同步来源代码与固定合约身份不一致")
    if request.start is None or request.end is None:
        raise ValueError("合约数据同步须明确起止交易日")
    first, last = identity.resolve(request.start), identity.resolve(request.end)
    if first.id != last.id:
        raise ValueError("一次同步不能跨越多个实际合约生命周期")
    if not first.product_id.startswith(request.exchange + "."):
        raise ValueError("固定合约身份与请求交易所不一致")
    return identity
