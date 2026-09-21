"""Whole-batch admission and outer-transaction rollback through the data public boundary."""

from copy import deepcopy

import pytest
from sqlalchemy import select
from test_data_sync import context as data_context
from test_data_sync import prepared, request

from asterion.data.public import DailySyncBatch
from asterion.data.sync_batch import submit_batch
from asterion.platform.store import jobs

context = data_context


def basis(context, monkeypatch):
    def two(rows):
        second = deepcopy(rows[0])
        second.update(
            ts_code="RB2611.SHF",
            symbol="RB2611",
            d_month="202611",
            delist_date="20261115",
            last_ddate="20261120",
        )
        return rows + [second]

    _, _, sync, _ = context
    job, content = prepared(context, monkeypatch, request("contracts"), transform=two)
    version = sync.publish(job["id"], job["token"], content)
    return DailySyncBatch(
        command_prefix="batch-case",
        provider="tushare",
        connection_id=None,
        exchange="SHFE",
        contracts_version_id=version["id"],
        trading_day="2024-01-02",
        symbols=("RB2610.SHF", "RB2611.SHF"),
    )


def test_batch_uses_normal_identity_admission_and_freezes_all_inputs(context, monkeypatch):
    _, _, sync, _ = context
    body = basis(context, monkeypatch)
    with sync.engine.begin() as conn:
        queued = submit_batch(sync, conn, body)
        assert len(queued) == 2
        rows = conn.execute(select(jobs).where(jobs.c.kind == "data.sync")).mappings().all()
    payloads = [row["payload"] for row in rows if row["id"] in {job.id for job in queued}]
    assert {p["request"]["symbol"] for p in payloads} == set(body.symbols)
    assert all(
        p["contract_identity"]["catalog"]["inputs"][0]["version_id"] == body.contracts_version_id
        for p in payloads
    )
    assert len({p["configuration"]["ref"] for p in payloads}) == 1


@pytest.mark.parametrize("failure", ["candidate", "after_insert"])
def test_failed_batch_leaves_no_partial_jobs(context, monkeypatch, failure):
    _, _, sync, _ = context
    body = basis(context, monkeypatch)
    if failure == "candidate":
        body = body.model_copy(update={"symbols": ("RB2610.SHF", "RB2612.SHF")})
    with pytest.raises((ValueError, RuntimeError)), sync.engine.begin() as conn:
        submit_batch(sync, conn, body)
        if failure == "after_insert":
            raise RuntimeError("outer workflow failed")
    with sync.engine.connect() as conn:
        assert len(conn.execute(select(jobs)).all()) == 1  # Only the published reference job.
