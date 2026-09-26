"""Catalogue queries and version rows of the Rust data service, on the caller's
storage transaction."""

import json

from . import _native
from .database import kernel_call, native_connection
from .storage import transaction_connection


def _call(transaction, action, *args):
    conn = transaction_connection(transaction)
    return json.loads(kernel_call(conn, action, native_connection(conn), *args))


def catalog_list(transaction, query: dict) -> dict:
    return _call(transaction, _native.data_catalog_list, json.dumps(query))


def catalog_history(transaction, dataset_id: str, offset: int, limit: int) -> dict:
    return _call(transaction, _native.data_catalog_history, dataset_id, offset, limit)


def catalog_hierarchy(transaction, include_archived: bool) -> list[dict]:
    return _call(transaction, _native.data_catalog_hierarchy, include_archived)


def version_preview(transaction, root, version_id: str, offset: int, limit: int) -> dict:
    """Checksum-verified rows; KeyError for unknown versions, ValueError when unreadable."""
    return _call(transaction, _native.data_version_preview, root, version_id, offset, limit)


def import_execute(transaction, root, job_id: str, token: str, now: float) -> dict:
    """Execute a claimed import task in this write transaction (the entry's executor step)."""
    conn = transaction_connection(transaction, write=True)
    return json.loads(
        kernel_call(
            conn, _native.data_import_execute, native_connection(conn), root, job_id, token, now
        )
    )


def snapshot_bars(transaction, root, snapshot_id: str, limit: int = 1000) -> list[dict]:
    return _call(transaction, _native.data_snapshot_bars, root, snapshot_id, limit)


def daily_chart(rows: list[dict], available_at: str) -> tuple[bytes, dict]:
    content, manifest = _native.data_daily_chart(json.dumps(rows, default=str), available_at)
    return content, json.loads(manifest)
