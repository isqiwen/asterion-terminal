"""CSV import fixtures through the Rust data service, which owns imports.

Fixtures queue an import task the way the entry does and execute it with the
entry's own executor step; imports themselves are tested in services/data and
through the real entry.
"""

import time

from asterion_bindings.data_catalog import import_execute, snapshot_bars
from storage_support import data_store, raw_engine


def import_options(identity, *, source_id="fixture", **options):
    """Published import options; the Rust service fills and checks the rest."""
    return {
        "identity": identity.model_dump(mode="json")
        if hasattr(identity, "model_dump")
        else identity,
        "trading_time": None,
        "timestamp_semantics": None,
        "type_id": "futures.bars",
        "frequency": "unspecified",
        "source_id": source_id,
        "column_mapping": {},
    } | {
        key: value.model_dump(mode="json") if hasattr(value, "model_dump") else value
        for key, value in options.items()
    }


def import_payload(csv, options, source="fixture.csv"):
    return {"source": source, "csv": csv, "options": options}


def publish_import(engine, root, job):
    """Execute a claimed import task as the entry's executor does."""
    with data_store(raw_engine(engine)).begin() as transaction:
        return import_execute(transaction, root, job["id"], job["token"], time.time())


def chart_bars(engine, root, snapshot_id, limit=1000):
    with data_store(raw_engine(engine)).connect() as transaction:
        return snapshot_bars(transaction, root, snapshot_id, limit)
