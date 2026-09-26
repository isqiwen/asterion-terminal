from decimal import Decimal
from uuid import uuid4

import pytest
from asterion_bindings.database import create_engine
from asterion_bindings.execution import ExecutionFactory
from asterion_bindings.task_repository import Conflict
from asterion_bindings.tasks import ExecutionContext
from fastapi.testclient import TestClient
from import_identity_support import import_identity
from import_support import import_options, import_payload, publish_import
from rules_support import rule_access, rule_version
from storage_support import data_store, domain_tasks, raw_engine, research_store, scheduler

from asterion.api.app import create_app
from asterion.data.library import DataLibrary
from asterion.data.public import VersionAccess, VersionReader
from asterion.distribution import strategy_catalog
from asterion.platform.config import Settings
from asterion.platform.serialization import canonical
from asterion.platform.store import metadata
from asterion.research.engine import ENGINE, BacktestRequest, calculate
from asterion.research.execution import EXECUTION
from asterion.research.service import Backtests
from asterion.research.strategies import STRATEGY_RESOURCE
from asterion.research.worker import execute


def config(**kwargs):
    return BacktestRequest(
        command_id=str(uuid4()),
        version_id="test-version",
        start="2024-01-01",
        end="2024-01-10",
        strategy=strategy_catalog().list()[0].identity,
        parameters={"fast": 1, "slow": 2},
        capital="1000",
        rules=rule_version(),
        slippage_ticks=1,
        assumption="historical-close-unverified-calendar",
        coverage_policy="allow_incomplete",
        coverage_note="离线测试未提供日历依据",
        **kwargs,
    )


def payload():
    # At Jan 2 close the first long signal is known; cannot buy before Jan 3 open.
    prices = [(10, 10), (10, 12), (20, 21), (22, 18), (17, 17)]
    bars = [
        {
            "contract": "SHFE.rb2405",
            "contract_id": "SHFE.RB.202405.20230516",
            "trading_day": f"2024-01-{i + 1:02}",
            "open": str(o),
            "close": str(c),
            "settle": str(c),
        }
        for i, (o, c) in enumerate(prices)
    ]
    return {
        "engine": ENGINE,
        "request": config().model_dump(mode="json", exclude={"command_id"}),
        "bars": bars,
    }


def test_next_open_and_mark_to_market_exact_accounting():
    result = calculate(payload(), strategy_catalog(), ExecutionFactory())
    assert [(f["day"], f["side"], f["price"]) for f in result["fills"]] == [
        ("2024-01-03", "BUY", "21"),
        ("2024-01-05", "SELL", "16"),
    ]
    # Buy 21, sell 16, multiplier 10, two commissions of 2 = -54.
    assert result["summary"]["final_equity"] == "946"
    assert result["summary"]["fees"] == "4"
    assert Decimal(result["summary"]["max_drawdown"]) == Decimal("0.054")
    assert [p["position"] for p in result["curve"]] == [0, 0, 1, 1, 0]
    assert canonical(result) == canonical(
        calculate(payload(), strategy_catalog(), ExecutionFactory())
    )


def test_future_prices_do_not_change_previous_decisions():
    first = payload()
    second = payload()
    second["bars"][-1]["close"] = "10000"
    assert (
        calculate(first, strategy_catalog(), ExecutionFactory())["curve"][:-1]
        == calculate(second, strategy_catalog(), ExecutionFactory())["curve"][:-1]
    )
    assert (
        calculate(first, strategy_catalog(), ExecutionFactory())["fills"]
        == calculate(second, strategy_catalog(), ExecutionFactory())["fills"]
    )


def test_margin_rejection_and_open_position_valuation():
    value = payload()
    value["request"]["capital"] = "20"
    result = calculate(value, strategy_catalog(), ExecutionFactory())
    assert not result["fills"]
    assert result["events"][0]["reason"] == "资金不足，拒绝开仓"
    value = payload()
    value["bars"] = value["bars"][:3]
    result = calculate(value, strategy_catalog(), ExecutionFactory())
    assert result["summary"]["open_lots"] == 1
    assert result["summary"]["final_equity"] == "998"
    assert result["summary"]["fees"] == "2"  # No invented last-day liquidation.


def test_gap_margin_call_closes_before_intraday_recovery():
    value = payload()
    value["request"].update(capital="50", slippage_ticks=0)
    from asterion_bindings.rules import RuleSpec, rule_id

    spec = value["request"]["rules"]["spec"]
    spec["periods"][0].update(open_fee="0", close_fee="0")
    value["request"]["rules"]["id"] = rule_id(RuleSpec.model_validate(spec))
    value["bars"][3].update(open="16", close="100")
    result = calculate(value, strategy_catalog(), ExecutionFactory())
    assert result["fills"][1]["day"] == "2024-01-04"
    assert result["fills"][1]["reason"] == "保证金不足平仓"
    assert result["curve"][3]["position"] == 0


@pytest.fixture
def services(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/test.db")
    metadata.create_all(engine)
    tasks = scheduler(engine)
    (tmp_path / "data").mkdir(exist_ok=True)
    csv = "contract,trading_day,open,high,low,close,vol,settle\n" + "\n".join(
        f"SHFE.rb2405,2024-01-{i + 1:02},{o},{max(o, c)},{min(o, c)},{c},100,{c}"
        for i, (o, c) in enumerate([(10, 10), (10, 12), (20, 21), (22, 18), (17, 17)])
    )
    options = import_options(
        import_identity("SHFE.rb2405"), type_id="futures.daily", frequency="1d", source_id="local"
    )
    tasks.submit("import", "data.import_csv", import_payload(csv, options, "test history"))
    job = tasks.claim("test")
    publish_import(engine, tmp_path / "data", job)
    service = Backtests(
        research_store(engine),
        domain_tasks(research_store(engine), "research"),
        version_access(engine, tmp_path / "data"),
        rule_access(engine),
        strategy_catalog(),
        ExecutionFactory(),
    )
    version = DataLibrary(data_store(engine), tmp_path / "data").list(
        type_id="futures.daily", layer="STANDARD"
    )["items"][0]
    from reference_support import research_coverage

    report = research_coverage(
        data_store(engine), tmp_path / "data", version["id"], "2024-01-01", "2024-01-10"
    )
    yield (
        service,
        config().model_copy(
            update={"version_id": version["id"], "coverage_report_id": report["id"]}
        ),
        tmp_path,
    )
    engine.dispose()


def test_submission_freezes_inputs_rerun_and_atomic_publication(services):
    service, request, _ = services
    submitted = service.submit(request)
    assert service.submit(request)["id"] == submitted["id"]
    assert [b["settle"] for b in submitted["payload"]["bars"]] == ["10", "12", "21", "18", "17"]
    with pytest.raises(Conflict):
        service.submit(request.model_copy(update={"parameters": {"fast": 2, "slow": 3}}))
    job = scheduler(service.engine).claim("test")
    content, _ = execute(
        ExecutionContext(
            (
                STRATEGY_RESOURCE,
                EXECUTION,
            ),
            {STRATEGY_RESOURCE: strategy_catalog(), EXECUTION: ExecutionFactory()},
        ),
        job["payload"],
    )
    with pytest.raises(ValueError, match="不一致"):
        service.publish(job["id"], job["token"], b"{}")
    result = service.publish(job["id"], job["token"], content)
    assert result["net_profit"] == "-54"
    assert service.publish(job["id"], job["token"], content) == result
    assert service.get(job["id"])["output"]["summary"]["final_equity"] == "946"
    again = service.rerun(job["id"], "rerun")
    claimed = scheduler(service.engine).claim("again")
    assert claimed["payload"] == job["payload"]
    assert (
        execute(
            ExecutionContext(
                (
                    STRATEGY_RESOURCE,
                    EXECUTION,
                ),
                {STRATEGY_RESOURCE: strategy_catalog(), EXECUTION: ExecutionFactory()},
            ),
            claimed["payload"],
        )[0]
        == content
    )
    assert again["id"] != job["id"]
    scheduler(service.engine).cancel(again["id"])
    with pytest.raises(Conflict):
        service.publish(again["id"], claimed["token"], content)


def test_invalid_inputs_and_cancelled_publication(services):
    service, request, _ = services
    with pytest.raises(ValueError, match="多于策略预热"):
        service.submit(request.model_copy(update={"parameters": {"fast": 1, "slow": 20}}))
    service.submit(request)
    job = scheduler(service.engine).claim("worker")
    scheduler(service.engine).cancel(job["id"])
    with pytest.raises(Conflict):
        service.publish(
            job["id"],
            job["token"],
            execute(
                ExecutionContext(
                    (
                        STRATEGY_RESOURCE,
                        EXECUTION,
                    ),
                    {STRATEGY_RESOURCE: strategy_catalog(), EXECUTION: ExecutionFactory()},
                ),
                job["payload"],
            )[0],
        )
    assert service.get(job["id"])["output"] is None


def test_api_auth_and_task_roundtrip(services):
    service, request, root = services
    app = create_app(
        Settings(
            token="test-research-token-at-least-24-chars",
            data_root=root / "data",
            require_account=False,
        ),
        raw_engine(service.engine),
    )
    client = TestClient(app)
    assert client.get("/api/v1/research/runs").status_code == 401
    client.headers["Authorization"] = "Bearer test-research-token-at-least-24-chars"
    response = client.post("/api/v1/research/runs", json=request.model_dump(mode="json"))
    assert response.status_code == 202
    job = scheduler(service.engine).claim("api-test")
    published = client.post(
        f"/api/v1/jobs/{job['id']}/publish-research",
        content=execute(
            ExecutionContext(
                (
                    STRATEGY_RESOURCE,
                    EXECUTION,
                ),
                {STRATEGY_RESOURCE: strategy_catalog(), EXECUTION: ExecutionFactory()},
            ),
            job["payload"],
        )[0],
        headers={"X-Lease-Token": job["token"]},
    )
    assert published.status_code == 200
    listing = client.get("/api/v1/research/runs").json()
    assert listing[0]["result"]["net_profit"] == "-54"
    assert "bars" not in listing[0]
    assert client.get(f"/api/v1/research/runs/{job['id']}").json()["output"]["fills"]
    assert client.get("/api/v1/research/runs/missing").status_code == 404
    # Account gate remains mandatory in production.
    app = create_app(
        Settings(
            token="test-research-token-at-least-24-chars",
            data_root=root / "data",
            require_account=True,
        ),
        raw_engine(service.engine),
    )
    gated = TestClient(
        app, headers={"Authorization": "Bearer test-research-token-at-least-24-chars"}
    )
    assert gated.get("/api/v1/research/runs").status_code in (401, 403, 423)


def test_single_job_status_is_private_and_not_limited_to_recent_list(services):
    service, request, _ = services
    submitted = service.submit(request)
    claimed = scheduler(service.engine).claim("private-worker")
    for index in range(101):
        scheduler(service.engine).submit(f"later-{index}", "test.only", {"private": "payload"})
    assert submitted["id"] not in {j["id"] for j in scheduler(service.engine).list()}
    status = scheduler(service.engine).get(submitted["id"])
    assert status["state"] == "RUNNING"
    assert "token" not in status and "payload" not in status
    assert claimed["token"]


def test_rerun_rejects_incomplete_frozen_contract_without_mutating_input(services):
    import hashlib

    from sqlalchemy import select

    from asterion.platform.store import jobs

    service, request, _ = services
    job = service.submit(request)
    payload = dict(job["payload"])
    payload.pop("coverage")
    payload["input_checksum"] = hashlib.sha256(
        canonical({k: v for k, v in payload.items() if k != "input_checksum"})
    ).hexdigest()
    with raw_engine(service.engine).begin() as conn:
        conn.execute(jobs.update().where(jobs.c.id == job["id"]).values(payload=payload))
    with pytest.raises(ValueError, match="固定输入契约"):
        service.rerun(job["id"], "unsupported-rerun")
    with pytest.raises(ValueError, match="固定输入契约"):
        service.get(job["id"])
    with raw_engine(service.engine).connect() as conn:
        assert (
            conn.execute(select(jobs.c.payload).where(jobs.c.id == job["id"])).scalar_one()
            == payload
        )


def version_access(engine, root):
    reader = VersionReader(data_store(engine), root)
    return VersionAccess(reader.read, reader.coverage, reader.scan)


def test_submission_rejects_missing_settlement_before_creating_job(services):

    service, request, _ = services
    versions = service.versions

    def missing(request):
        import pyarrow as pa

        from asterion.data.scan_public import ScanResult

        result = versions.scan(request)

        def batches():
            for batch in result.batches:
                rows = batch.to_pylist()
                rows[0]["settle"] = None
                yield pa.RecordBatch.from_pylist(rows)

        return ScanResult(result.version, batches())

    service.versions = VersionAccess(versions.read, versions.coverage, missing)
    with pytest.raises(ValueError, match="有效结算价"):
        service.submit(request)
    assert service.list() == []
