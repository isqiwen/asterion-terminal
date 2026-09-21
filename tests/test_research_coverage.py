from configuration_support import set_token
from credential_helpers import provider_secrets
from rules_support import rule_access, rule_version
from storage_support import data_store, domain_tasks, research_store, scheduler

from asterion.data.public import VersionAccess, VersionReader
from asterion.distribution import strategy_catalog
from asterion.platform.tasks.execution import ExecutionContext
from asterion.research.strategies import STRATEGY_RESOURCE

"""Research must bind the exact immutable report, never a latest/report UI hint."""

from datetime import date
from uuid import uuid4

import pytest
from sqlalchemy import create_engine
from test_coverage import calendar, contracts
from test_cumulative import publish
from test_research import config

from asterion.data.coverage import CoverageRequest
from asterion.data.sync import DataSync
from asterion.platform.store import metadata
from asterion.research.service import Backtests
from asterion.research.worker import execute


@pytest.fixture
def evidence(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/research-coverage.db")
    metadata.create_all(engine)
    sync = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path,
        provider_secrets("research-coverage-test-master"),
    )
    set_token(sync, "tushare", "offline-test-token")
    contracts(sync)
    calendar(sync, {f"2024-01-{day:02}": 1 for day in range(2, 7)})
    daily = publish(sync, ["2024-01-02", "2024-01-03", "2024-01-04"])
    body = config().model_copy(
        update={
            "version_id": daily["id"],
            "rules": rule_version("SHFE.rb2610"),
            "start": date(2024, 1, 2),
            "end": date(2024, 1, 4),
            "coverage_policy": "require_complete",
            "coverage_note": "",
        }
    )
    service = Backtests(
        research_store(engine),
        domain_tasks(research_store(engine), "research"),
        version_access(engine, tmp_path),
        rule_access(engine, "SHFE.rb2610"),
        strategy_catalog(),
    )
    yield sync, service, body
    engine.dispose()


def report(sync, body):
    return sync.coverage.check(body.version_id, CoverageRequest(start=body.start, end=body.end))


def test_strict_requires_matching_complete_report_and_freezes_it(evidence):
    sync, service, body = evidence
    with pytest.raises(ValueError, match="需要所选版本"):
        service.submit(body)
    checked = report(sync, body)
    assert checked["status"] == "COVERED"
    body = body.model_copy(update={"coverage_report_id": checked["id"]})
    job = service.submit(body)
    claimed = scheduler(sync.engine).claim("research")
    original_bytes = execute(
        ExecutionContext((STRATEGY_RESOURCE,), {STRATEGY_RESOURCE: strategy_catalog()}),
        claimed["payload"],
    )[0]
    service.publish(job["id"], claimed["token"], original_bytes)
    # Later calendar revisions and daily corrections cannot rewrite the saved report.
    calendar(sync, {"2024-01-03": 0})
    publish(sync, ["2024-01-03"], price=3210)
    assert service.get(job["id"])["coverage"] == checked
    assert service.submit(body)["id"] == job["id"]
    service.rerun(job["id"], str(uuid4()))
    replay = scheduler(sync.engine).claim("again")
    assert replay["payload"] == claimed["payload"]
    assert (
        execute(
            ExecutionContext((STRATEGY_RESOURCE,), {STRATEGY_RESOURCE: strategy_catalog()}),
            replay["payload"],
        )[0]
        == original_bytes
    )


@pytest.mark.parametrize("change", ["range", "version", "missing"])
def test_wrong_report_cannot_be_bypassed_by_exploration(evidence, change):
    sync, service, body = evidence
    checked = report(sync, body)
    patch = {
        "coverage_report_id": checked["id"],
        "coverage_policy": "allow_incomplete",
        "coverage_note": "explicit exploratory reason",
    }
    if change == "range":
        patch["end"] = date(2024, 1, 5)
    if change == "version":
        patch["version_id"] = publish(sync, ["2024-01-05"])["id"]
    if change == "missing":
        patch["coverage_report_id"] = "missing-report"
    with pytest.raises(ValueError, match="覆盖报告"):
        service.submit(body.model_copy(update=patch))


def test_gap_requires_exploration_reason_and_report_is_preserved(evidence):
    sync, service, body = evidence
    body = body.model_copy(update={"end": date(2024, 1, 5)})
    checked = report(sync, body)
    assert checked["status"] == "GAPS"
    body = body.model_copy(update={"coverage_report_id": checked["id"]})
    with pytest.raises(ValueError, match="需要所选版本"):
        service.submit(body)
    body = body.model_copy(update={"coverage_policy": "allow_incomplete"})
    with pytest.raises(ValueError, match="填写"):
        service.submit(body)
    job = service.submit(body.model_copy(update={"coverage_note": "观察缺失日线对研究的影响"}))
    assert service.get(job["id"])["coverage"]["counts"]["GAP"] == 1


def test_conflicts_are_blocked_even_in_exploration(evidence):
    sync, service, body = evidence
    calendar(sync, {"2024-01-03": 0})
    checked = report(sync, body)
    assert checked["status"] == "CONFLICT"
    with pytest.raises(ValueError, match="冲突"):
        service.submit(
            body.model_copy(
                update={
                    "coverage_report_id": checked["id"],
                    "coverage_policy": "allow_incomplete",
                    "coverage_note": "cannot override conflict",
                }
            )
        )


def version_access(engine, root):
    reader = VersionReader(data_store(engine), root)
    return VersionAccess(reader.read, reader.coverage)
