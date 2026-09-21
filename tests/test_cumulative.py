from configuration_support import set_token
from credential_helpers import provider_secrets
from storage_support import data_store, domain_tasks, scheduler
from sync_identity_support import submit_source

"""Cumulative versions preserve old inputs and resolve observations independently of commit order."""

import json
import os
from concurrent.futures import ThreadPoolExecutor
from datetime import UTC, datetime
from threading import Barrier
from uuid import uuid4

import pytest
from sqlalchemy import create_engine

from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.data.public import read_bars
from asterion.data.sync import DataSync
from asterion.platform.serialization import canonical
from asterion.platform.store import metadata


@pytest.fixture
def sync(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/catalog.db")
    metadata.create_all(engine)
    result = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path,
        provider_secrets("synthetic-master-key"),
    )
    set_token(result, "tushare", "synthetic-provider-key")
    return result


def pending(sync, days, *, dataset="daily", price=3200, symbol="RB2610.SHF", connection_id=None):
    request = SyncRequest.model_validate(
        {
            "command_id": str(uuid4()),
            "provider": "tushare",
            "connection_id": connection_id,
            "dataset": dataset,
            "exchange": "SHFE",
            "symbol": symbol if dataset == "daily" else "",
            "start": min(days),
            "end": max(days),
        }
    )
    submit_source(sync, request)
    job = scheduler(sync.engine).claim("test-cumulative")
    plan = Tushare().plan(request)
    observed = datetime.now(UTC).isoformat()
    evidence = []
    for part in plan:
        records = []
        for day in days:
            compact = day.replace("-", "")
            if not part.params["start_date"] <= compact <= part.params["end_date"]:
                continue
            values = {
                "ts_code": symbol,
                "trade_date": compact,
                "open": price,
                "high": price + 10,
                "low": price - 10,
                "close": price,
                "settle": price,
                "vol": 100,
                "exchange": "SHFE",
                "cal_date": compact,
                "is_open": 1,
                "pretrade_date": "20231229",
            }
            records.append({field: values.get(field) for field in part.fields})
        evidence.append({"partition": part.model_dump(), "rows": records, "observed_at": observed})
    return job, canonical(evidence)


def publish(sync, days, **kwargs):
    job, content = pending(sync, days, **kwargs)
    return sync.publish(job["id"], job["token"], content)


def test_disjoint_months_reuse_files_and_versions_remain_fixed(sync):
    first = publish(sync, ["2024-01-02", "2024-01-03"])
    original = sync.library.preview(first["id"])
    part = first["manifest"]["partitions"][0]
    path = sync.root / "artifacts" / f"{part['checksum']}.parquet"
    timestamp = path.stat().st_mtime_ns
    second = publish(sync, ["2024-02-02"])
    assert first["dataset_id"] == second["dataset_id"]
    assert second["rows"] == 3
    assert second["manifest"]["parent_version_id"] == first["id"]
    assert second["manifest"]["revision"] == 2
    assert second["manifest"]["partitions"][0] == part
    assert path.stat().st_mtime_ns == timestamp
    assert sync.library.preview(first["id"]) == original
    page = sync.library.preview(second["id"], offset=1, limit=2)
    assert [row["trading_day"] for row in page["rows"]] == ["2024-01-03", "2024-02-02"]
    assert page["row_sources"][0] == original["row_sources"][1]
    assert page["row_sources"][1]["raw_version_id"] == second["manifest"]["inputs"][0]
    assert sync.library.preview(second["manifest"]["inputs"][0])["total"] == 1
    assert sync.library.list(layer="STANDARD")["items"][0]["id"] == second["id"]
    assert sync.library.preview(second["id"], 100)["rows"] == []
    bars = read_bars(sync.root / "published" / f"{second['manifest']['snapshot_id']}.parquet")
    assert len(bars) == 3
    assert datetime.fromisoformat(bars[0]["available_at"]) == datetime.fromisoformat(
        original["row_sources"][0]["observed_at"]
    )


def test_revision_preserves_absent_rows_and_untouched_month(sync):
    first = publish(sync, ["2024-01-02", "2024-01-03", "2024-02-02"])
    changed = publish(sync, ["2024-01-02"], price=3210)
    assert changed["rows"] == 3
    assert changed["manifest"]["changes"]["revised"] == 1
    assert (
        changed["manifest"]["partitions"][0]["replaces"]
        == first["manifest"]["partitions"][0]["checksum"]
    )
    assert changed["manifest"]["partitions"][1] == first["manifest"]["partitions"][1]
    assert [row["close"] for row in sync.library.preview(changed["id"])["rows"]] == [
        "3210",
        "3200",
        "3200",
    ]
    assert sync.library.preview(first["id"])["rows"][0]["close"] == "3200"
    # An entirely empty recollection cannot delete the prior data or publish an empty latest.
    job, content = pending(sync, ["2024-01-02"])
    envelope = json.loads(content)
    envelope[0]["rows"] = []
    with pytest.raises(ProviderError, match="EMPTY_UNCONFIRMED"):
        sync.publish(job["id"], job["token"], canonical(envelope))
    assert sync.library.list(layer="STANDARD")["items"][0]["id"] == changed["id"]


def test_delayed_old_observation_cannot_overwrite_newer_revision(sync):
    old_job, old_content = pending(sync, ["2024-01-02", "2024-01-03"], price=3200)
    fresh = publish(sync, ["2024-01-02"], price=3220)
    merged = sync.publish(old_job["id"], old_job["token"], old_content)
    assert merged["manifest"]["changes"]["stale_ignored"] == 1
    assert merged["manifest"]["changes"]["added"] == 1
    assert [row["close"] for row in sync.library.preview(merged["id"])["rows"]] == ["3220", "3200"]
    assert merged["manifest"]["observed_at"] == fresh["manifest"]["observed_at"]
    newer = publish(sync, ["2024-03-01"])
    assert sync.publish(old_job["id"], old_job["token"], old_content) == merged
    assert sync.library.list(layer="STANDARD")["items"][0]["id"] == newer["id"]


def test_equal_observation_time_conflict_is_rejected(sync):
    first_job, first_content = pending(sync, ["2024-01-02"])
    second_job, second_content = pending(sync, ["2024-01-02"], price=3210)
    timestamp = datetime.now(UTC).isoformat()
    envelopes = [json.loads(content) for content in (first_content, second_content)]
    for envelope in envelopes:
        envelope[0]["observed_at"] = timestamp
    first = sync.publish(first_job["id"], first_job["token"], canonical(envelopes[0]))
    with pytest.raises(ProviderError, match="拒绝自动裁决"):
        sync.publish(second_job["id"], second_job["token"], canonical(envelopes[1]))
    assert sync.library.list(layer="STANDARD")["items"][0]["id"] == first["id"]


def test_calendar_gap_is_reported_then_filled(sync):
    publish(sync, ["2024-01-02"], dataset="calendar")
    gapped = publish(sync, ["2024-01-04"], dataset="calendar")
    assert gapped["manifest"]["coverage"] == "CALENDAR_GAPS"
    assert gapped["manifest"]["coverage_gaps"] == [{"start": "2024-01-03", "end": "2024-01-03"}]
    filled = publish(sync, ["2024-01-03"], dataset="calendar")
    assert filled["rows"] == 3
    assert filled["manifest"]["coverage"] == "CALENDAR_COMPLETE"
    assert filled["manifest"]["coverage_gaps"] == []
    assert sync.library.preview(gapped["id"])["total"] == 2


def test_daily_coverage_does_not_infer_calendar_gaps_and_contracts_stay_separate(sync):
    first = publish(sync, ["2024-01-02", "2024-01-04"])
    assert first["manifest"]["coverage"] == "RETURNED_ROWS_ONLY"
    assert first["manifest"]["coverage_gaps"] is None
    different = publish(sync, ["2024-01-02"], symbol="CU2610.SHF")
    assert first["dataset_id"] != different["dataset_id"]
    assert different["rows"] == 1
    assert different["manifest"]["parent_version_id"] is None


def test_failed_artifact_write_does_not_advance_parent(sync, monkeypatch):
    first = publish(sync, ["2024-01-02"])
    job, content = pending(sync, ["2024-02-02"])
    with monkeypatch.context() as patch:

        def fail(*args):
            raise OSError("synthetic disk full")

        patch.setattr("asterion.data.partitions.atomic_write", fail)
        with pytest.raises(OSError):
            sync.publish(job["id"], job["token"], content)
    assert sync.library.list(layer="STANDARD")["items"][0]["id"] == first["id"]
    second = sync.publish(job["id"], job["token"], content)
    assert second["rows"] == 2
    assert second["manifest"]["revision"] == 2


def test_corrupt_parent_blocks_preview_and_further_publication(sync):
    first = publish(sync, ["2024-01-02"])
    checksum = first["manifest"]["partitions"][0]["checksum"]
    (sync.root / "artifacts" / f"{checksum}.parquet").write_bytes(b"corrupt")
    with pytest.raises(ProviderError, match="校验和"):
        sync.library.preview(first["id"])
    job, content = pending(sync, ["2024-02-02"])
    with pytest.raises(ProviderError, match="校验和"):
        sync.publish(job["id"], job["token"], content)
    assert sync.library.history(first["dataset_id"])["total"] == 1


def test_acquisition_series_is_not_silently_adopted(sync, monkeypatch):
    with monkeypatch.context() as patch:
        patch.setattr("asterion.data.sync.SUPPORTED", set())
        acquisition = publish(sync, ["2024-01-02"])
    cumulative = publish(sync, ["2024-02-02"])
    assert acquisition["dataset_id"] != cumulative["dataset_id"]
    assert cumulative["rows"] == 1
    assert cumulative["manifest"]["parent_version_id"] is None
    assert sync.library.preview(acquisition["id"])["rows"][0]["trading_day"] == "2024-01-02"


@pytest.mark.skipif(
    not os.getenv("ASTERION_TEST_DATABASE_URL"), reason="PostgreSQL test URL not set"
)
def test_concurrent_cumulative_publication_serializes_parent_selection(tmp_path):
    engine = create_engine(os.environ["ASTERION_TEST_DATABASE_URL"])
    metadata.create_all(engine)
    sync = DataSync(
        data_store(engine),
        domain_tasks(data_store(engine), "data"),
        tmp_path,
        provider_secrets("synthetic-concurrency-master"),
    )
    set_token(sync, "tushare", "synthetic-provider-key")
    from asterion.data.connections import NewConnection

    connection = sync.connections.create(NewConnection(provider="tushare", name="concurrency"))
    set_token(sync, connection["id"], "synthetic-provider-key")
    symbol = "RB2610.SHF"
    inputs = [
        pending(sync, [day], symbol=symbol, connection_id=connection["id"])
        for day in ["2024-01-02", "2024-02-02", "2024-03-02"]
    ]
    barrier = Barrier(3)

    def publish_one(item):
        job, content = item
        barrier.wait(timeout=10)
        return sync.publish(job["id"], job["token"], content)

    try:
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(publish_one, inputs))
        ordered = sorted(results, key=lambda record: record["manifest"]["revision"])
        assert [record["rows"] for record in ordered] == [1, 2, 3]
        assert ordered[0]["manifest"]["parent_version_id"] is None
        assert [record["manifest"]["parent_version_id"] for record in ordered[1:]] == [
            record["id"] for record in ordered[:-1]
        ]
        assert sync.library.history(ordered[0]["dataset_id"])["items"][0]["id"] == ordered[-1]["id"]
        assert sync.library.preview(ordered[-1]["id"])["total"] == 3
        old = pending(
            sync, ["2024-01-02"], price=3210, symbol=symbol, connection_id=connection["id"]
        )
        fresh = pending(
            sync, ["2024-01-02"], price=3220, symbol=symbol, connection_id=connection["id"]
        )
        barrier = Barrier(2)
        with ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(publish_one, [old, fresh]))
        latest = sync.library.history(ordered[0]["dataset_id"])["items"][0]
        assert latest["manifest"]["revision"] == 5
        assert sync.library.preview(latest["id"])["rows"][0]["close"] == "3220"
        assert sync.library.preview(ordered[-1]["id"])["rows"][0]["close"] == "3200"
    finally:
        engine.dispose()
