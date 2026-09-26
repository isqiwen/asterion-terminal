"""Data-owned HTTP contributions, including immutable publication."""

from asterion_bindings.catalog import (
    ContractResolution,
    ReferenceCatalog,
    ReferenceRelease,
    ResolutionRequest,
)
from fastapi import APIRouter, Depends, HTTPException, Query

from asterion.data.reference_source import (
    SourceCatalogRequest,
    source_catalog,
)
from asterion.data.reference_store import ReferenceSummary


def catalog_router(reference, account_access, version_reader):
    routes = APIRouter()

    @routes.post(
        "/api/v1/reference/source/preview",
        response_model=ReferenceCatalog,
        dependencies=[Depends(account_access)],
    )
    def reference_source_preview(body: SourceCatalogRequest):
        return source_catalog(version_reader, body)

    @routes.post(
        "/api/v1/reference/source/publish",
        response_model=ReferenceRelease,
        dependencies=[Depends(account_access)],
    )
    def reference_source_publish(body: SourceCatalogRequest):
        return reference.publish(source_catalog(version_reader, body))

    @routes.get(
        "/api/v1/reference/releases",
        response_model=list[ReferenceSummary],
        dependencies=[Depends(account_access)],
    )
    def reference_list(limit: int = Query(50, ge=1, le=100), offset: int = Query(0, ge=0)):
        return reference.list(limit, offset)

    @routes.post(
        "/api/v1/reference/releases",
        response_model=ReferenceRelease,
        dependencies=[Depends(account_access)],
    )
    def reference_publish(body: ReferenceCatalog):
        return reference.publish(body)

    @routes.get(
        "/api/v1/reference/releases/{release_id}",
        response_model=ReferenceRelease,
        dependencies=[Depends(account_access)],
    )
    def reference_get(release_id: str):
        try:
            return reference.get(release_id)
        except KeyError:
            raise HTTPException(404, "Reference release not found") from None

    @routes.post(
        "/api/v1/reference/releases/{release_id}/resolve",
        response_model=ContractResolution,
        dependencies=[Depends(account_access)],
    )
    def reference_resolve(release_id: str, body: ResolutionRequest):
        try:
            release = reference.get(release_id)
        except KeyError:
            raise HTTPException(404, "Reference release not found") from None
        return release.catalog.resolve(body)

    return routes
