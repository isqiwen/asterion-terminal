"""Account-protected data UI API; lease-protected worker publication API."""

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request
from pydantic import BaseModel, Field

from asterion.data.configuration import (
    ConfigurationCheck,
    ConfigurationState,
    ConfigurationUpdate,
    VerificationState,
)
from asterion.data.connections import ConnectionState, ConnectionUpdate, NewConnection
from asterion.data.coverage import (
    CoverageReport,
    CoverageRequest,
    RefillRequest,
    RefillResult,
    RefillTracking,
)
from asterion.data.ingestion import Observation, ObservationPage, ObservationPreview
from asterion.data.lifecycle import ArchiveRequest, VersionLifecycle
from asterion.data.preparation import Preparation
from asterion.data.providers.public import ProviderManifest, SyncRequest
from asterion.data.sync_admission import SyncSubmission, admit
from asterion.data.types.public import TypeManifest
from asterion.platform.tasks.public import Job


class ProviderStatus(ProviderManifest):
    lifecycle: ConnectionState
    verification: VerificationState
    plugin_id: str | None = None
    connection_id: str | None = None
    configured: bool
    credential_error: str | None = None


class Retry(BaseModel):
    resume: bool = False
    command_id: str = Field(min_length=1, max_length=100)


class Progress(BaseModel):
    token: str
    completed: int
    total: int


def router(sync, account_access, reference_readers=()):
    routes = APIRouter(prefix="/api/v1")
    access = [Depends(account_access)]
    lifecycle = VersionLifecycle(sync.engine, reference_readers)

    @routes.get("/data/versions/{version_id}/lifecycle", dependencies=access)
    def version_lifecycle(version_id: str):
        try:
            return lifecycle.inspect(version_id)
        except KeyError:
            raise HTTPException(404, "数据版本不存在") from None

    @routes.post("/data/versions/{version_id}/archive", dependencies=access)
    def archive_version(version_id: str, body: ArchiveRequest):
        try:
            return lifecycle.archive(version_id, body)
        except KeyError:
            raise HTTPException(404, "数据版本不存在") from None

    @routes.get("/data/providers", response_model=list[ProviderStatus], dependencies=access)
    def providers():
        return sync.providers()

    @routes.post("/data/connections", status_code=201, dependencies=access)
    def create_connection(body: NewConnection):
        return sync.connections.create(body)

    @routes.post(
        "/data/connections/{identifier}", response_model=ConnectionState, dependencies=access
    )
    def update_connection(identifier: str, body: ConnectionUpdate):
        return sync.connections.update(identifier, body)

    @routes.post(
        "/data/providers/{provider}/verify", response_model=VerificationState, dependencies=access
    )
    def verify_saved(provider: str):
        if sync.connections.state(provider).state != "enabled":
            raise ValueError("连接已停用或归档，请先恢复连接")
        return sync.configuration.verify_saved(provider)

    @routes.get(
        "/data/providers/{provider}/configuration",
        response_model=ConfigurationState,
        dependencies=access,
    )
    def configuration(provider: str):
        return sync.configuration.state(provider)

    @routes.post(
        "/data/providers/{provider}/configuration",
        response_model=ConfigurationState,
        dependencies=access,
    )
    def apply_configuration(provider: str, body: ConfigurationUpdate):
        return sync.configuration.apply(provider, body)

    @routes.post(
        "/data/providers/{provider}/configuration/check",
        response_model=ConfigurationCheck,
        dependencies=access,
    )
    def check_configuration(provider: str, body: ConfigurationUpdate):
        return sync.configuration.check(provider, body)

    @routes.post(
        "/data/preparations", response_model=Preparation, status_code=202, dependencies=access
    )
    def prepare_research(body: SyncRequest):
        return sync.preparations.submit(body)

    @routes.get("/data/preparations", response_model=list[Preparation], dependencies=access)
    def preparations():
        return sync.preparations.list()

    @routes.get("/data/preparations/{identifier}", response_model=Preparation, dependencies=access)
    def preparation(identifier: str):
        try:
            return sync.preparations.get(identifier)
        except KeyError:
            raise HTTPException(404, "研究数据准备记录不存在") from None

    @routes.post("/data/sync", status_code=202, response_model=Job, dependencies=access)
    def submit(body: SyncSubmission):
        return admit(sync, body)

    @routes.post(
        "/data/jobs/{job_id}/retry", status_code=202, response_model=Job, dependencies=access
    )
    def retry(job_id: str, body: Retry):
        return sync.retry(job_id, body.command_id, body.resume)

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

    @routes.post("/jobs/{job_id}/resume")
    def resume(job_id: str, x_lease_token: str = Header()):
        return sync.evidence.resume(job_id, x_lease_token)

    @routes.post("/jobs/{job_id}/observations/{index}")
    async def observation(job_id: str, index: int, request: Request, x_lease_token: str = Header()):
        content = bytearray()
        async for chunk in request.stream():
            content.extend(chunk)
            if len(content) > 8_000_000:
                raise HTTPException(413, "分段证据超过 8 MB")
        from starlette.concurrency import run_in_threadpool

        try:
            value = Observation.model_validate_json(bytes(content))
        except ValueError:
            raise HTTPException(422, "分段证据格式错误") from None
        return await run_in_threadpool(sync.evidence.record, job_id, x_lease_token, index, value)

    @routes.get(
        "/data/jobs/{job_id}/observations", response_model=ObservationPage, dependencies=access
    )
    def observations(
        job_id: str, offset: int = Query(0, ge=0), limit: int = Query(50, ge=1, le=100)
    ):
        try:
            return sync.evidence.list(job_id, offset, limit)
        except KeyError:
            raise HTTPException(404, "采集任务不存在") from None

    @routes.get(
        "/data/jobs/{job_id}/observations/{attempt}/{index}",
        response_model=ObservationPreview,
        dependencies=access,
    )
    def observation_preview(
        job_id: str,
        attempt: int,
        index: int,
        offset: int = Query(0, ge=0),
        limit: int = Query(100, ge=1, le=500),
    ):
        try:
            return sync.evidence.preview(job_id, attempt, index, offset, limit)
        except KeyError:
            raise HTTPException(404, "采集证据不存在") from None

    @routes.post(
        "/data/versions/{version_id}/coverage", response_model=CoverageReport, dependencies=access
    )
    def check_coverage(version_id: str, body: CoverageRequest):
        try:
            return sync.coverage.check(version_id, body)
        except KeyError:
            raise HTTPException(404, "核对输入版本不存在") from None

    @routes.get(
        "/data/versions/{version_id}/coverage",
        response_model=CoverageReport | None,
        dependencies=access,
    )
    def latest_coverage(version_id: str):
        try:
            return sync.coverage.latest(version_id)
        except KeyError:
            raise HTTPException(404, "日线版本不存在") from None

    @routes.get("/data/coverage/{report_id}", response_model=CoverageReport, dependencies=access)
    def coverage_report(report_id: str):
        try:
            return sync.coverage.get(report_id)
        except KeyError:
            raise HTTPException(404, "覆盖报告不存在") from None

    @routes.get(
        "/data/coverage/{report_id}/refill-status",
        response_model=RefillTracking,
        dependencies=access,
    )
    def refill_status(report_id: str):
        try:
            return sync.coverage.tracking(report_id)
        except KeyError:
            raise HTTPException(404, "覆盖报告不存在") from None

    @routes.post(
        "/data/coverage/{report_id}/refill", response_model=RefillResult, dependencies=access
    )
    def refill(report_id: str, body: RefillRequest):
        try:
            return sync.coverage.refill(report_id, body.command_id)
        except KeyError:
            raise HTTPException(404, "覆盖报告或核对版本不存在") from None

    @routes.get("/data/types", response_model=list[TypeManifest], dependencies=access)
    def data_types():
        return [p.manifest.model_dump() for p in sync.library.types.all()]

    @routes.get("/data/catalog", dependencies=access)
    def catalog(
        include_archived: bool = False,
        domain: str = "",
        type_id: str = "",
        source: str = "",
        layer: str = "",
        search: str = Query("", max_length=200),
        offset: int = Query(0, ge=0),
        limit: int = Query(50, ge=1, le=100),
    ):
        return sync.library.list(
            include_archived=include_archived,
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
