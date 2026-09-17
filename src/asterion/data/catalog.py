from sqlalchemy import JSON, Column, String, Table

from asterion.platform.store import metadata

snapshots = Table(
    "snapshots",
    metadata,
    Column("id", String, primary_key=True),
    Column("job_id", String, unique=True, nullable=False),
    Column("manifest", JSON, nullable=False),
)
