from import_support import chart_bars

"""Publication work is bounded by changed months; full reads still verify fixed bytes."""

from dataclasses import replace

import pytest
from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.files import read_files
from test_cumulative import publish
from test_cumulative import sync as sync  # noqa: PLC0414

from asterion.data import partitions
from asterion.data.backup import load_evidence, validate_backup


def test_only_touched_month_is_read_and_chart_is_fixed_lazy_projection(sync, monkeypatch):
    first = publish(sync, ["2024-01-02", "2024-02-02", "2024-03-02"])
    reads = []
    original = partitions.merge

    def tracked(store, request):
        outcome = original(store, request)
        reads.extend(outcome["metrics"]["partitions_read"])
        return outcome

    monkeypatch.setattr(partitions, "merge", tracked)
    changed = publish(sync, ["2024-02-02"], price=3210)
    assert reads == ["2024-02"]
    assert changed["rows"] == 3
    assert list((sync.root / "published").glob("*.parquet")) == []
    assert [
        r["close"] for r in chart_bars(sync.engine, sync.root, first["manifest"]["snapshot_id"])
    ] == ["3200.00000000"] * 3
    assert [
        r["close"] for r in chart_bars(sync.engine, sync.root, changed["manifest"]["snapshot_id"])
    ] == [
        "3200.00000000",
        "3210.00000000",
        "3200.00000000",
    ]
    reads.clear()
    added = publish(sync, ["2024-04-02"])
    assert reads == []
    assert added["rows"] == 4


def test_calendar_summary_preserves_gaps_across_months_and_extensions(sync):
    publish(sync, ["2024-02-28", "2024-02-29"], dataset="calendar")
    publish(sync, ["2024-01-31"], dataset="calendar")
    expanded = publish(sync, ["2024-03-02"], dataset="calendar")
    assert expanded["manifest"]["coverage_gaps"] == [
        {"start": "2024-02-01", "end": "2024-02-27"},
        {"start": "2024-03-01", "end": "2024-03-01"},
    ]
    filled = publish(sync, ["2024-03-01"], dataset="calendar")
    assert filled["manifest"]["coverage_gaps"] == [{"start": "2024-02-01", "end": "2024-02-27"}]
    assert filled["rows"] == 5


def test_reused_corruption_is_rejected_when_consumed_and_by_backup(sync):
    first = publish(sync, ["2024-01-02"])
    part = first["manifest"]["partitions"][0]
    (sync.root / "artifacts" / f"{part['checksum']}.parquet").write_bytes(b"corrupt")
    second = publish(sync, ["2024-02-02"])
    # Incremental publication does not claim it has re-scanned immutable old bytes.
    with pytest.raises(ValueError, match="校验和"):
        chart_bars(sync.engine, sync.root, second["manifest"]["snapshot_id"])
    with sync.engine.connect() as conn:
        evidence = load_evidence(
            conn, ArtifactStore(sync.root, read_only=True), read_files(sync.root), lambda _: True
        )
    with pytest.raises(ValueError, match="校验失败"):
        validate_backup(evidence)


def test_backup_rejects_changed_projection_reference(sync):
    publish(sync, ["2024-01-02"])
    with sync.engine.connect() as conn:
        evidence = load_evidence(
            conn, ArtifactStore(sync.root, read_only=True), read_files(sync.root), lambda _: True
        )
    validate_backup(evidence)
    value = evidence.snapshots[0]
    damaged = value | {"manifest": value["manifest"] | {"version_id": "missing"}}
    with pytest.raises(ValueError, match="图表投影"):
        validate_backup(replace(evidence, snapshots=(damaged,)))


def test_projection_returns_the_fixed_version_bars(sync):
    first = publish(sync, ["2024-01-02"])
    bars = chart_bars(sync.engine, sync.root, first["manifest"]["snapshot_id"])
    assert bars[0]["trading_day"] == "2024-01-02"
    assert bars[0]["close"] == "3200.00000000"
