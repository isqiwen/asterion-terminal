"""Internal HTTP host behind the Rust entry: forwarded authorization, errors and plugins.

Browsers never reach this process directly. CORS, accounts, the task queue
operations, event replay and scope credentials belong to `asterion-server`.
"""

import time
from contextlib import ExitStack, asynccontextmanager

from asterion_bindings.plugin_host import PluginHost
from asterion_bindings.task_repository import Conflict, Tasks
from fastapi import Depends, FastAPI, HTTPException, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from asterion.api.schema import ErrorResponse
from asterion.platform.config import Settings
from asterion.platform.diagnostics import ServiceReport, local_services
from asterion.platform.store import database


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
    from asterion_bindings.task_models import TASK_CHANGED

    from asterion.platform.communication.events import EventJournal

    topics = (TASK_CHANGED, *(topic for plugin in host.plugins for topic in plugin.publishes))
    journal = EventJournal(engine, topics)

    owners = ExitStack()

    @asynccontextmanager
    async def lifespan(app):
        try:
            yield
        finally:
            try:
                host.close()
            finally:
                try:
                    owners.close()
                finally:
                    if owned_engine:
                        engine.dispose()

    from dataclasses import asdict

    from asterion_bindings.authority import forwarded

    from asterion.distribution import request_policies

    policies = request_policies()
    worker_grants = host.handlers.worker_grants()
    # Request credentials are authorized by the Rust entry, which forwards only
    # authorized requests with its forwarding credential and the principal.
    # This declaration is exported to that entry (scripts/export_schema.py).
    entry_authorization = {
        "policies": {
            scope: [asdict(grant) for grant in grants] for scope, grants in policies.items()
        },
        "worker_grants": [asdict(grant) for grant in worker_grants],
        "topics": {
            topic.id: {"owner": topic.owner, "read_path": topic.read_path} for topic in topics
        },
    }
    principals = {"root", "worker", *policies}

    def authorize(request: Request):
        # Internal forwarding headers are read directly so they never appear in
        # the public API description.
        principal = request.headers.get("X-Asterion-Principal", "")
        credential = request.headers.get("X-Asterion-Forwarded", "")
        if principal not in principals or not forwarded(credential, settings.token):
            raise HTTPException(401, "请求必须经由终端入口授权")
        request.state.principal = principal

    app = FastAPI(
        title="Asterion Terminal",
        version="0.1.0",
        lifespan=lifespan,
        dependencies=[Depends(authorize)],
        responses={409: {"model": ErrorResponse}, 422: {"model": ErrorResponse}},
    )
    from asterion.platform.communication.http import CommunicationMiddleware

    app.add_middleware(CommunicationMiddleware)
    app.state.plugins = host
    app.state.settings = settings
    app.state.entry_authorization = entry_authorization

    @app.exception_handler(RequestValidationError)
    async def invalid_request(_, exc):
        return JSONResponse(
            status_code=422, content={"detail": "请求参数格式不正确", "code": "INVALID_INPUT"}
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

    try:
        if resources is None:
            from asterion.distribution import bootstrap_resources

            resources = bootstrap_resources(settings, engine, tasks, host.plugins, owners)
        from asterion_bindings.storage import initialize_stores

        from asterion.platform.communication.schema import CORE_TABLES

        initialize_stores(engine, resources, CORE_TABLES)
        host.activate(app, resources, events=journal)
        guards = host.hooks("access")
        if len(guards) != 1:
            raise ValueError("Exactly one access policy plugin is required")
    except BaseException:
        try:
            host.close()
        finally:
            try:
                owners.close()
            finally:
                if owned_engine:
                    engine.dispose()
        raise
    account_access = guards[0]

    @app.get(
        "/api/v1/services", response_model=ServiceReport, dependencies=[Depends(account_access)]
    )
    def service_status():
        services = local_services(engine, settings.data_root)
        ready = next(item for item in services if item.id == "database").state == "ready"
        for probe in host.hooks("services"):
            services.extend(probe(ready))
        return ServiceReport(checked_at=time.time(), services=services)

    return app
