"""Source observations of sync tasks, from the Rust data service, on the caller's
storage transaction. Refusals raise ValueError, lease and reuse conflicts
Conflict, unknown tasks or observations KeyError."""

import json
import time
from pathlib import Path

from . import _native
from .database import kernel_call, native_connection
from .storage import transaction_connection


def _call(transaction, action, *args, write=False):
    conn = transaction_connection(transaction, write=write)
    return json.loads(kernel_call(conn, action, native_connection(conn), *args))


def record(transaction, root: Path, job_id: str, token: str, index: int, value: dict) -> dict:
    return _call(
        transaction,
        _native.data_observation_record,
        root,
        job_id,
        token,
        index,
        json.dumps(value),
        time.time(),
        write=True,
    )


def resume(transaction, root: Path, job_id: str, token: str) -> dict[int, dict]:
    reused = _call(
        transaction, _native.data_observation_resume, root, job_id, token, time.time(), write=True
    )
    return {int(index): value for index, value in reused.items()}


def verify(transaction, job_id: str, attempt: int, envelope: list) -> None:
    _call(transaction, _native.data_observation_verify, job_id, attempt, json.dumps(envelope))


def listing(transaction, job_id: str, offset: int, limit: int) -> dict:
    return _call(transaction, _native.data_observation_list, job_id, offset, limit)


def preview(
    transaction, root: Path, job_id: str, attempt: int, index: int, offset: int, limit: int
) -> dict:
    return _call(
        transaction,
        _native.data_observation_preview,
        root,
        job_id,
        attempt,
        index,
        offset,
        limit,
    )
