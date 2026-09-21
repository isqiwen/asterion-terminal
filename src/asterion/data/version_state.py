"""Mutable presentation state kept outside immutable version manifests."""

from sqlalchemy import Boolean, Column, Float, ForeignKey, Integer, Table

from asterion.platform.store import metadata

version_states = Table(
    "data_version_states",
    metadata,
    Column("version_id", ForeignKey("data_versions.id"), primary_key=True),
    Column("archived", Boolean, nullable=False),
    Column("revision", Integer, nullable=False),
    Column("updated_at", Float, nullable=False),
)
