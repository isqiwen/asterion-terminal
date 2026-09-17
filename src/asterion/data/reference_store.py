"""Immutable reference catalog releases; publication does not attest source authority."""

import hashlib
import json
import time

from pydantic import BaseModel
from sqlalchemy import JSON, Column, Float, String, Table, select
from sqlalchemy.exc import IntegrityError

from asterion.data.reference import ReferenceCatalog
from asterion.platform.store import metadata

releases = Table(
    "reference_releases",
    metadata,
    Column("id", String(64), primary_key=True),
    Column("published_at", Float, nullable=False),
    Column("catalog", JSON, nullable=False),
)


class ReferenceRelease(BaseModel):
    id: str
    published_at: float
    catalog: ReferenceCatalog


class ReferenceSummary(BaseModel):
    id: str
    published_at: float
    products: int
    contracts: int
    sources: list[str]


class ReferenceStore:
    def __init__(self, engine):
        self.engine = engine
        # Additive table creation supports existing installations without altering old tables.
        releases.create(engine, checkfirst=True)

    def publish(self, catalog: ReferenceCatalog) -> ReferenceRelease:
        payload = catalog.model_dump(mode="json")
        content = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        if len(content.encode()) > 4_000_000:
            raise ValueError("Reference catalog exceeds 4 MB")
        digest = hashlib.sha256(content.encode()).hexdigest()
        try:
            with self.engine.begin() as conn:
                conn.execute(
                    releases.insert().values(id=digest, published_at=time.time(), catalog=payload)
                )
        except IntegrityError:
            # Concurrent retries of the same content return the original publication.
            return self.get(digest)
        return self.get(digest)

    def get(self, release_id: str) -> ReferenceRelease:
        with self.engine.connect() as conn:
            row = (
                conn.execute(select(releases).where(releases.c.id == release_id)).mappings().first()
            )
        if row is None:
            raise KeyError(release_id)
        return ReferenceRelease.model_validate(dict(row))

    def list(self, limit: int = 50, offset: int = 0) -> list[ReferenceSummary]:
        with self.engine.connect() as conn:
            rows = (
                conn.execute(
                    select(releases)
                    .order_by(releases.c.published_at.desc(), releases.c.id)
                    .limit(limit)
                    .offset(offset)
                )
                .mappings()
                .all()
            )
        return [
            ReferenceSummary(
                id=r["id"],
                published_at=r["published_at"],
                products=len(r["catalog"]["products"]),
                contracts=len(r["catalog"]["contracts"]),
                sources=sorted(
                    {
                        item["provenance"]["source"]
                        for group in ("products", "contracts", "calendars", "rules")
                        for item in r["catalog"][group]
                    }
                ),
            )
            for r in rows
        ]
