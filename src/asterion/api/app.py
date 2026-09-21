"""HTTP host: transport authentication, errors, tasks and plugin lifecycle."""

import time
from contextlib import asynccontextmanager
from typing import Annotated

from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

from asterion.api.schema import ErrorResponse
from asterion.platform.config import Settings
from asterion.platform.diagnostics import ServiceReport, local_services
from asterion.platform.plugins import PluginHost
from asterion.platform.store import database, jobs
from asterion.platform.tasks.public import ClaimedJob, Job
from asterion.platform.tasks.service import Conflict, Tasks


class Claim(BaseModel):
    worker_id: str = Field(min_length=1, max_length=100)


class Lease(BaseModel):
    token: str


class Failure(Lease):
    error: str


def create_app(settings: Settings | None = None, engine=None, *, plugins=None, resources=None):
    settings = settings or Settings()
    settings.require_token()
    owned_engine = engine is None
    engine = engine if engine is not None else database(settings.database_url)
    tasks = Tasks(engine, settings.lease_seconds)
    if plugins is None:
        from asterion.distribution import builtin_plugins

        plugins = builtin_plugins()
    host = PluginHost(tuple(plugins))

    @asynccontextmanager
    async def lifespan(app):
        try:
            yield
        finally:
            try:
                host.close()
            finally:
                if owned_engine:
                    engine.dispose()

    from asterion.distribution import request_policies
    from asterion.platform.authorization import Authority, Grant

    worker_grants = [
        Grant("/jobs/claim", ("POST",)),
        Grant("/jobs/:id/heartbeat", ("POST",)),
        Grant("/jobs/:id/fail", ("POST",)),
    ]
    for plugin in host.plugins:
        for handler in plugin.handlers:
            worker_grants.append(Grant("/jobs/:id" + handler.publish_suffix, ("POST",)))
            worker_grants.extend(handler.requests)
    authority = Authority(settings.token, request_policies(), worker_grants)

    def authorize(request: Request, authorization: Annotated[str | None, Header()] = None):
        credential = (
            authorization[7:] if authorization and authorization.startswith("Bearer ") else ""
        )
        try:
            request.state.principal = authority.authorize(
                credential,
                request.method,
                request.url.path.removeprefix("/api/v1"),
                request.headers.get("X-Account-Session", ""),
            )
        except ValueError:
            raise HTTPException(401, "请求身份无效或无权访问此接口") from None

    app = FastAPI(
        title="Asterion Terminal",
        version="0.1.0",
        lifespan=lifespan,
        dependencies=[Depends(authorize)],
        responses={409: {"model": ErrorResponse}, 422: {"model": ErrorResponse}},
    )
    app.state.plugins = host

    @app.exception_handler(RequestValidationError)
    async def invalid_request(_, exc):
        return JSONResponse(
            status_code=422, content={"detail": "请求参数格式不正确", "code": "INVALID_INPUT"}
        )

    app.add_middleware(
        CORSMiddleware,
        allow_origins=[
            "http://localhost:1420",
            "http://127.0.0.1:1420",
            "tauri://localhost",
            "http://tauri.localhost",
        ],
        allow_methods=["GET", "POST"],
        allow_headers=["Authorization", "Content-Type", "X-Lease-Token", "X-Account-Session"],
    )

    @app.exception_handler(Conflict)
    async def conflict(_, exc):
        return JSONResponse(status_code=409, content={"detail": str(exc), "code": "CONFLICT"})

    @app.exception_handler(ValueError)
    async def invalid(_, exc):
        return JSONResponse(status_code=422, content={"detail": str(exc), "code": "INVALID_INPUT"})

    @app.get("/api/v1/health")
    def health():
        from sqlalchemy import text

        with engine.connect() as conn:
            conn.execute(text("SELECT 1"))
        return {"status": "ready", "version": "0.1.0"}

    if resources is None:
        from asterion.distribution import bootstrap_resources

        resources = bootstrap_resources(settings, engine, tasks, host.plugins)
    from asterion.platform.storage import initialize_stores

    initialize_stores(engine, resources, (jobs,))
    host.activate(app, resources)
    guards = host.hooks("access")
    if len(guards) != 1:
        host.close()
        raise ValueError("Exactly one access policy plugin is required")
    account_access = guards[0]

    class ScopeRequest(BaseModel):
        scope: str = Field(min_length=1, max_length=80)

    @app.post("/api/v1/access/scopes", dependencies=[Depends(account_access)])
    def issue_scope(body: ScopeRequest, request: Request):
        if request.state.principal != "root":
            raise HTTPException(403, "只有可信工作台可申请功能授权")
        return authority.issue(body.scope, request.headers.get("X-Account-Session", ""))

    @app.get(
        "/api/v1/services", response_model=ServiceReport, dependencies=[Depends(account_access)]
    )
    def service_status():
        services = local_services(engine, settings.data_root)
        ready = next(item for item in services if item.id == "database").state == "ready"
        for probe in host.hooks("services"):
            services.extend(probe(ready))
        return ServiceReport(checked_at=time.time(), services=services)

    @app.get("/api/v1/jobs", response_model=list[Job], dependencies=[Depends(account_access)])
    def list_jobs():
        return tasks.list()

    @app.get("/api/v1/jobs/{job_id}", response_model=Job, dependencies=[Depends(account_access)])
    def get_job(job_id: str):
        try:
            return tasks.get(job_id)
        except KeyError:
            raise HTTPException(404, "任务不存在") from None

    @app.post("/api/v1/jobs/claim", response_model=ClaimedJob | None)
    def claim(body: Claim):
        return tasks.claim(body.worker_id)

    @app.post("/api/v1/jobs/{job_id}/heartbeat")
    def heartbeat(job_id: str, body: Lease):
        tasks.heartbeat(job_id, body.token)
        return {"status": "renewed"}

    @app.post("/api/v1/jobs/{job_id}/fail")
    def fail(job_id: str, body: Failure):
        tasks.fail(job_id, body.token, body.error)
        return {"status": "failed"}

    @app.post("/api/v1/jobs/{job_id}/cancel", dependencies=[Depends(account_access)])
    def cancel(job_id: str):
        tasks.cancel(job_id)
        return {"status": "cancelled"}

    return app
