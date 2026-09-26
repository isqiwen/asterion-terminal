"""Account-protected data UI API; lease-protected worker publication API."""

from datetime import date
from uuid import UUID

from asterion_bindings.task_models import Job
from fastapi import APIRouter, Depends, Header, HTTPException, Request
from pydantic import BaseModel, Field

from asterion.data.coverage import (
    CoverageReport,
    CoverageRequest,
    RefillRequest,
    RefillResult,
    RefillTracking,
)
from asterion.data.history import (
    HistoryBatch,
    HistoryCoverage,
    HistoryDownloads,
    HistoryPlan,
    HistoryRequest,
    HistorySummary,
)
from asterion.data.minute import MinuteCoverage
from asterion.data.minute import coverage as minute_coverage
from asterion.data.preparation import Preparation
from asterion.data.providers.public import SyncRequest


class Retry(BaseModel):
    resume: bool = False
    command_id: str = Field(min_length=1, max_length=100)


def router(sync, account_access):
    routes = APIRouter(prefix="/api/v1")
    access = [Depends(account_access)]

    history_downloads = HistoryDownloads(sync)

    @routes.get("/data/history", response_model=list[HistorySummary], dependencies=access)
    def history_recent():
        return history_downloads.recent()

    @routes.post("/data/history/plan", response_model=HistoryPlan, dependencies=access)
    def history_plan(body: HistoryRequest):
        try:
            return history_downloads.plan(body)
        except KeyError:
            raise HTTPException(404, "历史计划依据不存在") from None

    @routes.post("/data/history", response_model=HistoryBatch, status_code=202, dependencies=access)
    def history_submit(body: HistoryRequest):
        try:
            return history_downloads.submit(body)
        except KeyError:
            raise HTTPException(404, "历史计划依据不存在") from None

    @routes.get("/data/history/{identifier}", response_model=HistoryBatch, dependencies=access)
    def history_status(identifier: UUID):
        try:
            return history_downloads.get(identifier)
        except KeyError:
            raise HTTPException(404, "历史下载计划不存在") from None

    @routes.post(
        "/data/history/{identifier}/coverage", response_model=HistoryCoverage, dependencies=access
    )
    def history_coverage(identifier: UUID):
        try:
            return history_downloads.coverage(identifier)
        except KeyError:
            raise HTTPException(404, "历史下载计划不存在") from None

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

    @routes.post(
        "/data/jobs/{job_id}/retry", status_code=202, response_model=Job, dependencies=access
    )
    def retry(job_id: str, body: Retry):
        return sync.retry(job_id, body.command_id, body.resume)

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

    @routes.post(
        "/data/versions/{version_id}/minute-coverage",
        response_model=MinuteCoverage,
        dependencies=access,
    )
    def check_minute_coverage(version_id: str, trading_day: date):
        try:
            return minute_coverage(sync, version_id, trading_day)
        except KeyError:
            raise HTTPException(404, "分钟版本不存在") from None

    return routes
