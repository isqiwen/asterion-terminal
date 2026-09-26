"""The built-in data types of the Rust data store: manifests, row validation, coverage."""

import json
from datetime import date

from . import _native


def manifests() -> list[dict]:
    return json.loads(_native.data_types())


def validate(type_id: str, rows: list[dict]) -> None:
    """Refusals raise ValueError with the user-facing reason."""
    _native.data_type_validate(type_id, json.dumps(rows, default=str))


def coverage(type_id: str, rows: int, start: date | None, end: date | None) -> str:
    return _native.data_type_coverage(
        type_id, rows, start.isoformat() if start else None, end.isoformat() if end else None
    )
