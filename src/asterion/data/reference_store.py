"""Immutable reference catalog releases; publication does not attest source authority."""

import hashlib
import json
import time

from pydantic import BaseModel, ConfigDict, model_validator
from sqlalchemy import JSON, Column, Float, String, Table, select
from sqlalchemy.exc import IntegrityError

from asterion.data.library import versions
from asterion.data.reference import ReferenceCatalog
from asterion.data.reference_source import validate_catalog_input
from asterion.platform.store import metadata

releases = Table(
    "reference_releases",
    metadata,
    Column("id", String(64), primary_key=True),
    Column("published_at", Float, nullable=False),
    Column("catalog", JSON, nullable=False),
)


class ReferenceRelease(BaseModel):
    model_config = ConfigDict(extra="forbid")

    id: str
    published_at: float
    catalog: ReferenceCatalog

    @model_validator(mode="after")
    def fingerprint(self):
        if self.id != catalog_digest(self.catalog):
            raise ValueError("合约目录版本指纹不一致")
        return self


def catalog_digest(catalog: ReferenceCatalog) -> str:
    content = json.dumps(
        catalog.model_dump(mode="json"), sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode()
    if len(content) > 4_000_000:
        raise ValueError("Reference catalog exceeds 4 MB")
    return hashlib.sha256(content).hexdigest()


class ReferenceSummary(BaseModel):
    id: str
    published_at: float
    products: int
    contracts: int
    sources: list[str]


class ReferenceStore:
    def __init__(self, engine):
        self.engine = engine
        engine.initialize(releases)

    def publish(self, catalog: ReferenceCatalog) -> ReferenceRelease:
        payload = catalog.model_dump(mode="json")
        digest = catalog_digest(catalog)
        try:
            with self.engine.begin() as conn:
                for item in catalog.inputs:
                    manifest = conn.execute(
                        select(versions.c.manifest).where(versions.c.id == item.version_id)
                    ).scalar_one_or_none()
                    if manifest is None:
                        raise ValueError("目录输入版本不存在")
                    validate_catalog_input(item, manifest)
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
        validated = [ReferenceRelease.model_validate(dict(row)) for row in rows]
        return [
            ReferenceSummary(
                id=r.id,
                published_at=r.published_at,
                products=len(r.catalog.products),
                contracts=len(r.catalog.contracts),
                sources=sorted(
                    {
                        item.provenance.source
                        for group in (r.catalog.products, r.catalog.contracts, r.catalog.symbols)
                        for item in group
                    }
                ),
            )
            for r in validated
        ]

    def references(self, conn, version_id):
        return sum(
            any(
                item.version_id == version_id
                for item in ReferenceRelease.model_validate(dict(row)).catalog.inputs
            )
            for row in conn.execute(select(releases)).mappings()
        )
