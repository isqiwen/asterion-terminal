"""Attributed synthetic reference inputs for isolated tests and desktop smoke only."""

from datetime import UTC, date, datetime
from types import SimpleNamespace
from uuid import uuid4

import pyarrow as pa
import pyarrow.parquet as pq

from asterion.data.coverage import CoverageRequest, DailyCoverage
from asterion.data.library import DataLibrary
from asterion.platform.serialization import canonical


def reference_inputs(storage, root):
    library = DataLibrary(storage, root)
    contracts = [
        {
            "exchange": "SHFE",
            "symbol": "rb2405.fixture",
            "contract": "SHFE.rb2405",
            "product": "RB",
            "name": "Synthetic contract",
            "currency": "CNY",
            "delivery_month": "2024-05",
            "listed": "2023-05-16",
            "delisted": "2024-05-15",
            "last_delivery_on": None,
            "suggested_multiplier": None,
            "multiplier_note": "Synthetic identity input; no trading specification",
            "multiplier": None,
            "quote_unit_desc": None,
        }
    ]
    calendar = [
        {
            "exchange": "SHFE",
            "date": date(2024, 1, d).isoformat(),
            "is_open": 1,
            "previous_trading_day": None,
        }
        for d in range(1, 32)
    ]
    ids = {}
    for kind, rows in (("contracts", contracts), ("calendar", calendar)):
        ident = str(uuid4())
        folder = "reference-fixtures/" + ident
        library.types.get("futures." + kind).validate(rows)
        output = pa.BufferOutputStream()
        pq.write_table(pa.Table.from_pylist(rows), output)
        standard = library.artifacts.put(
            folder + "/standard.parquet", output.getvalue().to_pybytes()
        )
        raw = library.artifacts.put(folder + "/raw.json", canonical(rows))
        observed = datetime.now(UTC).isoformat()
        with storage.begin() as conn:
            _, ids[kind] = library.publish_pair(
                conn,
                job_id=ident,
                type_id="futures." + kind,
                source="test-reference",
                scope={"exchange": "SHFE", "symbol": ""},
                raw=raw,
                raw_format="fixture",
                standard=standard,
                row_count=len(rows),
                detail={"observed_at": observed, "available_at": observed, "demo": False},
            )
    return {
        "calendar_version_id": ids["calendar"],
        "contracts_version_id": ids["contracts"],
        "reference_policy": "explicit_external",
        "reference_symbol": "rb2405.fixture",
        "reference_note": "Synthetic identity and all-open January calendar for isolated tests",
    }


def research_coverage(storage, root, version_id, start, end):
    references = reference_inputs(storage, root)
    library = DataLibrary(storage, root)
    return DailyCoverage(SimpleNamespace(engine=storage, library=library)).check(
        version_id, CoverageRequest.model_validate({"start": start, "end": end, **references})
    )
