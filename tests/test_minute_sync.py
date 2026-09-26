import copy

"""Fixed minute source windows, overnight identity and immutable daily partitions."""

import json
from datetime import UTC, date, datetime
from uuid import uuid4

import pytest
from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.calendar import TimeSpec, TimeVersion, time_id
from storage_support import scheduler
from test_cumulative import sync as sync  # noqa: PLC0414
from test_trading_time import example

from asterion.data.minute import MinuteContext, coverage
from asterion.data.providers.public import SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.platform.serialization import canonical


@pytest.fixture
def prepared(sync):
    request = SyncRequest(
        command_id=str(uuid4()), provider="tushare", dataset="contracts", exchange="SHFE"
    )
    sync.submit(request)
    job = scheduler(sync.engine).claim("reference")
    part = Tushare().plan(request)[0]
    values = {
        "ts_code": "AU2506.SHF",
        "exchange": "SHFE",
        "name": "合成测试合约",
        "fut_code": "AU",
        "d_month": "202506",
        "list_date": "20240617",
        "delist_date": "20250616",
        "per_unit": 1000,
        "trade_unit": "克",
        "quote_unit": "元/克",
    }
    reference = sync.publish(
        job["id"],
        job["token"],
        canonical(
            [
                {
                    "partition": part.model_dump(),
                    "observed_at": datetime.now(UTC).isoformat(),
                    "rows": [{f: values.get(f) for f in part.fields}],
                }
            ]
        ),
    )
    spec = TimeSpec.model_validate(example())
    context = MinuteContext(
        frequency="1m",
        trading_time=TimeVersion(id=time_id(spec), spec=spec),
        timestamp_semantics="bar_end",
        semantics_source="Explicit synthetic fixture: minute end timestamps, not real-source certification",
    )
    body = {
        "command_id": str(uuid4()),
        "provider": "tushare",
        "dataset": "minute",
        "exchange": "SHFE",
        "symbol": "AU2506.SHF",
        "start": "2025-04-14",
        "end": "2025-04-14",
        "contracts_version_id": reference["id"],
        "minute_context": context.model_dump(mode="json"),
    }
    return sync, body


DAY = date(2025, 4, 14)


def pending(sync, body, *, missing=False):
    sync.admit(body)
    job = scheduler(sync.engine).claim("minute-test")
    request = SyncRequest.model_validate(job["payload"]["request"])
    part = Tushare().plan(request)[0]
    day = date.fromisoformat(str(body["start"]))
    stamps = MinuteContext.model_validate(body["minute_context"]).label_stamps("SHFE.AU2506", day)
    rows = [
        {
            "ts_code": body["symbol"],
            "trade_time": s.strftime("%Y-%m-%d %H:%M:%S"),
            "open": 100,
            "high": 101,
            "low": 99,
            "close": 100,
            "vol": 10,
            "amount": 10000,
            "oi": 200,
        }
        for s in stamps[int(missing) :]
    ]
    envelope = [
        {"partition": part.model_dump(), "observed_at": datetime.now(UTC).isoformat(), "rows": rows}
    ]
    return job, envelope


def test_overnight_download_gap_repair_and_fixed_old_version(prepared):
    sync, body = prepared
    job, envelope = pending(sync, body, missing=True)
    part = envelope[0]["partition"]
    assert part["api"] == "ft_mins" and part["params"]["freq"] == "1min"
    assert part["params"]["start_date"] == "2025-04-11 21:01:00"
    assert part["params"]["end_date"] == "2025-04-14 15:00:00"
    first = sync.publish(job["id"], job["token"], canonical(envelope))
    assert first["manifest"]["partitions"][0]["key"] == "2025-04-14"
    report = coverage(sync, first["id"], DAY)
    assert report.status == "GAPS" and len(report.missing) == 1
    body = body | {"command_id": str(uuid4())}
    job, envelope = pending(sync, body)
    second = sync.publish(job["id"], job["token"], canonical(envelope))
    assert second["dataset_id"] == first["dataset_id"]
    assert coverage(sync, second["id"], DAY).status == "COVERED"
    assert coverage(sync, first["id"], DAY) == report
    assert (
        first["manifest"]["contract_identity"]["catalog"]["contracts"][0]["id"]
        == "SHFE.AU.202506.20240617"
    )


def test_missing_time_evidence_and_session_or_symbol_errors_are_rejected(prepared):
    sync, body = prepared
    with pytest.raises(ValueError, match="时段"):
        sync.admit(body | {"minute_context": None})
    job, envelope = pending(sync, body)
    original = json.loads(canonical(envelope))
    envelope[0]["rows"][0]["trade_time"] = "2025-04-14 10:20:00"
    with pytest.raises(ValueError, match="休市"):
        sync.publish(job["id"], job["token"], canonical(envelope))
    envelope = original
    envelope[0]["rows"][0]["ts_code"] = "AU2508.SHF"
    with pytest.raises(ValueError, match="不符合请求"):
        sync.publish(job["id"], job["token"], canonical(envelope))


def test_full_source_limit_cannot_publish_truncated_minutes(prepared):
    sync, body = prepared
    job, envelope = pending(sync, body)
    envelope[0]["rows"] = [envelope[0]["rows"][0]] * 8000
    with pytest.raises(ValueError, match="接口上限"):
        sync.publish(job["id"], job["token"], canonical(envelope))


def test_add_trading_day_reuses_immutable_existing_partition(prepared):
    sync, body = prepared
    job, envelope = pending(sync, body)
    first = sync.publish(job["id"], job["token"], canonical(envelope))
    original = first["manifest"]["partitions"][0]
    path = sync.root / "artifacts" / f"{original['checksum']}.parquet"
    content, modified = path.read_bytes(), path.stat().st_mtime_ns
    earlier = date(2025, 4, 11)
    body = body | {"command_id": str(uuid4()), "start": earlier, "end": earlier}
    job, envelope = pending(sync, body)
    second = sync.publish(job["id"], job["token"], canonical(envelope))
    assert second["dataset_id"] == first["dataset_id"]
    assert original in second["manifest"]["partitions"]
    assert len(second["manifest"]["partitions"]) == 2
    assert path.read_bytes() == content and path.stat().st_mtime_ns == modified
    assert coverage(sync, second["id"], earlier).status == "COVERED"
    assert coverage(sync, second["id"], date(2025, 4, 14)).status == "COVERED"
    assert coverage(sync, first["id"], earlier).present == 0


def test_explicit_start_semantics_and_invalid_price(prepared):
    sync, body = prepared
    context = body["minute_context"] | {"timestamp_semantics": "bar_start"}
    body = body | {"minute_context": context}
    job, envelope = pending(sync, body)
    assert envelope[0]["partition"]["params"]["start_date"] == "2025-04-11 21:00:00"
    assert envelope[0]["partition"]["params"]["end_date"] == "2025-04-14 14:59:00"
    envelope[0]["rows"][0]["high"] = 90
    with pytest.raises(ValueError):
        sync.publish(job["id"], job["token"], canonical(envelope))
    envelope[0]["rows"][0]["high"] = 101
    release = sync.publish(job["id"], job["token"], canonical(envelope))
    assert coverage(sync, release["id"], DAY).status == "COVERED"


@pytest.mark.parametrize("frequency", ["1m", "5m", "15m", "30m", "60m"])
def test_provider_period_preserved_and_separate_collections(prepared, frequency):
    from asterion_bindings.data_store import Partition

    from asterion.data.partitions import read_partition

    sync, body = prepared
    job, envelope = pending(sync, body)
    baseline = sync.publish(job["id"], job["token"], canonical(envelope))
    context = body["minute_context"] | {"frequency": frequency}
    body = body | {"command_id": str(uuid4()), "minute_context": context}
    job, envelope = pending(sync, body)
    assert envelope[0]["partition"]["params"]["freq"] == frequency.replace("m", "min")
    # Sparse vendor-shaped observations, not a locally aggregated series.
    envelope[0]["rows"] = [envelope[0]["rows"][74], envelope[0]["rows"][-1]]
    envelope[0]["rows"][0]["amount"] = "123456.78"
    envelope[0]["rows"][0]["vol"] = "37"
    release = sync.publish(job["id"], job["token"], canonical(envelope))
    manifest = release["manifest"]
    assert manifest["type"]["frequency"] == manifest["scope"]["frequency"] == frequency
    assert any(
        len(node["path"]) == 7 and node["path"][-3] == frequency
        for node in sync.library.hierarchy()
    )
    rows = read_partition(
        sync.library.artifacts, Partition.model_validate(manifest["partitions"][0])
    )
    first = next(r for r in rows if r["amount"] == "123456.78")
    assert first["vol"] == "37" and first["frequency"] == frequency
    report = coverage(sync, release["id"], DAY)
    assert report.frequency == frequency
    if frequency == "1m":
        assert release["dataset_id"] == baseline["dataset_id"]
        assert report.status == "COVERED"
    else:
        assert release["dataset_id"] != baseline["dataset_id"]
        assert release["rows"] == 2
        assert report.status == "UNVERIFIED"
        assert report.expected is None and report.missing is None
    assert coverage(sync, baseline["id"], DAY).frequency == "1m"


def test_period_required_and_no_conflicting_wire_frequency(prepared):
    sync, body = prepared
    value = copy.deepcopy(body)
    del value["minute_context"]["frequency"]
    with pytest.raises(ValueError):
        sync.admit(value)
    for frequency in ("2m", "1h"):
        value["minute_context"]["frequency"] = frequency
        with pytest.raises(ValueError):
            sync.admit(value)
    with pytest.raises(ValueError, match="重复指定"):
        sync.admit(body | {"frequency": "5m"})


def test_retry_preserves_provider_period_and_reuses_evidence(prepared):

    sync, body = prepared
    body = body | {"minute_context": body["minute_context"] | {"frequency": "30m"}}
    job, envelope = pending(sync, body)
    envelope[0]["rows"] = envelope[0]["rows"][29::30]
    sync.evidence.record(job["id"], job["token"], 0, envelope[0])
    tasks = scheduler(sync.engine)
    tasks.fail(job["id"], job["token"], "synthetic interruption")
    sync.retry(job["id"], str(uuid4()), resume=True)
    retry = tasks.claim("resume-minute")
    assert retry["payload"]["request"]["frequency"] == "30m"
    assert retry["payload"]["minute_context"] == job["payload"]["minute_context"]
    reused = sync.evidence.resume(retry["id"], retry["token"])
    assert list(reused) == [0]
    release = sync.publish(retry["id"], retry["token"], canonical([reused[0]]))
    assert release["manifest"]["scope"]["frequency"] == "30m"


def test_backup_rejects_changed_period_without_rewriting_files(prepared):
    from copy import deepcopy
    from dataclasses import replace

    from asterion_bindings.files import read_files
    from sqlalchemy import select

    from asterion.data.backup import DataBackup, validate_backup
    from asterion.data.library import versions

    sync, body = prepared
    body = body | {"minute_context": body["minute_context"] | {"frequency": "60m"}}
    job, envelope = pending(sync, body)
    envelope[0]["rows"] = envelope[0]["rows"][59::60]
    release = sync.publish(job["id"], job["token"], canonical(envelope))
    with sync.engine.connect() as conn:
        records = tuple(dict(r) for r in conn.execute(select(versions)).mappings())
    evidence = DataBackup(
        records,
        (),
        (),
        (),
        (),
        (),
        ArtifactStore(sync.root, read_only=True),
        read_files(sync.root),
        lambda b: b,
    )
    assert validate_backup(evidence)["versions"] == len(records)
    changed = deepcopy(records)
    target = next(r for r in changed if r["id"] == release["id"])
    target["manifest"]["scope"]["frequency"] = "5m"
    with pytest.raises(ValueError, match="周期"):
        validate_backup(replace(evidence, versions=changed))
    assert validate_backup(evidence)["versions"] == len(records)
