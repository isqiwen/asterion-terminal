"""Explicit offline contract observations for source pipeline tests, never shipped."""

from datetime import UTC, datetime
from uuid import uuid4

from storage_support import scheduler

from asterion.data.providers.public import SyncRequest
from asterion.data.providers.tushare import Tushare
from asterion.data.sync_admission import SyncSubmission, admit
from asterion.platform.serialization import canonical


def submit_source(sync, request):
    if request.dataset not in {"daily", "settlement"}:
        return sync.submit(request)
    references = sync.library.list(
        type_id="futures.contracts",
        source=request.connection_id or request.provider,
        layer="STANDARD",
    )["items"]
    references = [
        v
        for v in references
        if v["manifest"]["scope"].get("connection_id") == request.connection_id
    ]
    if not references:
        req = SyncRequest(
            command_id="fixture-contracts-" + str(uuid4()),
            provider="tushare",
            connection_id=request.connection_id,
            dataset="contracts",
            exchange="SHFE",
        )
        accepted = sync.submit(req)
        task = scheduler(sync.engine).claim("fixture-contracts")
        assert task["id"] == accepted["id"], "Seed reference before queuing other work"
        part = Tushare().plan(req)[0]
        rows = []
        for symbol, product, month in [
            ("RB2610.SHF", "RB", "202610"),
            ("CU2610.SHF", "CU", "202610"),
            ("RB2405.SHF", "RB", "202405"),
        ]:
            values = {
                "ts_code": symbol,
                "exchange": "SHFE",
                "name": "fixture",
                "fut_code": product,
                "d_month": month,
                "list_date": "20230101",
                "delist_date": "20261015",
                "per_unit": 10,
                "trade_unit": "吨",
                "quote_unit": "元/吨",
            }
            rows.append({key: values.get(key) for key in part.fields})
        content = canonical(
            [
                {
                    "partition": part.model_dump(),
                    "rows": rows,
                    "observed_at": datetime.now(UTC).isoformat(),
                }
            ]
        )
        references = [sync.publish(task["id"], task["token"], content)]
    return admit(
        sync,
        SyncSubmission.model_validate(
            request.model_dump() | {"contracts_version_id": references[0]["id"]}
        ),
    )
