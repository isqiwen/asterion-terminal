"""Account-protected data UI API; lease-protected worker publication API."""

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request
from pydantic import BaseModel, Field, SecretStr

from asterion.api.schema import Job
from asterion.data.providers.public import ProviderManifest, SyncRequest
from asterion.data.types.public import TypeManifest


class ProviderStatus(ProviderManifest):
    configured: bool
    credential_error: str | None = None


class Retry(BaseModel):
    command_id: str = Field(min_length=1, max_length=100)


class CredentialUpdate(BaseModel):
    token: SecretStr


class Progress(BaseModel):
    token: str
    completed: int
    total: int


def router(sync, account_access):
    routes = APIRouter(prefix="/api/v1")
    access = [Depends(account_access)]

    @routes.get("/data/providers", response_model=list[ProviderStatus], dependencies=access)
    def providers():
        return sync.providers()

    @routes.post("/data/providers/{provider}/credential", dependencies=access)
    def configure(provider: str, body: CredentialUpdate):
        sync.registry.get(provider)
        value = body.token.get_secret_value().strip()
        if len(value) > 256 or any(c.isspace() for c in value):
            raise ValueError("Token 格式不正确")
        sync.credentials.save(provider, value)
        return {"configured": bool(value)}

    @routes.post("/data/providers/{provider}/check", dependencies=access)
    def check(provider: str):
        plugin = sync.registry.get(provider)
        message = plugin.probe(sync.credentials.read(provider))
        return {"status": "verified", "message": message}

    @routes.post("/data/sync", status_code=202, response_model=Job, dependencies=access)
    def submit(body: SyncRequest):
        return sync.submit(body)

    @routes.post(
        "/data/jobs/{job_id}/retry", status_code=202, response_model=Job, dependencies=access
    )
    def retry(job_id: str, body: Retry):
        return sync.retry(job_id, body.command_id)

    @routes.post("/jobs/{job_id}/progress")
    def progress(job_id: str, body: Progress):
        sync.progress(job_id, body.token, body.completed, body.total)
        return {"status": "updated"}

    @routes.post("/jobs/{job_id}/publish-data")
    async def publish(job_id: str, request: Request, x_lease_token: str = Header()):
        content = bytearray()
        async for chunk in request.stream():
            content.extend(chunk)
            if len(content) > 8_000_000:
                raise HTTPException(413, "本次同步数据超过 8 MB")
        from starlette.concurrency import run_in_threadpool

        try:
            return await run_in_threadpool(sync.publish, job_id, x_lease_token, bytes(content))
        except (KeyError, TypeError):
            raise HTTPException(422, "同步证据格式错误") from None

    @routes.get("/data/types", response_model=list[TypeManifest], dependencies=access)
    def data_types():
        return [p.manifest.model_dump() for p in sync.library.types.all()]

    @routes.get("/data/catalog", dependencies=access)
    def catalog(
        domain: str = "",
        type_id: str = "",
        source: str = "",
        layer: str = "",
        search: str = Query("", max_length=200),
        offset: int = Query(0, ge=0),
        limit: int = Query(50, ge=1, le=100),
    ):
        return sync.library.list(
            domain=domain,
            type_id=type_id,
            source=source,
            layer=layer,
            search=search,
            offset=offset,
            limit=limit,
        )

    @routes.get("/data/catalog/{dataset_id}/versions", dependencies=access)
    def history(
        dataset_id: str, offset: int = Query(0, ge=0), limit: int = Query(50, ge=1, le=100)
    ):
        return sync.library.history(dataset_id, offset, limit)

    @routes.get("/data/versions/{version_id}", dependencies=access)
    def preview(
        version_id: str, offset: int = Query(0, ge=0), limit: int = Query(100, ge=1, le=500)
    ):
        try:
            return sync.library.preview(version_id, offset, limit)
        except KeyError:
            raise HTTPException(404, "数据版本不存在") from None

    return routes
