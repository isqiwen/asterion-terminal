from asterion_bindings.execution import ExecutionFactory
from asterion_bindings.tasks import ExecutionContext
from import_identity_support import import_identity
from rules_support import rule_access, rule_version
from storage_support import data_store, domain_tasks, research_store, scheduler

from asterion.data.public import VersionAccess, VersionReader
from asterion.distribution import strategy_catalog
from asterion.research.execution import EXECUTION
from asterion.research.strategies import STRATEGY_RESOURCE

"""File coverage requires explicit immutable references and never queues source refills."""

from datetime import date
from uuid import uuid4

import pytest
from import_support import import_options, import_payload, publish_import
from sqlalchemy import select
from test_coverage import calendar, contracts, sync  # noqa: F401
from test_research import config

from asterion.data.coverage import CoverageRequest
from asterion.data.library import versions
from asterion.data.providers.public import ProviderError
from asterion.research.packages import ResearchPackages
from asterion.research.service import Backtests
from asterion.research.worker import execute


@pytest.fixture
def local(request):
    service = request.getfixturevalue("sync")
    root = service.library.root
    raw = "contract,trading_day,open,high,low,close,vol,settle\n" + "\n".join(
        f"SHFE.rb2610,2024-01-0{d},10,12,9,11,100,10.5" for d in (2, 3, 4)
    )
    options = import_options(
        import_identity("SHFE.rb2610"), type_id="futures.daily", frequency="1d", source_id="history"
    )
    service.tasks.submit(
        str(uuid4()), "data.import_csv", import_payload(raw, options, "file-history")
    )
    job = scheduler(service.engine).claim("import")
    publish_import(service.engine, root, job)
    daily = service.library.list(type_id="futures.daily", layer="STANDARD")["items"][0]
    contract = contracts(service)
    cal = calendar(service, {f"2024-01-0{d}": 1 for d in (2, 3, 4, 5)})
    req = CoverageRequest(
        start="2024-01-02",
        end="2024-01-04",
        reference_policy="explicit_external",
        reference_symbol="RB2610.SHF",
        calendar_version_id=cal["id"],
        contracts_version_id=contract["id"],
        reference_note="同交易所实际合约，按此历史日历核对",
    )
    return service, daily, req


def test_explicit_local_evidence_strict_research_and_portable_replay(local):
    service, daily, req = local
    with pytest.raises(ProviderError, match="显式关联"):
        service.coverage.check(daily["id"], CoverageRequest(start=req.start, end=req.end))
    checked = service.coverage.check(daily["id"], req)
    assert checked["status"] == "COVERED"
    assert checked["source"] == "local_file"
    assert checked["references"]["calendar"]["source"] == "tushare"
    assert not checked["refill_supported"] and not checked["refill_ranges"]
    assert service.coverage.check(daily["id"], req) == checked
    research = Backtests(
        research_store(service.engine),
        domain_tasks(research_store(service.engine), "research"),
        version_access(service.engine, service.library.root),
        rule_access(service.engine, "SHFE.rb2610"),
        strategy_catalog(),
        ExecutionFactory(),
    )
    body = config().model_copy(
        update={
            "version_id": daily["id"],
            "rules": rule_version("SHFE.rb2610"),
            "start": req.start,
            "end": req.end,
            "coverage_policy": "require_complete",
            "coverage_note": "",
            "coverage_report_id": checked["id"],
        }
    )
    job = research.submit(body)
    claimed = scheduler(service.engine).claim("research")
    research.publish(
        job["id"],
        claimed["token"],
        execute(
            ExecutionContext(
                (
                    STRATEGY_RESOURCE,
                    EXECUTION,
                ),
                {STRATEGY_RESOURCE: strategy_catalog(), EXECUTION: ExecutionFactory()},
            ),
            claimed["payload"],
        )[0],
    )
    package = ResearchPackages(research)
    assert package.receive("alice", package.export(job["id"], True))["can_replay"]
    calendar(service, {"2024-01-03": 0})
    assert service.coverage.check(daily["id"], req) == checked
    assert research.get(job["id"])["coverage"]["reference_note"] == req.reference_note
    with pytest.raises(ProviderError, match="不支持自动补齐"):
        service.coverage.refill(checked["id"], "no-cross-source")


def test_local_gaps_conflicts_and_wrong_reference(local):
    service, daily, req = local
    gap = service.coverage.check(daily["id"], req.model_copy(update={"end": date(2024, 1, 5)}))
    assert gap["status"] == "GAPS" and gap["counts"]["GAP"] == 1
    assert gap["refill_ranges"] == []
    closed = calendar(service, {"2024-01-03": 0})
    assert (
        service.coverage.check(
            daily["id"], req.model_copy(update={"calendar_version_id": closed["id"]})
        )["status"]
        == "CONFLICT"
    )
    with pytest.raises(ProviderError):
        service.coverage.check(
            daily["id"], req.model_copy(update={"calendar_version_id": daily["id"]})
        )
    with pytest.raises(ProviderError, match="固定版本"):
        service.coverage.check(daily["id"], req.model_copy(update={"use_latest_daily": True}))
    with service.engine.begin() as conn:
        manifest = dict(
            conn.execute(
                select(versions.c.manifest).where(versions.c.id == req.contracts_version_id)
            ).scalar_one()
        )
        manifest["scope"] = {**manifest["scope"], "exchange": "DCE"}
        conn.execute(
            versions.update()
            .where(versions.c.id == req.contracts_version_id)
            .values(manifest=manifest)
        )
    with pytest.raises(ProviderError, match="交易所"):
        service.coverage.check(daily["id"], req)


@pytest.mark.parametrize(
    "missing", ["calendar_version_id", "contracts_version_id", "reference_note"]
)
def test_external_mapping_requires_complete_explicit_input(missing):
    body = {
        "start": "2024-01-02",
        "end": "2024-01-04",
        "reference_policy": "explicit_external",
        "calendar_version_id": "calendar",
        "contracts_version_id": "contracts",
        "reference_note": "reason",
    }
    del body[missing]
    with pytest.raises(ValueError, match="外部依据"):
        CoverageRequest.model_validate(body)


def version_access(engine, root):
    reader = VersionReader(data_store(engine), root)
    return VersionAccess(reader.read, reader.coverage, reader.scan)
