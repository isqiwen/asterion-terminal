"""Public task state returned by application and business APIs."""

from typing import Literal

from pydantic import BaseModel


class Job(BaseModel):
    id: str
    command_id: str
    kind: str
    state: Literal["QUEUED", "RUNNING", "SUCCEEDED", "FAILED", "CANCELLED"]
    attempt: int
    created_at: float
    worker_id: str | None = None
    lease_until: float | None = None
    error: str | None = None
    result: dict | None = None


class ClaimedJob(Job):
    payload: dict
    token: str
