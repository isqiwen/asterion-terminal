"""Research selects a bounded input through the data plugin's fixed batch contract."""

from contextlib import closing

from asterion.data.public import ScanRequest

COLUMNS = ("contract", "trading_day", "open", "high", "low", "close", "settle")


def selected_input(access, request, limit):
    result = access.scan(
        ScanRequest(
            version_id=request.version_id,
            contract_ids=(request.rules.spec.contract.id,),
            start=request.start,
            end=request.end,
            columns=COLUMNS,
            batch_rows=512,
        )
    )
    rows, sources = [], []
    with closing(result.batches):
        for batch in result.batches:
            if len(rows) + batch.num_rows > limit:
                raise ValueError(f"所选研究区间超过 {limit} 行，请缩小范围；不会截断执行")
            for row in batch.to_pylist():
                rows.append({key: row[key] for key in COLUMNS})
                sources.append(
                    {"observed_at": row["_observed_at"], "raw_version_id": row["_raw_version_id"]}
                    if row["_raw_version_id"] is not None
                    else {"version_created_at": result.version["created_at"]}
                )
    return {"version": result.version, "rows": rows, "row_sources": sources}
