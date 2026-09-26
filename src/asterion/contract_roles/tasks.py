"""Role-owned fixed-input continuation tasks; publication is lease-guarded and atomic."""

from datetime import UTC, datetime

from asterion_bindings.task_handlers import TaskHandler
from asterion_bindings.task_models import Job
from pydantic import Field
from sqlalchemy import select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert

from asterion.contract_roles.computed import replay_computed
from asterion.contract_roles.computed_public import ComputedSpec, ComputedVersion, digest
from asterion.contract_roles.models import Strict
from asterion.contract_roles.sequence import (
    ContinuationRequest,
    check_extension,
    check_published,
    continuation,
)
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs

KIND = "contract_roles.continue"


class Submission(Strict):
    command_id: str = Field(min_length=1, max_length=100)
    continuation: ContinuationRequest


class Payload(Strict):
    previous: ComputedVersion
    request: ContinuationRequest
    spec: ComputedSpec


def execute(context, payload):
    body = Payload.model_validate(payload)
    if body.previous.id != body.request.previous_version_id:
        raise ValueError("续算前序版本不一致")
    replay_computed(body.previous.spec)
    check_published(body.previous)
    replay_computed(body.spec)
    check_extension(body.previous.spec, body.spec)
    return canonical(body.spec.model_dump(mode="json")), {}


handler = TaskHandler(KIND, execute, "/publish-roles")


class RoleTasks:
    def __init__(self, storage, tasks, sources, read, table):
        self.storage, self.tasks, self.sources = storage, tasks, sources
        self.read, self.table = read, table

    def submit(self, body: Submission):
        previous = self.read(body.continuation.previous_version_id)
        spec = continuation(self.sources, previous, body.continuation)
        payload = Payload(previous=previous, request=body.continuation, spec=spec)
        return Job.model_validate(
            self.tasks.submit(body.command_id, KIND, payload.model_dump(mode="json"))
        )

    def get(self, identifier):
        with self.storage.connect() as conn:
            row = conn.execute(select(jobs).where(jobs.c.id == identifier)).mappings().first()
        if row is None:
            raise ValueError("续算任务不存在")
        return dict(row)

    def retry(self, identifier):
        row = self.get(identifier)
        if row["state"] != "FAILED":
            raise ValueError("只有失败的续算任务可以重试")
        body = Payload.model_validate(row["payload"])
        if (
            continuation(self.sources, self.read(body.request.previous_version_id), body.request)
            != body.spec
        ):
            raise ValueError("续算固定输入已变化")
        # One successor per failed attempt, including after a lost HTTP response.
        return Job.model_validate(
            self.tasks.submit("role-retry:" + identifier, KIND, row["payload"])
        )

    def publish(self, identifier, token, content):
        with self.storage.begin() as conn:
            row = (
                conn.execute(select(jobs).where(jobs.c.id == identifier).with_for_update())
                .mappings()
                .first()
            )
            if row is None:
                raise ValueError("续算任务不存在")
            body = Payload.model_validate(row["payload"])
            expected = canonical(body.spec.model_dump(mode="json"))
            if content != expected:
                raise ValueError("续算结果与固定输入不一致")
            if row["state"] == "SUCCEEDED" and row["token"] == token:
                return row["result"]
            self.tasks.require_lease(conn, identifier, token)
            if (
                continuation(
                    self.sources, self.read(body.request.previous_version_id), body.request
                )
                != body.spec
            ):
                raise ValueError("续算发布来源不一致")
            self.tasks.require_lease(conn, identifier, token)
            version_id = digest(body.spec.model_dump(mode="json"))
            existing = (
                conn.execute(select(self.table).where(self.table.c.id == version_id))
                .mappings()
                .first()
            )
            record = (
                ComputedVersion.model_validate(dict(existing))
                if existing
                else ComputedVersion(id=version_id, spec=body.spec, published_at=datetime.now(UTC))
            )
            check_published(record)
            insert = pg_insert if self.storage.dialect.name == "postgresql" else sqlite_insert
            conn.execute(
                insert(self.table)
                .values(
                    id=record.id,
                    spec=record.spec.model_dump(mode="json"),
                    published_at=record.published_at.isoformat(),
                )
                .on_conflict_do_nothing(index_elements=["id"])
            )
            result = {"computed_version_id": record.id, "previous_version_id": body.previous.id}
            self.tasks.complete(conn, identifier, token, result)
            return result
