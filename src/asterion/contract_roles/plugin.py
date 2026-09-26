"""Provider role evidence registration and fixed-version diagnostics, owned by this plugin."""

from collections import Counter
from dataclasses import dataclass
from datetime import UTC, datetime

from asterion_bindings.calendar import Span
from asterion_bindings.plugin_host import Activation, Plugin
from asterion_bindings.recovery import BackupCheck
from asterion_bindings.roles import (
    NextOpening,
    RoleQuery,
    RoleResolution,
    RoleSpec,
    RoleVersion,
    next_opening,
    resolve,
    role_id,
)
from asterion_bindings.task_models import Job
from fastapi import APIRouter, Depends, Header, HTTPException, Request
from sqlalchemy import JSON, Column, String, Table, select, true
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert
from starlette.concurrency import run_in_threadpool

from asterion.contract_roles.candidates import (
    CandidateEvidence,
    CandidateRequest,
    candidate_evidence,
)
from asterion.contract_roles.computed import ComputedSources, algorithm_artifact, replay_computed
from asterion.contract_roles.computed_public import (
    COMPUTED_ROLE_ACCESS,
    AlgorithmArtifact,
    ComputedRequest,
    ComputedResolution,
    ComputedRoleAccess,
    ComputedSpec,
    ComputedVersion,
    digest,
    resolve_computed,
)
from asterion.contract_roles.hierarchy import RoleDirectory
from asterion.contract_roles.public import ROLE_ACCESS, RoleAccess
from asterion.contract_roles.ranking import RankingRequest, RankingResult, rank_roles
from asterion.contract_roles.sequence import (
    ContinuationRequest,
    SequenceRequest,
    SequenceResult,
    check_published,
    continuation,
    verify_sequence,
)
from asterion.contract_roles.sources import RoleSourceRequest, RoleSources
from asterion.contract_roles.sync_batch import BatchContinuation, BatchPlan
from asterion.contract_roles.sync_batch import preview as preview_sync_batch
from asterion.contract_roles.sync_batch import submit as submit_batch_continuation
from asterion.contract_roles.sync_workflow import (
    SyncContinuation,
    SyncWorkflow,
    WorkflowRecord,
    workflows,
)
from asterion.contract_roles.tasks import RoleTasks, Submission, handler
from asterion.data.public import SYNC_ACCESS, SYNC_BATCH_ACCESS, VERSION_ACCESS, VersionAccess
from asterion.identity.public import ACCOUNT_ACCESS
from asterion.platform.resources import STORAGE, TASKS
from asterion.platform.store import metadata

versions = Table(
    "contract_role_versions",
    metadata,
    Column("id", String, primary_key=True),
    Column("spec", JSON, nullable=False),
)

computed_versions = Table(
    "computed_role_versions",
    metadata,
    Column("id", String, primary_key=True),
    Column("spec", JSON, nullable=False),
    Column("published_at", String, nullable=False),
)


@dataclass(frozen=True)
class RoleBackup:
    versions: tuple[dict, ...]
    sources: VersionAccess
    computed_versions: tuple[dict, ...]
    sync_workflows: tuple[dict, ...]


def validate(evidence: RoleBackup):
    sources = RoleSources(evidence.sources)
    for value in evidence.versions:
        version = RoleVersion.model_validate(value)
        sources.verify(version.spec)
    computed = ComputedSources(evidence.sources)
    for value in evidence.computed_versions:
        record = ComputedVersion.model_validate(value)
        computed.verify(record.spec)
    identifiers = {value["id"] for value in evidence.computed_versions}
    for row in evidence.sync_workflows:
        request = WorkflowRecord.model_validate(row).request
        if (
            row["id"] != digest({"command_id": request.command_id})
            or request.previous_version_id not in identifiers
        ):
            raise ValueError("同步续算工作流身份或前序版本不一致")
    return {
        "contract_role_versions": len(evidence.versions),
        "computed_role_versions": len(evidence.computed_versions),
    }


def activate(context):
    storage = context.resource(STORAGE)
    storage.initialize(versions, computed_versions)
    sources = RoleSources(context.require(VERSION_ACCESS))
    computed = ComputedSources(context.require(VERSION_ACCESS))
    router = APIRouter(
        prefix="/api/v1/contract-roles",
        dependencies=[Depends(context.require(ACCOUNT_ACCESS).account)],
    )

    def read(identifier):
        with storage.connect() as conn:
            row = (
                conn.execute(select(versions).where(versions.c.id == identifier)).mappings().first()
            )
        if row is None:
            raise ValueError("角色版本不存在")
        return RoleVersion.model_validate(dict(row))

    def read_computed(identifier):
        with storage.connect() as conn:
            row = (
                conn.execute(select(computed_versions).where(computed_versions.c.id == identifier))
                .mappings()
                .first()
            )
        if row is None:
            raise ValueError("计算角色版本不存在")
        return ComputedVersion.model_validate(dict(row))

    task_service = RoleTasks(
        storage, context.resource(TASKS), computed, read_computed, computed_versions
    )
    sync_workflow = SyncWorkflow(
        storage, context.resource(TASKS), context.require(SYNC_ACCESS), task_service
    )
    worker_router = APIRouter(prefix="/api/v1")

    @router.get("/computed/{identifier}/sync-plan", response_model=BatchPlan)
    def get_sync_plan(identifier: str):
        return preview_sync_batch(sync_workflow, identifier)

    @router.get("/computed/{identifier}/sync-workflows", response_model=list[WorkflowRecord])
    def list_sync_workflows(identifier: str):
        with storage.connect() as conn:
            rows = (
                conn.execute(
                    select(workflows)
                    .where(workflows.c.request["previous_version_id"].as_string() == identifier)
                    .order_by(workflows.c.id)
                )
                .mappings()
                .all()
            )
        return [WorkflowRecord.model_validate(dict(row)) for row in rows]

    @router.post("/computed/sync-batches", status_code=202)
    def submit_sync_batch(body: BatchContinuation):
        return submit_batch_continuation(sync_workflow, context.require(SYNC_BATCH_ACCESS), body)

    @router.post("/computed/sync-workflows", status_code=202)
    def submit_sync_workflow(body: SyncContinuation):
        return sync_workflow.submit(body)

    @router.get("/computed/sync-workflows/{identifier}")
    def get_sync_workflow(identifier: str):
        return sync_workflow.get(identifier)

    @router.post("/computed/tasks", response_model=Job, status_code=202)
    def submit_task(body: Submission):
        return task_service.submit(body)

    @router.get("/computed/tasks/{identifier}", response_model=Job)
    def get_task(identifier: str):
        return Job.model_validate(task_service.get(identifier))

    @router.post("/computed/tasks/{identifier}/retry", response_model=Job, status_code=202)
    def retry_task(identifier: str):
        return task_service.retry(identifier)

    @worker_router.post("/jobs/{job_id}/publish-roles")
    async def publish_task(job_id: str, request: Request, x_lease_token: str = Header()):
        content = bytearray()
        async for chunk in request.stream():
            content.extend(chunk)
            if len(content) > 8_000_000:
                raise HTTPException(413, "续算结果超过 8 MB")
        return await run_in_threadpool(task_service.publish, job_id, x_lease_token, bytes(content))

    @router.post("/computed/continue-preview", response_model=ComputedSpec)
    def continue_preview(body: ContinuationRequest):
        return continuation(computed, read_computed(body.previous_version_id), body)

    @router.post("/computed/sequence/verify", response_model=SequenceResult)
    def sequence_verify(body: SequenceRequest):
        return verify_sequence(computed, read_computed, body)

    @router.get("/hierarchy", response_model=list[RoleDirectory])
    def hierarchy():
        counts = Counter()
        with storage.connect() as conn:
            for spec in conn.execute(select(computed_versions.c.spec)).scalars():
                for role in ("main", "secondary"):
                    counts[(spec["request"]["product_id"], role)] += 1
            for spec in conn.execute(select(versions.c.spec)).scalars():
                for role in {row["role"] for row in spec["reports"]}:
                    counts[(spec["product_id"], role)] += 1
        return [
            RoleDirectory(product_id=product, role=role, count=count)
            for (product, role), count in sorted(counts.items())
        ]

    @router.get("/computed", response_model=list[ComputedVersion])
    def computed_listing(limit: int = 50, offset: int = 0, product_id: str = ""):
        if not 1 <= limit <= 100 or offset < 0:
            raise HTTPException(422, "分页范围无效")
        with storage.connect() as conn:
            return [
                ComputedVersion.model_validate(dict(row))
                for row in conn.execute(
                    select(computed_versions)
                    .where(
                        computed_versions.c.spec["request"]["product_id"].as_string() == product_id
                        if product_id
                        else true()
                    )
                    .order_by(computed_versions.c.id)
                    .limit(limit)
                    .offset(offset)
                ).mappings()
            ]

    @router.get("/computed/{identifier}", response_model=ComputedVersion)
    def computed_get(identifier: str):
        return read_computed(identifier)

    @router.post("/computed/preview", response_model=ComputedSpec)
    def computed_preview(body: ComputedRequest):
        return computed.build(body)

    @router.post("/computed", response_model=ComputedVersion)
    def computed_save(body: ComputedSpec):
        computed.verify(body)
        identifier = digest(body.model_dump(mode="json"))
        record = ComputedVersion(id=identifier, spec=body, published_at=datetime.now(UTC))
        insert = pg_insert if storage.dialect.name == "postgresql" else sqlite_insert
        with storage.begin() as conn:
            conn.execute(
                insert(computed_versions)
                .values(
                    id=identifier,
                    spec=body.model_dump(mode="json"),
                    published_at=record.published_at.isoformat(),
                )
                .on_conflict_do_nothing(index_elements=["id"])
            )
        return read_computed(identifier)

    @router.post("/computed/start", response_model=ComputedVersion)
    def computed_start(body: ComputedSpec):
        """Publish a timely single-day seed; retries preserve its first publication."""
        computed.verify(body)
        if len(body.input.observations) != 1:
            raise ValueError("首次连续发布必须只包含一个观测交易日")
        identifier = digest(body.model_dump(mode="json"))
        insert = pg_insert if storage.dialect.name == "postgresql" else sqlite_insert
        with storage.begin() as conn:
            existing = (
                conn.execute(select(computed_versions).where(computed_versions.c.id == identifier))
                .mappings()
                .first()
            )
            record = (
                ComputedVersion.model_validate(dict(existing))
                if existing
                else ComputedVersion(id=identifier, spec=body, published_at=datetime.now(UTC))
            )
            check_published(record)
            conn.execute(
                insert(computed_versions)
                .values(
                    id=identifier,
                    spec=body.model_dump(mode="json"),
                    published_at=record.published_at.isoformat(),
                )
                .on_conflict_do_nothing(index_elements=["id"])
            )
        record = read_computed(identifier)
        check_published(record)
        return record

    @router.post("/computed/resolve", response_model=ComputedResolution)
    def computed_lookup(body: RoleQuery):
        return resolve_computed(read_computed(body.version_id), body)

    @router.post("/computed/replay", response_model=RankingResult)
    def computed_replay(body: ComputedSpec):
        return replay_computed(body)

    @router.get("", response_model=list[RoleVersion])
    def listing(limit: int = 50, offset: int = 0, product_id: str = ""):
        if not 1 <= limit <= 100 or offset < 0:
            raise HTTPException(422, "分页范围无效")
        with storage.connect() as conn:
            return [
                RoleVersion.model_validate(dict(r))
                for r in conn.execute(
                    select(versions)
                    .where(
                        versions.c.spec["product_id"].as_string() == product_id
                        if product_id
                        else true()
                    )
                    .order_by(versions.c.id)
                    .limit(limit)
                    .offset(offset)
                ).mappings()
            ]

    @router.get("/{identifier}", response_model=RoleVersion)
    def get(identifier: str):
        return read(identifier)

    @router.post("/source/preview", response_model=RoleSpec)
    def preview(body: RoleSourceRequest):
        return sources.build(body)

    @router.post("", response_model=RoleVersion)
    def save(body: RoleSpec):
        sources.verify(body)
        result = RoleVersion(id=role_id(body), spec=body)
        insert = pg_insert if storage.dialect.name == "postgresql" else sqlite_insert
        with storage.begin() as conn:
            conn.execute(
                insert(versions)
                .values(id=result.id, spec=body.model_dump(mode="json"))
                .on_conflict_do_nothing(index_elements=["id"])
            )
        return result

    @router.post("/resolve", response_model=RoleResolution)
    def lookup(body: RoleQuery):
        return resolve(read(body.version_id), body)

    @router.post("/next-opening", response_model=Span)
    def opening(body: NextOpening):
        return next_opening(body)

    @router.post("/ranking/diagnose", response_model=RankingResult)
    def ranking(body: RankingRequest):
        return rank_roles(body)

    @router.post("/candidates/preview", response_model=CandidateEvidence)
    def candidates(body: CandidateRequest):
        return candidate_evidence(context.require(VERSION_ACCESS).read, body)

    @router.get("/ranking/algorithm", response_model=AlgorithmArtifact)
    def algorithm():
        return algorithm_artifact()

    return Activation(
        exports={
            ROLE_ACCESS: RoleAccess(read),
            COMPUTED_ROLE_ACCESS: ComputedRoleAccess(read_computed),
        },
        routers=(router, worker_router),
        close=storage.close,
        hooks={"data.sync_published": (sync_workflow.published,)},
    )


plugin = Plugin(
    "asterion.contract_roles",
    ("asterion.identity", "asterion.data"),
    activate,
    consumes=(ACCOUNT_ACCESS, VERSION_ACCESS, SYNC_ACCESS, SYNC_BATCH_ACCESS),
    provides=(ROLE_ACCESS, COMPUTED_ROLE_ACCESS),
    resources=(STORAGE, TASKS),
    handlers=(handler,),
    backup=BackupCheck(RoleBackup, validate),
)
