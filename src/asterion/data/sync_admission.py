"""User-facing sync admission; supplier requests contain no host identity evidence."""

from pydantic import Field

from asterion.data.providers.public import SyncRequest
from asterion.data.reference import SourceIdentity
from asterion.data.reference_source import SourceCatalogRequest, source_catalog
from asterion.data.reference_store import catalog_digest
from asterion.data.sync_identity import IDENTITY_TYPES


class SyncSubmission(SyncRequest):
    contracts_version_id: str | None = Field(default=None, min_length=1, max_length=100)


def prepare(sync, submission: SyncSubmission):
    request = SyncRequest.model_validate(submission.model_dump(exclude={"contracts_version_id"}))
    provider = sync.registry.get(request.provider)
    provider.plan(request)
    capability = next(c for c in provider.manifest.capabilities if c.id == request.dataset)
    if capability.type_id not in IDENTITY_TYPES:
        if submission.contracts_version_id is not None:
            raise ValueError("此数据类型不接受合约身份依据")
        return request, None
    if submission.contracts_version_id is None:
        raise ValueError("合约数据同步须选择固定合约资料版本")
    catalog = source_catalog(
        sync.library.preview,
        SourceCatalogRequest(version_id=submission.contracts_version_id, symbols=[request.symbol]),
    )
    identity = SourceIdentity(
        catalog_id=catalog_digest(catalog),
        catalog=catalog,
        source=request.provider,
        symbol=request.symbol,
        information_at=max(item.provenance.available_at for item in catalog.contracts),
    )
    # One current admission contract applies to all contract-bound source types.
    return request, identity.model_dump(mode="json")


def admit(sync, submission: SyncSubmission):
    request, identity = prepare(sync, submission)
    return sync.submit(request, contract_identity=identity)
