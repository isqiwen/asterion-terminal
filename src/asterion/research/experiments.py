"""Account-owned bounded parameter experiments using existing atomic task submission."""

import hashlib
import itertools
import time

from pydantic import BaseModel, ConfigDict, Field
from sqlalchemy import JSON, Column, Float, String, Table, select
from sqlalchemy.exc import IntegrityError

from asterion.platform.serialization import canonical
from asterion.platform.store import jobs, metadata
from asterion.platform.tasks.service import Conflict
from asterion.research.engine import BacktestRequest
from asterion.research.parameters import ParameterValue
from asterion.research.service import KIND

experiments = Table(
    "research_experiments",
    metadata,
    Column("id", String, primary_key=True),
    Column("owner", String, nullable=False),
    Column("name", String, nullable=False),
    Column("created_at", Float, nullable=False),
    Column("checksum", String, nullable=False),
    Column("spec", JSON, nullable=False),
    Column("runs", JSON, nullable=False),
)


class ExperimentRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    name: str = Field(min_length=1, max_length=80)
    base: BacktestRequest
    grid: dict[str, list[ParameterValue]] = Field(min_length=1, max_length=4)


class Experiments:
    def __init__(self, service):
        self.service = service
        self.engine = service.engine
        self.engine.initialize(experiments)

    def list(self, owner):
        with self.engine.connect() as conn:
            return [
                dict(row)
                for row in conn.execute(
                    select(experiments)
                    .where(experiments.c.owner == owner)
                    .order_by(experiments.c.created_at.desc())
                    .limit(100)
                ).mappings()
            ]

    def get(self, owner, identifier):
        with self.engine.connect() as conn:
            row = (
                conn.execute(
                    select(experiments).where(
                        experiments.c.id == identifier, experiments.c.owner == owner
                    )
                )
                .mappings()
                .first()
            )
            if row is None:
                raise KeyError(identifier)
            items = [
                dict(item)
                for item in conn.execute(select(jobs).where(jobs.c.id.in_(row["runs"]))).mappings()
            ]
        by_id = {item["id"]: item for item in items}
        return dict(row) | {
            "items": [
                {key: by_id[identifier][key] for key in ("id", "state", "error", "result")}
                | {"parameters": by_id[identifier]["payload"]["request"]["parameters"]}
                for identifier in row["runs"]
            ]
        }

    def submit(self, owner, body):
        spec = body.model_dump(mode="json")
        identifier = hashlib.sha256(canonical([owner, body.base.command_id])).hexdigest()
        try:
            existing = self.get(owner, identifier)
        except KeyError:
            existing = None
        if existing:
            if canonical(existing["spec"]) != canonical(spec):
                raise Conflict("实验命令标识已用于不同输入")
            return existing
        strategy = self.service.strategies.resolve(body.base.strategy)
        keys = sorted(body.grid)
        if not set(keys) <= {p.key for p in strategy.info.parameters}:
            raise ValueError("实验包含未知策略参数")
        size = 1
        for key in keys:
            candidates = body.grid[key]
            if (
                not candidates
                or len(candidates) > 32
                or len({canonical(v) for v in candidates}) != len(candidates)
            ):
                raise ValueError("参数候选不能为空、重复或超过 32 项")
            size *= len(candidates)
        if not 2 <= size <= 32:
            raise ValueError("每次实验必须包含 2 至 32 个组合")
        combinations = [
            strategy.parameters(body.base.parameters | dict(zip(keys, values, strict=True)))
            for values in itertools.product(*(body.grid[k] for k in keys))
        ]
        common = self.service.prepare(body.base)
        commands = []
        for index, values in enumerate(combinations):
            if len(common["bars"]) <= strategy.required_bars(values):
                raise ValueError("实验参数组合超出所选数据的预热容量")
            payload = {key: value for key, value in common.items() if key != "input_checksum"}
            payload["request"] = common["request"] | {"parameters": values}
            payload["input_checksum"] = hashlib.sha256(canonical(payload)).hexdigest()
            commands.append((f"experiment:{identifier}:{index}", KIND, payload))
        try:
            with self.engine.begin() as conn:
                records = self.service.tasks.submit_batch(conn, commands)
                conn.execute(
                    experiments.insert().values(
                        id=identifier,
                        owner=owner,
                        name=body.name,
                        created_at=time.time(),
                        spec=spec,
                        checksum=hashlib.sha256(
                            canonical([spec, [r["id"] for r in records]])
                        ).hexdigest(),
                        runs=[r["id"] for r in records],
                    )
                )
        except IntegrityError:
            existing = self.get(owner, identifier)
            if canonical(existing["spec"]) != canonical(spec):
                raise Conflict("实验命令标识已用于不同输入") from None
        return self.get(owner, identifier)

    def cancel(self, owner, identifier):
        with self.engine.begin() as conn:
            row = (
                conn.execute(
                    select(experiments).where(
                        experiments.c.id == identifier, experiments.c.owner == owner
                    )
                )
                .mappings()
                .first()
            )
            if row is None:
                raise KeyError(identifier)
            self.service.tasks.cancel_batch(conn, row["runs"])
        return self.get(owner, identifier)
