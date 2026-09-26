"""Read-only validation and recomputation of fixed research results."""

import hashlib
from collections.abc import Callable, Iterator
from dataclasses import dataclass

from asterion_bindings.execution import ExecutionFactory
from asterion_bindings.recovery import BackupCheck
from sqlalchemy import text

from asterion.platform.serialization import canonical
from asterion.research.engine import calculate
from asterion.research.strategies import StrategyCatalog


@dataclass(frozen=True)
class ResearchBackup:
    strategies: StrategyCatalog
    validate_external: Callable[[dict], None]
    results: Callable[[], Iterator[tuple[str, dict]]]
    inputs: Callable[[], Iterator[dict]]
    reproductions: Callable[[], Iterator[tuple[dict, str]]]
    experiments: Callable[[], Iterator[tuple[dict, list, str, set]]]
    validations: Callable[[], Iterator[tuple[dict, set, set]]]
    execution: ExecutionFactory


def load_evidence(conn, strategies, execution, validate_external):
    def results():
        yield from (
            (checksum, output)
            for checksum, output in conn.execute(
                text("SELECT checksum, output FROM research_results")
            )
        )

    def inputs():
        yield from conn.execute(
            text("SELECT payload FROM jobs WHERE kind='research.backtest'")
        ).scalars()

    def reproductions():
        yield from (
            (payload, checksum)
            for payload, checksum in conn.execute(
                text(
                    "SELECT j.payload, r.checksum FROM jobs j JOIN research_results r ON r.job_id=j.id"
                )
            )
        )

    def experiments():
        known = set(
            conn.execute(text("SELECT id FROM jobs WHERE kind='research.backtest'")).scalars()
        )
        for spec, runs, checksum in conn.execute(
            text("SELECT spec, runs, checksum FROM research_experiments")
        ):
            yield spec, runs, checksum, known

    def validations():
        runs = set(
            conn.execute(text("SELECT id FROM jobs WHERE kind='research.backtest'")).scalars()
        )
        groups = set(conn.execute(text("SELECT id FROM research_experiments")).scalars())
        for row in conn.execute(text("SELECT * FROM research_validations")).mappings():
            yield dict(row), runs, groups

    return ResearchBackup(
        strategies,
        validate_external,
        results,
        inputs,
        reproductions,
        experiments,
        validations,
        execution,
    )


def validate_backup(evidence: ResearchBackup):
    research_count = 0
    external_count = 0
    for checksum, output in evidence.results():
        if hashlib.sha256(canonical(output)).hexdigest() != checksum:
            raise ValueError("恢复后研究结果校验失败")
        research_count += 1
    for payload in evidence.inputs():
        expected = payload["input_checksum"]
        if (
            hashlib.sha256(
                canonical({k: v for k, v in payload.items() if k != "input_checksum"})
            ).hexdigest()
            != expected
        ):
            raise ValueError("恢复后研究输入校验失败")

    builtin_ids = {s.identity.id for s in evidence.strategies.list()}
    for payload, checksum in evidence.reproductions():
        from asterion.research.service import validate_input

        validate_input(payload)
        if payload["request"]["strategy"]["id"] not in builtin_ids:
            evidence.validate_external(payload["request"])
            external_count += 1
            continue
        if (
            hashlib.sha256(
                canonical(calculate(payload, evidence.strategies, evidence.execution))
            ).hexdigest()
            != checksum
        ):
            raise ValueError("恢复后研究结果复算不一致")
    for spec, runs, checksum, known in evidence.experiments():
        from asterion.research.experiments import ExperimentRequest

        ExperimentRequest.model_validate(spec)
        if (
            hashlib.sha256(canonical([spec, runs])).hexdigest() != checksum
            or len(set(runs)) != len(runs)
            or not set(runs) <= known
        ):
            raise ValueError("恢复后实验记录或任务关联校验失败")
    for record, runs, groups in evidence.validations():
        from asterion.research.validation import validate_record

        validate_record(record, runs, groups)
    return {"research_results": research_count, "research_external_not_recomputed": external_count}


check = BackupCheck(ResearchBackup, validate_backup)
