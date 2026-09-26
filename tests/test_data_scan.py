from asterion_bindings.catalog import ImportIdentity
from import_support import import_options

"""Fixed scans prune history, close resources and never truncate research silently."""

import hashlib
from datetime import date, timedelta
from types import SimpleNamespace

import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from asterion_bindings.catalog import ReferenceCatalog, catalog_digest
from asterion_bindings.database import create_engine
from import_identity_support import import_identity
from storage_support import data_store
from test_cumulative import publish
from test_cumulative import sync as sync  # noqa: PLC0414

from asterion.data.library import DataLibrary, versions
from asterion.data.public import VersionReader
from asterion.data.scan_public import ScanRequest
from asterion.research.data_input import selected_input


def test_partition_and_column_selection_keeps_provenance_and_closes_file(sync, monkeypatch):
    from asterion.data import scanning

    record = publish(sync, ["2024-01-02", "2024-02-02", "2024-02-03"])
    reader = VersionReader(sync.engine, sync.root)
    actual = record["manifest"]["scope"]["contract_ids"][0]
    handles = []
    original = scanning.open_scan

    def track(*args, **kwargs):
        scanner = original(*args, **kwargs)
        handles.append(scanner)
        return scanner

    monkeypatch.setattr(scanning, "open_scan", track)
    monkeypatch.setattr(
        reader._library, "preview", lambda *a, **kw: pytest.fail("Scan must not use preview")
    )
    result = reader.scan(
        ScanRequest(
            version_id=record["id"],
            contract_ids=(actual,),
            start="2024-02-02",
            end="2024-02-03",
            columns=("close",),
            batch_rows=1,
        )
    )
    batch = next(result.batches)
    assert batch.num_rows == 1
    assert batch.schema.names == ["close", "_contract_id", "_observed_at", "_raw_version_id"]
    assert batch.to_pylist()[0]["_contract_id"] == actual
    assert batch.to_pylist()[0]["_raw_version_id"] == record["manifest"]["inputs"][0]
    result.batches.close()
    metrics = handles[0].metrics()
    assert metrics["closed"] is True
    assert metrics["files_opened"] == 1
    assert metrics["row_groups_decoded"] == 1
    assert metrics["rows_decoded"] == 1
    publish(sync, ["2024-02-02"], price=3220)
    frozen = list(
        reader.scan(
            ScanRequest(
                version_id=record["id"],
                contract_ids=(actual,),
                start="2024-02-02",
                end="2024-02-02",
                columns=("close",),
            )
        ).batches
    )
    assert len(frozen) == 1
    assert frozen[0].to_pylist()[0]["close"] == "3200"


def test_invalid_selection_and_corrupt_selected_file_fail(sync):
    record = publish(sync, ["2024-01-02"])
    reader = VersionReader(sync.engine, sync.root)
    query = {
        "version_id": record["id"],
        "contract_ids": tuple(record["manifest"]["scope"]["contract_ids"]),
        "start": "2024-01-02",
        "end": "2024-01-02",
        "columns": ("close",),
    }
    with pytest.raises(ValueError, match="未声明"):
        reader.scan(ScanRequest(**(query | {"columns": ("password",)})))
    with pytest.raises(ValueError, match="不在固定版本"):
        reader.scan(ScanRequest(**(query | {"contract_ids": ("SHFE.RB.204001.20200101",)})))
    checksum = record["manifest"]["partitions"][0]["checksum"]
    (sync.root / "artifacts" / f"{checksum}.parquet").write_bytes(b"damaged")
    with pytest.raises(ValueError, match="校验和"):
        list(reader.scan(ScanRequest(**query)).batches)


def test_unselected_month_is_not_opened_or_decoded(sync):
    record = publish(sync, ["2024-01-02", "2024-02-02"])
    january = record["manifest"]["partitions"][0]["checksum"]
    (sync.root / "artifacts" / f"{january}.parquet").write_bytes(b"unselected invalid parquet")
    reader = VersionReader(sync.engine, sync.root)
    result = reader.scan(
        ScanRequest(
            version_id=record["id"],
            contract_ids=tuple(record["manifest"]["scope"]["contract_ids"]),
            start="2024-02-02",
            end="2024-02-02",
            columns=("close",),
        )
    )
    assert [row["close"] for batch in result.batches for row in batch.to_pylist()] == ["3200"]


def test_large_version_small_selection_and_explicit_execution_limit(tmp_path, monkeypatch):
    from asterion.data import scanning

    # A declared synthetic long-lived identity, not a claim about a real exchange contract.
    identity = import_identity()
    catalog = identity["catalog"]
    actual = "SHFE.RB.204005.20000101"
    catalog["contracts"][0].update(
        id=actual, delivery_month="2040-05", listed_on="2000-01-01", last_trade_on="2040-05-15"
    )
    catalog["symbols"][0].update(
        symbol="SHFE.rb4005", contract_id=actual, valid_from="2000-01-01", valid_until="2040-05-15"
    )
    identity["catalog_id"] = catalog_digest(ReferenceCatalog.model_validate(catalog))
    identity["bindings"] = [
        {"contract": "SHFE.rb4005", "source": "fixture", "symbol": "SHFE.rb4005"}
    ]
    options = import_options(identity, type_id="futures.daily", frequency="1d", source_id="fixture")
    engine = create_engine(f"sqlite:///{tmp_path}/scan.db")
    library = DataLibrary(data_store(engine), tmp_path)
    rows = [
        {
            "contract": "SHFE.rb4005",
            "exchange": "SHFE",
            "symbol": "rb4005",
            "trading_day": str(date(2000, 1, 1) + timedelta(days=i)),
            "open": "100",
            "high": "101",
            "low": "99",
            "close": "100",
            "settle": "100",
            "vol": "10",
        }
        for i in range(7000)
    ]
    ImportIdentity.model_validate(identity).validate_rows(rows)
    library.types.get("futures.daily").validate(rows)
    path = tmp_path / "history.parquet"
    pq.write_table(pa.Table.from_pylist(rows), path, row_group_size=100)
    with engine.begin() as conn:
        dataset = library.ensure_collection(
            conn, "futures.daily", "local_file", {"contract_ids": [actual]}, "STANDARD"
        )
        conn.execute(
            versions.insert().values(
                id="large",
                dataset_id=dataset,
                job_id="seed",
                created_at=1,
                rows=len(rows),
                manifest={
                    "type": library.types.get("futures.daily").manifest.model_dump(),
                    "source": "local_file",
                    "layer": "STANDARD",
                    "scope": {"contract_ids": [actual]},
                    "format": "parquet",
                    "path": path.name,
                    "checksum": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "import_options": options,
                },
            )
        )
    reader = VersionReader(data_store(engine), tmp_path)
    scans = []
    original = scanning.open_scan

    def track(*args, **kwargs):
        scanner = original(*args, **kwargs)
        scans.append(scanner)
        return scanner

    monkeypatch.setattr(scanning, "open_scan", track)
    request = SimpleNamespace(
        version_id="large",
        rules=SimpleNamespace(spec=SimpleNamespace(contract=SimpleNamespace(id=actual))),
        start=date(2010, 1, 1),
        end=date(2010, 1, 5),
    )
    selected = selected_input(reader, request, 5000)
    assert len(selected["rows"]) == 5
    assert selected["version"]["rows"] == 7000
    assert scans[0].metrics()["row_groups_decoded"] == 1  # 69 unrelated groups are not decoded.
    assert scans[0].metrics()["closed"] is True
    request.start, request.end = date(2000, 1, 1), date(2019, 1, 1)
    with pytest.raises(ValueError, match="不会截断"):
        selected_input(reader, request, 5000)
    assert scans[-1].metrics()["closed"] is True
    engine.dispose()
