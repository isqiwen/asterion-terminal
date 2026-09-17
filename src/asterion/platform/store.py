from sqlalchemy import JSON, Column, Float, Integer, MetaData, String, Table, create_engine

metadata = MetaData()
jobs = Table(
    "jobs",
    metadata,
    Column("id", String, primary_key=True),
    Column("command_id", String, unique=True, nullable=False),
    Column("kind", String, nullable=False),
    Column("payload", JSON, nullable=False),
    Column("state", String, nullable=False),
    Column("attempt", Integer, nullable=False, default=0),
    Column("token", String),
    Column("worker_id", String),
    Column("lease_until", Float),
    Column("created_at", Float, nullable=False),
    Column("error", String),
    Column("result", JSON),
)


def database(url: str):
    return create_engine(url, pool_pre_ping=True)
