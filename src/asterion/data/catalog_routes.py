"""Data-owned HTTP contributions, including immutable publication."""

from typing import Annotated

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request

from asterion.data.importing import ImportPreview, encode_import, preview
from asterion.data.public import Bar, ImportRequest, Snapshot, read_bars
from asterion.data.reference import ContractResolution, ReferenceCatalog, ResolutionRequest
from asterion.data.reference_source import SourceCatalogRequest, source_catalog
from asterion.data.reference_store import ReferenceRelease, ReferenceSummary
from asterion.platform.tasks.public import Job


def catalog_router(data, reference, tasks, account_access, version_reader):
    routes = APIRouter()

    @routes.post(
        "/api/v1/imports",
        status_code=202,
        response_model=Job,
        dependencies=[Depends(account_access)],
    )
    def import_csv(body: ImportRequest):
        body.options.identity.validate_inputs(data.library.preview)
        encode_import(body.model_dump())
        return tasks.submit(
            body.command_id,
            "data.import_csv",
            body.model_dump(mode="json", exclude={"command_id"}),
        )

    @routes.post(
        "/api/v1/imports/preview",
        response_model=ImportPreview,
        dependencies=[Depends(account_access)],
    )
    def preview_import(body: ImportRequest):
        body.options.identity.validate_inputs(data.library.preview)
        return preview(body.csv, body.options)

    @routes.post("/api/v1/jobs/{job_id}/publish")
    async def publish(job_id: str, request: Request, x_lease_token: Annotated[str, Header()]):
        # This first CSV slice is capped. General large-file publication is a separate protocol.
        chunks = bytearray()
        async for chunk in request.stream():
            chunks.extend(chunk)
            if len(chunks) > 8_000_000:
                raise HTTPException(413, "Artifact exceeds 8 MB")
        from starlette.concurrency import run_in_threadpool

        return await run_in_threadpool(data.publish, job_id, x_lease_token, bytes(chunks))

    @routes.get(
        "/api/v1/snapshots", response_model=list[Snapshot], dependencies=[Depends(account_access)]
    )
    def list_snapshots():
        return data.list()

    @routes.get(
        "/api/v1/snapshots/{snapshot_id}/bars",
        response_model=list[Bar],
        dependencies=[Depends(account_access)],
    )
    def bars(snapshot_id: str):
        try:
            return read_bars(data.path(snapshot_id))
        except KeyError:
            raise HTTPException(404, "Published snapshot not found")

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
