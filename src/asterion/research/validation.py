"""One immutable, later-period validation per parameter experiment."""

import hashlib
import time
from datetime import date
from typing import Literal

from asterion_bindings.task_repository import Conflict
from pydantic import BaseModel, ConfigDict, Field
from sqlalchemy import JSON, Column, Float, String, Table, select
from sqlalchemy.exc import IntegrityError

from asterion.platform.serialization import canonical
from asterion.platform.store import jobs, metadata
from asterion.research.engine import BacktestRequest
from asterion.research.experiments import Experiments
from asterion.research.service import KIND, validate_input

validations = Table(
    "research_validations",
    metadata,
    Column("experiment_id", String, primary_key=True),
    Column("owner", String, nullable=False),
    Column("created_at", Float, nullable=False),
    Column("run_id", String, nullable=False),
    Column("selection", JSON, nullable=False),
    Column("evidence", JSON, nullable=False),
    Column("checksum", String, nullable=False),
)


class Selection(BaseModel):
    model_config = ConfigDict(extra="forbid")
    source_run: str = Field(min_length=1, max_length=100)
    start: date
    end: date
    coverage_report_id: str | None
    coverage_policy: Literal["require_complete", "allow_incomplete"]
    coverage_note: str = Field(max_length=500)
    reason: str = Field(min_length=1, max_length=1000)


class Validations:
    def __init__(self, service):
        self.service = service
        self.experiments = Experiments(service)
        self.engine = service.engine
        self.engine.initialize(validations)

    def get(self, owner, identifier):
        self.experiments.get(owner, identifier)
        with self.engine.connect() as conn:
            row = (
                conn.execute(
                    select(validations).where(
                        validations.c.experiment_id == identifier, validations.c.owner == owner
                    )
                )
                .mappings()
                .first()
            )
        if row is None:
            return None
        record = dict(row)
        if checksum(record) != record["checksum"]:
            raise ValueError("验证选择记录校验失败")
        return record | {"validation": self.service.get(record["run_id"])}

    def submit(self, owner, identifier, body):
        experiment = self.experiments.get(owner, identifier)
        selection = body.model_dump(mode="json")
        old = self.get(owner, identifier)
        if old:
            if canonical(old["selection"]) != canonical(selection):
                raise Conflict("该实验已冻结验证选择，不能改选参数或验证区间")
            return old
        if not body.reason.strip():
            raise ValueError("请记录选择该参数组合的依据")
        if body.source_run not in experiment["runs"]:
            raise ValueError("必须从本实验选择已完成的研究运行")
        if any(item["state"] in {"QUEUED", "RUNNING"} for item in experiment["items"]):
            raise ValueError("请等待实验完成，或先取消剩余组合，再冻结选择")
        source = self.service.get(body.source_run)
        with self.engine.connect() as conn:
            validate_input(
                conn.execute(
                    select(jobs.c.payload).where(jobs.c.id == body.source_run)
                ).scalar_one()
            )
        if source["state"] != "SUCCEEDED" or source["output"] is None:
            raise ValueError("所选研究运行尚未成功")
        if body.start <= date.fromisoformat(source["request"]["end"]) or body.end < body.start:
            raise ValueError("验证区间必须完全晚于研究区间")
        request = BacktestRequest.model_validate(
            source["request"]
            | {
                "command_id": f"validation:{identifier}",
                "start": body.start,
                "end": body.end,
                "coverage_report_id": body.coverage_report_id,
                "coverage_policy": body.coverage_policy,
                "coverage_note": body.coverage_note,
            }
        )
        payload = self.service.prepare(request)
        warmup = self.service.strategies.resolve(request.strategy).required_bars(request.parameters)
        evidence = {
            "research": {
                "run_id": body.source_run,
                "request": source["request"],
                "input_checksum": source["input_checksum"],
                "summary": source["output"]["summary"],
                "result_checksum": source["result"]["checksum"],
            },
            "warmup_start": payload["bars"][0]["trading_day"],
            "warmup_end": payload["bars"][warmup - 1]["trading_day"],
            "evaluation_start": payload["bars"][warmup]["trading_day"],
            "evaluation_end": payload["bars"][-1]["trading_day"],
            "warmup_bars": warmup,
            "validation_input_checksum": payload["input_checksum"],
            "initial_position": "flat",
        }
        try:
            with self.engine.begin() as conn:
                run = self.service.tasks.submit_batch(conn, [(request.command_id, KIND, payload)])[
                    0
                ]
                record = {
                    "experiment_id": identifier,
                    "owner": owner,
                    "created_at": time.time(),
                    "run_id": run["id"],
                    "selection": selection,
                    "evidence": evidence,
                }
                conn.execute(validations.insert().values(**record, checksum=checksum(record)))
        except IntegrityError:
            old = self.get(owner, identifier)
            if not old or canonical(old["selection"]) != canonical(selection):
                raise Conflict("该实验已经冻结其他验证选择") from None
        return self.get(owner, identifier)

    def cancel(self, owner, identifier):
        record = self.get(owner, identifier)
        if record is None:
            raise KeyError(identifier)
        with self.engine.begin() as conn:
            self.service.tasks.cancel_batch(conn, [record["run_id"]])
        return self.get(owner, identifier)


def checksum(record):
    return hashlib.sha256(
        canonical(
            {
                key: record[key]
                for key in (
                    "experiment_id",
                    "owner",
                    "created_at",
                    "run_id",
                    "selection",
                    "evidence",
                )
            }
        )
    ).hexdigest()


def validate_record(record, known_runs, known_experiments):
    selection = Selection.model_validate(record["selection"])
    if (
        checksum(record) != record["checksum"]
        or record["experiment_id"] not in known_experiments
        or not {record["run_id"], selection.source_run} <= known_runs
    ):
        raise ValueError("验证选择或运行关联校验失败")
