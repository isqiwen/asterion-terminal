"""Tables of data source connections and configurations.

The Rust data service (`services/data`: connections, configuration,
credentials) owns every read and write; these declarations only keep the
shared DDL until table ownership moves to the Rust services.
"""

from sqlalchemy import Column, Float, Integer, String, Table

from asterion.platform.store import metadata

connections = Table(
    "data_connections",
    metadata,
    Column("id", String, primary_key=True),
    Column("provider", String, nullable=False),
    Column("name", String, nullable=False),
)

connection_settings = Table(
    "data_connection_settings",
    metadata,
    Column("id", String, primary_key=True),
    Column("name", String, nullable=False),
    Column("state", String, nullable=False),
    Column("revision", Integer, nullable=False),
)

configurations = Table(
    "data_provider_configurations",
    metadata,
    Column("provider", String, primary_key=True),
    Column("revision", Integer, nullable=False),
    Column("schema_version", Integer, nullable=False),
    Column("snapshot_ref", String, nullable=False),
)

verification_records = Table(
    "data_connection_verifications",
    metadata,
    Column("provider", String, primary_key=True),
    Column("revision", Integer, nullable=False),
    Column("started_at", Float, nullable=False),
    Column("checked_at", Float, nullable=False),
    Column("status", String, nullable=False),
)

TABLES = (connections, connection_settings, configurations, verification_records)
