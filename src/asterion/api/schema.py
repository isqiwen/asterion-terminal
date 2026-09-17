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


class Manifest(BaseModel):
    frequency: str | None = None
    time_semantics: str | None = None
    dataset_id: str | None = None
    schema_version: int
    rows: int
    checksum: str
    contracts: list[str]
    start: str
    end: str
    uri: str
    source: str
    state: Literal["PUBLISHED"]


class Snapshot(BaseModel):
    id: str
    job_id: str
    manifest: Manifest
