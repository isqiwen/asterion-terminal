"""Public task state returned by application and business APIs."""

from pydantic import BaseModel, ConfigDict

from asterion_bindings.tasks_generated import TaskState

from .events import Topic


class Job(BaseModel):
    id: str
    command_id: str
    kind: str
    state: TaskState
    attempt: int
    created_at: float
    worker_id: str | None = None
    lease_until: float | None = None
    error: str | None = None
    result: dict | None = None


class ClaimedJob(Job):
    communication: dict
    payload: dict
    token: str


class TaskChanged(BaseModel):
    model_config = ConfigDict(extra="forbid")
    job_id: str
    kind: str
    state: TaskState
    attempt: int


TASK_CHANGED = Topic("runtime.task.changed", "asterion.runtime", TaskChanged, "/jobs")
