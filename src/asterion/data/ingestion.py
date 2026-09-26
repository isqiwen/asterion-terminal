"""Source observations of sync tasks for the internal Python process.

The Rust data service (`services/data`: observations) records, resumes, lists
and verifies them, and the entry serves their HTTP operations; the table
declaration only keeps the shared DDL until table ownership moves.
"""

from asterion_bindings import data_observations
from asterion_bindings.task_repository import Conflict
from sqlalchemy import JSON, Column, Integer, String, Table

from asterion.data.providers.public import ProviderError
from asterion.platform.store import metadata

observations = Table(
    "ingestion_observations",
    metadata,
    Column("job_id", String, primary_key=True),
    Column("attempt", Integer, primary_key=True),
    Column("partition_index", Integer, primary_key=True),
    Column("manifest", JSON, nullable=False),
)


def _refused(call, *args):
    try:
        return call(*args)
    except (Conflict, KeyError):
        raise
    except ValueError as error:
        raise ProviderError(str(error)) from None


class IngestionEvidence:
    def __init__(self, engine, root):
        self.engine, self.root = engine, root
        engine.initialize(observations)

    def record(self, job_id, token, index, value: dict):
        with self.engine.begin() as conn:
            return _refused(data_observations.record, conn, self.root, job_id, token, index, value)

    def resume(self, job_id, token):
        with self.engine.begin() as conn:
            return _refused(data_observations.resume, conn, self.root, job_id, token)

    def verify_publication(self, conn, job, envelope):
        _refused(data_observations.verify, conn, job["id"], job["attempt"], envelope)

    def list(self, job_id, offset=0, limit=50):
        with self.engine.connect() as conn:
            return _refused(data_observations.listing, conn, job_id, offset, limit)

    def preview(self, job_id, attempt, index, offset=0, limit=100):
        with self.engine.connect() as conn:
            return _refused(
                data_observations.preview, conn, self.root, job_id, attempt, index, offset, limit
            )
