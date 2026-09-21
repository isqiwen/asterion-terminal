from dataclasses import replace

from configuration_support import set_token
from credential_helpers import provider_secrets
from storage_support import data_store, domain_tasks, raw_engine, scheduler

"""Coverage must be reproducible and never turn missing evidence into trading-day gaps."""

import json
import os
from concurrent.futures import ThreadPoolExecutor
from datetime import UTC, date, datetime
from uuid import uuid4

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine, select, text
from test_cumulative import pending, publish

from asterion.api.app import create_app
from asterion.data.coverage import CoverageRequest
from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.data.sync import DataSync
from asterion.platform.config import Settings
from asterion.platform.serialization import canonical
from asterion.platform.store import jobs, metadata
from asterion.platform.tasks.service import Conflict

MASTER = "synthetic-coverage-master-key"


@pytest.fixture
def sync(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/coverage.db")
    metadata.create_all(engine)
    service = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path,
        provider_secrets(MASTER),
    )
    set_token(service, "tushare", "synthetic-only-token")
    return service


def contracts(sync, listed="20240102", delisted="20240106"):
    req = SyncRequest(
        command_id=str(uuid4()), provider="tushare", dataset="contracts", exchange="SHFE"
    )
    sync.submit(req)
    job = scheduler(sync.engine).claim("reference")
    part = Tushare().plan(req)[0]
    row = {
        "ts_code": "RB2610.SHF",
        "exchange": "SHFE",
        "name": "合成合约",
        "fut_code": "RB",
        "d_month": "202610",
        "last_ddate": None,
        "list_date": listed,
        "delist_date": delisted,
        "per_unit": 10,
        "trade_unit": "吨",
        "quote_unit": "元/吨",
    }
    content = canonical(
        [
            {
                "partition": part.model_dump(),
                "rows": [{field: row.get(field) for field in part.fields}],
                "observed_at": datetime.now(UTC).isoformat(),
            }
        ]
    )
    return sync.publish(job["id"], job["token"], content)


def calendar(sync, days):
    job, content = pending(sync, list(days), dataset="calendar")
    evidence = json.loads(content)
    for part in evidence:
        for row in part["rows"]:
            compact = row["cal_date"]
            row["is_open"] = days[f"{compact[:4]}-{compact[4:6]}-{compact[6:]}"]
    return sync.publish(job["id"], job["token"], canonical(evidence))


def setup_range(sync):
    reference = contracts(sync)
    cal = calendar(sync, {f"2024-01-{day:02}": int(day != 6) for day in range(1, 9)})
    daily = publish(sync, ["2024-01-02", "2024-01-04"])
    return daily, cal, reference


def request(**changes):
    return CoverageRequest.model_validate({"start": "2024-01-01", "end": "2024-01-08"} | changes)


def test_fixed_report_separates_listing_closure_and_gaps(sync):
    daily, cal, reference = setup_range(sync)
    report = sync.coverage.check(daily["id"], request())
    assert report["status"] == "GAPS"
    assert report["counts"] == {"OUTSIDE_LISTING": 3, "PRESENT": 2, "GAP": 2, "CLOSED": 1}
    assert report["calendar_version_id"] == cal["id"]
    assert report["contracts_version_id"] == reference["id"]
    assert report["refill_ranges"] == [
        {"start": "2024-01-03", "end": "2024-01-03"},
        {"start": "2024-01-05", "end": "2024-01-05"},
    ]
    assert sync.coverage.check(daily["id"], request()) == report
    assert sync.coverage.latest(daily["id"]) == report
    # Later reference revisions do not rewrite a saved report.
    revised = calendar(sync, {"2024-01-03": 0})
    assert sync.coverage.get(report["id"]) == report
    new = sync.coverage.check(daily["id"], request())
    assert new["id"] != report["id"]
    assert new["calendar_version_id"] == revised["id"]
    assert new["counts"]["GAP"] == 1
    fixed = sync.coverage.check(
        daily["id"], request(calendar_version_id=cal["id"], contracts_version_id=reference["id"])
    )
    assert fixed == report


def test_unknown_calendar_does_not_create_gaps_and_reference_is_pinned(sync):
    daily = publish(sync, ["2024-01-02"])
    report = sync.coverage.check(daily["id"], request())
    assert report["status"] == "UNCONFIRMED"
    assert report["counts"] == {"UNKNOWN_CALENDAR": 8}
    assert not report["refill_ranges"]
    basis = report["contracts_version_id"]
    contracts(sync, delisted=None)
    later = sync.coverage.check(daily["id"], request())
    assert later["contracts_version_id"] == basis
    assert later["counts"] == {"UNKNOWN_CALENDAR": 8}


def test_conflicting_bar_on_closed_day_blocks_refill(sync):
    daily, _, _ = setup_range(sync)
    calendar(sync, {"2024-01-02": 0})
    report = sync.coverage.check(daily["id"], request())
    assert report["status"] == "CONFLICT"
    assert report["counts"]["CONFLICT"] == 1
    assert report["counts"]["GAP"] == 2
    assert not report["refill_ranges"]
    assert sync.coverage.refill(report["id"], "conflict")["jobs"] == []


def test_today_is_pending_even_with_calendar_evidence(sync, monkeypatch):
    daily, _, _ = setup_range(sync)
    monkeypatch.setattr("asterion.data.coverage.today", lambda: date(2024, 1, 5))
    report = sync.coverage.check(daily["id"], request(end="2024-01-05"))
    assert report["counts"]["PENDING"] == 1
    assert report["days"][-1]["status"] == "PENDING"
    assert all(part["end"] < "2024-01-05" for part in report["refill_ranges"])
    with pytest.raises(ValueError):
        request(end="2024-01-06")


def test_refill_rechecks_latest_skips_filled_dates_and_is_idempotent(sync):
    daily, _, _ = setup_range(sync)
    report = sync.coverage.check(daily["id"], request())
    filled = publish(sync, ["2024-01-03"])
    result = sync.coverage.refill(report["id"], "fill-once")
    assert result["skipped_gap_days"] == 1
    assert len(result["jobs"]) == 1
    assert sync.coverage.refill(report["id"], "fill-once") == result
    assert sync.coverage.get(result["checked_report_id"])["daily_version_id"] == filled["id"]
    with sync.engine.connect() as conn:
        queued = (
            conn.execute(select(jobs).where(jobs.c.id == result["jobs"][0]["id"])).mappings().one()
        )
    assert (
        queued["payload"]["request"]["start"] == queued["payload"]["request"]["end"] == "2024-01-05"
    )
    assert queued["payload"]["coverage_report_id"] == result["checked_report_id"]
    new_report = sync.coverage.check(filled["id"], request())
    with pytest.raises(Conflict):
        sync.coverage.refill(new_report["id"], "fill-once")
    job = scheduler(sync.engine).claim("refill-worker")
    part = Tushare().plan(SyncRequest.model_validate(job["payload"]["request"]))[0]
    values = {
        "ts_code": "RB2610.SHF",
        "trade_date": "20240105",
        "open": 3200,
        "high": 3210,
        "low": 3190,
        "close": 3200,
        "vol": 100,
    }
    content = canonical(
        [
            {
                "partition": part.model_dump(),
                "rows": [{field: values.get(field) for field in part.fields}],
                "observed_at": datetime.now(UTC).isoformat(),
            }
        ]
    )
    complete = sync.publish(job["id"], job["token"], content)
    assert complete["manifest"]["coverage_report_id"] == result["checked_report_id"]
    checked = sync.coverage.check(daily["id"], request(use_latest_daily=True))
    assert checked["status"] == "COVERED"
    assert checked["counts"]["PRESENT"] == 4
    assert not sync.coverage.refill(report["id"], "already-complete")["jobs"]
    assert sync.coverage.get(report["id"])["status"] == "GAPS"


def test_empty_refill_stays_a_gap_and_retries_keep_context(sync):
    daily, _, _ = setup_range(sync)
    report = sync.coverage.check(daily["id"], request(start="2024-01-03", end="2024-01-03"))
    sync.coverage.refill(report["id"], "empty")
    job = scheduler(sync.engine).claim("empty-refill")
    part = Tushare().plan(SyncRequest.model_validate(job["payload"]["request"]))[0]
    with pytest.raises(ProviderError, match="EMPTY_UNCONFIRMED"):
        sync.publish(
            job["id"],
            job["token"],
            canonical(
                [
                    {
                        "partition": part.model_dump(),
                        "rows": [],
                        "observed_at": datetime.now(UTC).isoformat(),
                    }
                ]
            ),
        )
    scheduler(sync.engine).fail(job["id"], job["token"], "EMPTY_UNCONFIRMED")
    retry = sync.retry(job["id"], "retry-empty", resume=True)
    with sync.engine.connect() as conn:
        payload = conn.execute(select(jobs.c.payload).where(jobs.c.id == retry["id"])).scalar_one()
    assert payload["coverage_report_id"] == report["id"]
    assert (
        sync.coverage.check(
            daily["id"], request(start="2024-01-03", end="2024-01-03", use_latest_daily=True)
        )["counts"]["GAP"]
        == 1
    )


def test_refill_submission_is_atomic_and_credentials_are_required(sync, monkeypatch):
    daily, _, _ = setup_range(sync)
    report = sync.coverage.check(daily["id"], request())
    before = len(scheduler(sync.engine).list())
    set_token(sync, "tushare", "")
    with pytest.raises(ProviderError, match="Token"):
        sync.coverage.refill(report["id"], "retry-after-config")
    assert len(scheduler(sync.engine).list()) == before
    set_token(sync, "tushare", "synthetic-only-token")
    original = sync.tasks.submit_batch

    def fail(conn, commands):
        original(conn, commands[:1])
        raise OSError("synthetic interrupted submission")

    with monkeypatch.context() as patch:
        patch.setattr(sync, "tasks", replace(sync.tasks, submit_batch=fail))
        with pytest.raises(OSError):
            sync.coverage.refill(report["id"], "retry-after-config")
    assert len(scheduler(sync.engine).list()) == before
    assert len(sync.coverage.refill(report["id"], "retry-after-config")["jobs"]) == 2


def test_reference_type_and_corrupt_evidence_are_rejected(sync):
    daily, cal, reference = setup_range(sync)
    with pytest.raises(ProviderError, match="对应类型"):
        sync.coverage.check(daily["id"], request(calendar_version_id=reference["id"]))
    checksum = cal["manifest"]["partitions"][0]["checksum"]
    (sync.root / "artifacts" / f"{checksum}.parquet").write_bytes(b"corrupt")
    with pytest.raises(ProviderError, match="校验和"):
        sync.coverage.check(daily["id"], request())


def test_coverage_api_account_boundary_and_roundtrip(sync):
    daily, _, _ = setup_range(sync)
    settings = Settings(token=MASTER, data_root=sync.root)
    client = TestClient(create_app(settings, raw_engine(sync.engine)))
    client.headers["Authorization"] = f"Bearer {MASTER}"
    path = f"/api/v1/data/versions/{daily['id']}/coverage"
    assert client.get(path).json() is None
    response = client.post(path, json=request().model_dump(mode="json"))
    assert response.status_code == 200, response.text
    report = response.json()
    assert client.get(path).json() == report
    assert client.get(f"/api/v1/data/coverage/{report['id']}").json() == report
    assert (
        client.post(
            f"/api/v1/data/coverage/{report['id']}/refill", json={"command_id": "api-fill"}
        ).status_code
        == 200
    )
    tracked = client.get(f"/api/v1/data/coverage/{report['id']}/refill-status")
    assert tracked.status_code == 200 and len(tracked.json()["jobs"]) == 2
    assert client.get("/api/v1/data/coverage/missing/refill-status").status_code == 404
    assert client.post(path, json={"start": "2024-01-02", "end": "2024-01-01"}).status_code == 422
    protected = TestClient(
        create_app(settings.model_copy(update={"require_account": True}), raw_engine(sync.engine))
    )
    protected.headers["Authorization"] = f"Bearer {MASTER}"
    assert protected.get(path).status_code == 401
    assert protected.get(f"/api/v1/data/coverage/{report['id']}/refill-status").status_code == 401
    assert protected.post(path, json=request().model_dump(mode="json")).status_code == 401
    assert protected.get(f"/api/v1/data/coverage/{report['id']}").status_code == 401
    assert (
        protected.post(
            f"/api/v1/data/coverage/{report['id']}/refill", json={"command_id": "denied"}
        ).status_code
        == 401
    )


def test_only_confirmed_closed_dates_bridge_refill_ranges(sync):
    contracts(sync, delisted="20240110")
    calendar(sync, {"2024-01-02": 1, "2024-01-03": 0, "2024-01-04": 1, "2024-01-05": 1})
    daily = publish(sync, ["2024-01-05"])
    report = sync.coverage.check(daily["id"], request(start="2024-01-02", end="2024-01-08"))
    assert report["counts"]["UNKNOWN_CALENDAR"] == 3
    assert report["refill_ranges"] == [{"start": "2024-01-02", "end": "2024-01-04"}]
    submitted = sync.coverage.refill(report["id"], "confirmed-only")
    assert len(submitted["jobs"]) == 1
    with sync.engine.connect() as conn:
        payload = conn.execute(
            select(jobs.c.payload).where(jobs.c.id == submitted["jobs"][0]["id"])
        ).scalar_one()
    assert payload["request"]["end"] == "2024-01-04"


def test_refill_does_not_widen_an_earlier_report_when_today_advances(sync, monkeypatch):
    daily, _, _ = setup_range(sync)
    monkeypatch.setattr("asterion.data.coverage.today", lambda: date(2024, 1, 5))
    report = sync.coverage.check(daily["id"], request(start="2024-01-03", end="2024-01-05"))
    assert report["days"][-1]["status"] == "PENDING"
    monkeypatch.setattr("asterion.data.coverage.today", lambda: date(2024, 1, 6))
    result = sync.coverage.refill(report["id"], "fixed-approved-dates")
    assert len(result["jobs"]) == 1
    with sync.engine.connect() as conn:
        payload = conn.execute(
            select(jobs.c.payload).where(jobs.c.id == result["jobs"][0]["id"])
        ).scalar_one()
    assert payload["request"]["start"] == payload["request"]["end"] == "2024-01-03"


def test_other_exchange_reference_is_rejected(sync):
    daily, _, _ = setup_range(sync)
    req = SyncRequest(
        command_id=str(uuid4()),
        provider="tushare",
        dataset="calendar",
        exchange="DCE",
        start=date(2024, 1, 2),
        end=date(2024, 1, 2),
    )
    sync.submit(req)
    job = scheduler(sync.engine).claim("other-exchange")
    part = Tushare().plan(req)[0]
    content = canonical(
        [
            {
                "partition": part.model_dump(),
                "rows": [
                    {
                        "exchange": "DCE",
                        "cal_date": "20240102",
                        "is_open": 1,
                        "pretrade_date": "20231229",
                    }
                ],
                "observed_at": datetime.now(UTC).isoformat(),
            }
        ]
    )
    other = sync.publish(job["id"], job["token"], content)
    with pytest.raises(ProviderError, match="来源及交易所"):
        sync.coverage.check(daily["id"], request(calendar_version_id=other["id"]))


@pytest.mark.skipif(
    not os.getenv("ASTERION_TEST_DATABASE_URL"), reason="PostgreSQL test URL not set"
)
def test_concurrent_refill_commands_create_one_atomic_batch(tmp_path):
    url = os.environ["ASTERION_TEST_DATABASE_URL"]
    admin = create_engine(url)
    schema = "coverage_" + uuid4().hex
    with admin.begin() as conn:
        conn.execute(text(f"CREATE SCHEMA {schema}"))
    engine = create_engine(url, connect_args={"options": f"-csearch_path={schema}"})
    try:
        metadata.create_all(engine)
        service = DataSync(
            data_store(engine),
            domain_tasks(data_store(engine), "data"),
            tmp_path,
            provider_secrets(MASTER),
        )
        set_token(service, "tushare", "synthetic-token")
        daily, _, _ = setup_range(service)
        report = service.coverage.check(daily["id"], request())
        with ThreadPoolExecutor(max_workers=6) as pool:
            results = list(
                pool.map(lambda _: service.coverage.refill(report["id"], "same-command"), range(6))
            )
        assert all(result == results[0] for result in results)
        assert len(results[0]["jobs"]) == 2
        tracking = service.coverage.tracking(report["id"])
        assert tracking.total == 2 and len(tracking.jobs) == 2
        first = scheduler(service.engine).claim("postgres-refill")
        scheduler(service.engine).fail(first["id"], first["token"], "fixture failure")
        retried = service.retry(first["id"], "postgres-retry", resume=True)
        assert retried["id"] in {job.id for job in service.coverage.tracking(report["id"]).jobs}
        assert first["id"] not in {job.id for job in service.coverage.tracking(report["id"]).jobs}
        with engine.connect() as conn:
            assert len(conn.execute(select(jobs).where(jobs.c.state == "QUEUED")).all()) == 2
    finally:
        engine.dispose()
        with admin.begin() as conn:
            conn.execute(text(f"DROP SCHEMA {schema} CASCADE"))
        admin.dispose()


def test_refill_tracking_restores_current_attempts_without_submitting(sync, monkeypatch):
    daily, _, _ = setup_range(sync)
    monkeypatch.setattr("asterion.data.coverage.today", lambda: date(2024, 1, 8))
    report = sync.coverage.check(daily["id"], request(start="2024-01-03", end="2024-01-03"))
    monkeypatch.setattr("asterion.data.coverage.today", lambda: date(2024, 1, 9))
    assert not sync.coverage.tracking(report["id"]).submitted
    batch = sync.coverage.refill(report["id"], "persisted-fill")
    assert batch["checked_report_id"] != report["id"]
    assert sync.coverage.tracking(batch["checked_report_id"]).total == 1
    claimed = scheduler(sync.engine).claim("failed-before-restart")
    scheduler(sync.engine).fail(claimed["id"], claimed["token"], "provider temporarily unavailable")
    # Recreate the service from persisted database/files; no browser cache or submission needed.
    restored = DataSync(
        data_store(sync.engine),
        domain_tasks(data_store(sync.engine), "data"),
        sync.root,
        provider_secrets(MASTER),
    )
    before_read = len(scheduler(restored.engine).list())
    current = restored.coverage.tracking(report["id"])
    assert len(scheduler(restored.engine).list()) == before_read
    assert current.jobs[0].id == batch["jobs"][0]["id"]
    assert current.jobs[0].state == "FAILED"
    assert current.jobs[0].error == "provider temporarily unavailable"
    retry = restored.retry(claimed["id"], "full-retry", resume=False)
    assert restored.retry(claimed["id"], "full-retry", resume=False)["id"] == retry["id"]
    assert [job.id for job in restored.coverage.tracking(report["id"]).jobs] == [retry["id"]]
    scheduler(restored.engine).cancel(retry["id"])
    resumed = restored.retry(retry["id"], "resumed-retry", resume=True)
    tracking = restored.coverage.tracking(report["id"])
    assert tracking.total == 3
    assert [job.id for job in tracking.jobs] == [resumed["id"]]
    encoded = tracking.model_dump_json()
    assert '"payload"' not in encoded and '"token"' not in encoded
    assert "synthetic-only-token" not in encoded
    unrelated = restored.coverage.check(daily["id"], request(start="2024-01-05", end="2024-01-05"))
    assert not restored.coverage.tracking(unrelated["id"]).submitted


def test_refill_tracking_empty_outcome_and_bounded_history(sync):
    daily, _, _ = setup_range(sync)
    covered = sync.coverage.check(daily["id"], request(start="2024-01-02", end="2024-01-02"))
    sync.coverage.refill(covered["id"], "already-covered")
    report = sync.coverage.tracking(covered["id"])
    assert report.submitted and report.statuses == ["NO_GAPS"] and report.jobs == []
    with sync.engine.begin() as conn:
        sync.tasks.submit_batch(
            conn,
            [
                (f"history-{n}", "data.sync", {"coverage_report_id": covered["id"]})
                for n in range(201)
            ],
        )
    report = sync.coverage.tracking(covered["id"])
    assert report.truncated and report.total == 201 and len(report.jobs) == 200
    with pytest.raises(KeyError):
        sync.coverage.tracking("missing-report")


def test_refill_missing_daily_identity_creates_no_jobs_and_preserves_record(sync):
    from copy import deepcopy

    from asterion.data.library import versions

    daily, _, _ = setup_range(sync)
    report = sync.coverage.check(daily["id"], request())
    manifest = deepcopy(daily["manifest"])
    manifest.pop("contract_identity")
    with raw_engine(sync.engine).begin() as conn:
        conn.execute(
            versions.update().where(versions.c.id == daily["id"]).values(manifest=manifest)
        )
    before = len(scheduler(sync.engine).list())
    with pytest.raises(ValueError):
        sync.coverage.refill(report["id"], "missing-identity")
    assert len(scheduler(sync.engine).list()) == before
    with sync.engine.connect() as conn:
        assert (
            conn.execute(
                select(versions.c.manifest).where(versions.c.id == daily["id"])
            ).scalar_one()
            == manifest
        )
