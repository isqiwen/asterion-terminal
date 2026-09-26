"""Atomic daily sync admission shared by functional workflow consumers."""

from asterion_bindings import data_sync
from asterion_bindings.task_models import Job

from asterion.data.public import DailySyncBatch
from asterion.data.sources import refused


def submit_batch(sync, transaction, body: DailySyncBatch):
    """All source plans, credentials and identities pass before any job is inserted."""
    with sync.engine.join(transaction) as conn, refused():
        rows = data_sync.submit_batch(
            conn, sync.credentials, sync.root, body.model_dump(mode="json")
        )
    return tuple(Job.model_validate(row) for row in rows)
