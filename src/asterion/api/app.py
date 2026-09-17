import secrets
from typing import Annotated

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

from asterion.api.schema import ClaimedJob, Job, Snapshot
from asterion.data.public import Bar, ImportRequest, read_bars
from asterion.data.reference import ReferenceCatalog
from asterion.data.reference_store import ReferenceRelease, ReferenceStore, ReferenceSummary
from asterion.data.routes import router as data_router
from asterion.data.snapshots import Snapshots
from asterion.data.sync import DataSync
from asterion.identity.routes import router as identity_router
from asterion.identity.service import Identity, IdentityError
from asterion.platform.config import Settings
from asterion.platform.store import database
from asterion.platform.tasks.service import Conflict, Tasks


class Claim(BaseModel):
    worker_id: str = Field(min_length=1, max_length=100)


class Lease(BaseModel):
    token: str


class Failure(Lease):
    error: str


def create_app(settings: Settings | None = None, engine=None):
    settings = settings or Settings()
    settings.require_token()
    engine = engine if engine is not None else database(settings.database_url)
    tasks = Tasks(engine, settings.lease_seconds)
    data = Snapshots(engine, tasks, settings.data_root)

    def authorize(authorization: Annotated[str | None, Header()] = None):
        if not secrets.compare_digest(authorization or "", f"Bearer {settings.token}"):
            raise HTTPException(401, "Invalid session token")

    app = FastAPI(title="Asterion Terminal", version="0.1.0", dependencies=[Depends(authorize)])
    identity = Identity(
        engine, settings.data_root, settings.token, mode=settings.account_verification
    )
    reference = ReferenceStore(engine)
    app.state.identity = identity
    app.include_router(identity_router(identity))

    @app.exception_handler(IdentityError)
    async def identity_error(_, exc):
        return JSONResponse(status_code=exc.status, content={"detail": str(exc), "code": exc.code})

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
        return {"status": "ready", "version": "0.1.0", "environment": "research"}

    def account_access(x_account_session: str = Header(default="")):
        if settings.require_account:
            identity.pin.require_unlocked(x_account_session)

    app.include_router(
        data_router(DataSync(engine, tasks, settings.data_root, settings.token), account_access)
    )

    @app.get("/api/v1/jobs", response_model=list[Job], dependencies=[Depends(account_access)])
    def list_jobs():
        return tasks.list()

    @app.post(
        "/api/v1/imports",
        status_code=202,
        response_model=Job,
        dependencies=[Depends(account_access)],
    )
    def import_csv(body: ImportRequest):
        return tasks.submit(
            body.command_id, "data.import_csv", body.model_dump(exclude={"command_id"})
        )

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

    @app.post("/api/v1/jobs/{job_id}/publish")
    async def publish(job_id: str, request: Request, x_lease_token: Annotated[str, Header()]):
        # This first CSV slice is capped. General large-file publication is a separate protocol.
        chunks = bytearray()
        async for chunk in request.stream():
            chunks.extend(chunk)
            if len(chunks) > 8_000_000:
                raise HTTPException(413, "Artifact exceeds 8 MB")
        from starlette.concurrency import run_in_threadpool

        return await run_in_threadpool(data.publish, job_id, x_lease_token, bytes(chunks))

    @app.get(
        "/api/v1/snapshots", response_model=list[Snapshot], dependencies=[Depends(account_access)]
    )
    def list_snapshots():
        return data.list()

    @app.get(
        "/api/v1/snapshots/{snapshot_id}/bars",
        response_model=list[Bar],
        dependencies=[Depends(account_access)],
    )
    def bars(snapshot_id: str):
        try:
            return read_bars(data.path(snapshot_id))
        except KeyError:
            raise HTTPException(404, "Published snapshot not found")

    @app.get(
        "/api/v1/reference/releases",
        response_model=list[ReferenceSummary],
        dependencies=[Depends(account_access)],
    )
    def reference_list(limit: int = Query(50, ge=1, le=100), offset: int = Query(0, ge=0)):
        return reference.list(limit, offset)

    @app.post(
        "/api/v1/reference/releases",
        response_model=ReferenceRelease,
        dependencies=[Depends(account_access)],
    )
    def reference_publish(body: ReferenceCatalog):
        return reference.publish(body)

    @app.get(
        "/api/v1/reference/releases/{release_id}",
        response_model=ReferenceRelease,
        dependencies=[Depends(account_access)],
    )
    def reference_get(release_id: str):
        try:
            return reference.get(release_id)
        except KeyError:
            raise HTTPException(404, "Reference release not found") from None

    return app
