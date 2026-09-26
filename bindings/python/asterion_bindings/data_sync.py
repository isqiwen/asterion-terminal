"""Sync task admission and submission of the Rust data service, on the caller's
storage transaction. Refusals raise ValueError, reused or concurrent commands
Conflict."""

import json
import time
from pathlib import Path

from . import _native
from .data_sources import SourceCredentials
from .database import kernel_call, native_connection
from .storage import transaction_connection


def _call(transaction, action, *args, write=False):
    conn = transaction_connection(transaction, write=write)
    return json.loads(kernel_call(conn, action, native_connection(conn), *args))


def _text(value) -> str:
    return json.dumps(value, default=str)


def task_identity(payload: dict) -> dict | None:
    """Check a payload's request, type and fixed identity against its source."""
    return json.loads(_native.data_sync_task_identity(_text(payload)))


def validate_identity(transaction, root: Path, payload: dict) -> dict:
    """The fixed identity, rebuilt unchanged from its fixed contracts version."""
    return _call(transaction, _native.data_sync_validate_identity, root, _text(payload))


def prepare(transaction, root: Path, submission: dict) -> tuple[dict, dict | None]:
    value = _call(transaction, _native.data_sync_prepare, root, _text(submission))
    return value["request"], value["identity"]


def payload(transaction, credentials: SourceCredentials, request: dict) -> dict:
    return _call(transaction, _native.data_sync_payload, credentials._handle, _text(request))


def submit(
    transaction, credentials: SourceCredentials, root: Path, request: dict, admission: dict
) -> dict:
    return _call(
        transaction,
        _native.data_sync_submit,
        credentials._handle,
        root,
        _text(request),
        _text(admission),
        time.time(),
        write=True,
    )


def admit(transaction, credentials: SourceCredentials, root: Path, submission: dict) -> dict:
    return _call(
        transaction,
        _native.data_sync_admit,
        credentials._handle,
        root,
        _text(submission),
        time.time(),
        write=True,
    )


def submit_batch(
    transaction, credentials: SourceCredentials, root: Path, batch: dict
) -> list[dict]:
    return _call(
        transaction,
        _native.data_sync_submit_batch,
        credentials._handle,
        root,
        _text(batch),
        time.time(),
        write=True,
    )


def collect(
    database_url: str, credentials: SourceCredentials, root: Path, job: dict, lease_seconds=240.0
) -> bytes:
    """Collect a claimed sync task from its built-in source, as the entry does;
    the evidence to publish. Refusals raise ValueError, a lost lease Conflict."""
    return _native.data_sync_collect(
        database_url, credentials._handle, root, _text(job), lease_seconds
    )
