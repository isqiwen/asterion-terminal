"""Product-owned immutable time versions and timestamp diagnostics."""

from datetime import date
from typing import Literal

from fastapi import APIRouter, Depends
from pydantic import AwareDatetime, BaseModel, ConfigDict
from sqlalchemy import JSON, Column, String, Table, select
from sqlalchemy.dialects.postgresql import insert as pg_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert

from asterion.identity.public import ACCOUNT_ACCESS
from asterion.platform.backup import BackupCheck
from asterion.platform.plugins import Activation, Plugin
from asterion.platform.resources import STORAGE
from asterion.platform.store import metadata
from asterion.trading_time.public import Span, TimeSpec, TimeVersion, time_id

versions = Table(
    "trading_time_versions",
    metadata,
    Column("id", String, primary_key=True),
    Column("spec", JSON, nullable=False),
)


class ResolveRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    version: TimeVersion
    contract: str
    timestamp: AwareDatetime
    boundary: Literal["event", "bar_end"]


class DayRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    version: TimeVersion
    contract: str
    trading_day: date


def activate(context):
    storage = context.resource(STORAGE)
    storage.initialize(versions)
    router = APIRouter(
        prefix="/api/v1/trading-time",
        dependencies=[Depends(context.require(ACCOUNT_ACCESS).account)],
    )

    @router.get("", response_model=list[TimeVersion])
    def listing():
        with storage.connect() as conn:
            return [
                TimeVersion.model_validate(dict(r))
                for r in conn.execute(select(versions)).mappings()
            ]

    @router.post("", response_model=TimeVersion)
    def save(body: TimeSpec):
        result = TimeVersion(id=time_id(body), spec=body)
        insert = pg_insert if storage.dialect.name == "postgresql" else sqlite_insert
        with storage.begin() as conn:
            conn.execute(
                insert(versions)
                .values(id=result.id, spec=body.model_dump(mode="json"))
                .on_conflict_do_nothing(index_elements=["id"])
            )
        return result

    @router.post("/resolve", response_model=Span)
    def resolve(body: ResolveRequest):
        return body.version.spec.resolve(body.contract, body.timestamp, body.boundary)

    @router.post("/day", response_model=list[Span])
    def daily(body: DayRequest):
        return body.version.spec.daily(body.contract, body.trading_day)

    return Activation(routers=(router,), close=storage.close)


def validate(evidence: list):
    for value in evidence:
        TimeVersion.model_validate(value)
    return {"trading_time_versions": len(evidence)}


plugin = Plugin(
    "asterion.trading_time",
    ("asterion.identity",),
    activate,
    consumes=(ACCOUNT_ACCESS,),
    resources=(STORAGE,),
    backup=BackupCheck(list, validate),
)
