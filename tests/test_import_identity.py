from copy import deepcopy
from datetime import date

import pytest
from asterion_bindings.artifacts import ArtifactStore
from asterion_bindings.catalog import ImportIdentity
from asterion_bindings.files import read_files
from import_identity_support import import_identity
from import_support import import_options, publish_import
from storage_support import raw_engine
from test_coverage import sync  # noqa: F401
from test_import_connections import CSV, options, services  # noqa: F401
from test_local_coverage import local  # noqa: F401

from asterion.data.backup import load_evidence, validate_backup


def test_identity_checks_lifecycle_availability_and_exact_file_mapping():
    value = import_identity("SHFE.rb2610")
    identity = ImportIdentity.model_validate(value)
    assert identity.resolve("SHFE.rb2610", date(2026, 9, 15)).id == "SHFE.RB.202610.20240102"
    for code, day in [("SHFE.RB2610", date(2026, 9, 15)), ("SHFE.rb2610", date(2023, 1, 1))]:
        with pytest.raises(ValueError):
            identity.resolve(code, day)
    value["information_at"] = "2019-01-01T00:00:00Z"
    with pytest.raises(ValueError):
        ImportIdentity.model_validate(value).resolve("SHFE.rb2610", date(2026, 9, 15))
    value = import_identity("SHFE.rb2610")
    value["bindings"][0]["contract"] = "SHFE.cu2610"
    with pytest.raises(ValueError, match="品种"):
        ImportIdentity.model_validate(value).resolve("SHFE.cu2610", date(2026, 9, 15))


def test_restore_rejects_import_identity_fingerprint_damage(local):  # noqa: F811
    from dataclasses import replace

    service, daily, _ = local
    with raw_engine(service.engine).connect() as conn:
        evidence = load_evidence(
            conn,
            ArtifactStore(service.root, read_only=True),
            read_files(service.root),
            lambda value: value,
        )
    validate_backup(evidence)
    rows = deepcopy(evidence.versions)
    manifest = next(row["manifest"] for row in rows if row["id"] == daily["id"])
    manifest["import_options"]["identity"]["catalog_id"] = "0" * 64
    with pytest.raises(ValueError, match="指纹"):
        validate_backup(replace(evidence, versions=rows))


def test_restore_validates_typed_arrow_bars_without_rewriting_artifacts(services, tmp_path):  # noqa: F811
    from datetime import datetime
    from decimal import Decimal
    from zipfile import ZipFile

    import pyarrow as pa
    import pyarrow.parquet as pq
    from asterion_bindings.calendar import TimeSpec, TimeVersion, time_id
    from storage_support import scheduler
    from test_trading_time import example

    service = services
    timing = TimeSpec.model_validate(example())
    body = import_options(
        import_identity("SHFE.au2506"),
        trading_time=TimeVersion(id=time_id(timing), spec=timing),
        timestamp_semantics="bar_start",
        type_id="futures.bars",
        frequency="1m",
        source_id="arrow_backup",
    )
    payload = {
        "source": "offline typed Arrow backup fixture",
        "csv": "contract,event_time,available_at,trading_day,open,high,low,close,volume\n"
        "SHFE.au2506,2025-04-11T21:00:00+08:00,2025-04-11T21:01:00+08:00,"
        "2025-04-14,100.25,101.00,100.00,100.50,2\n",
        "options": body,
    }
    service.tasks.submit("arrow-backup", "data.import_csv", payload)
    job = scheduler(service.engine).claim("import")
    publish_import(service.engine, service.root, job)
    standard = service.library.list(type_id="futures.bars", layer="STANDARD")["items"][0]

    # Reopen an independent restored artifact tree, including its real Arrow
    # physical types; JSON-only fixtures would not exercise this boundary.
    before = {
        path.relative_to(service.root).as_posix(): path.read_bytes()
        for path in service.root.rglob("*")
        if path.is_file()
    }
    archive = tmp_path / "data-backup.zip"
    with ZipFile(archive, "w") as backup:
        for name, content in before.items():
            backup.writestr(name, content)
    restored = tmp_path / "restored-artifacts"
    with ZipFile(archive) as backup:
        backup.extractall(restored)

    with raw_engine(service.engine).connect() as conn:
        evidence = load_evidence(
            conn, ArtifactStore(restored, read_only=True), read_files(restored), lambda value: value
        )
    manifest = next(row["manifest"] for row in evidence.versions if row["id"] == standard["id"])
    table = pq.read_table(restored / manifest["path"])
    assert pa.types.is_date32(table.schema.field("trading_day").type)
    assert pa.types.is_timestamp(table.schema.field("event_time").type)
    assert pa.types.is_decimal128(table.schema.field("close").type)
    row = table.to_pylist()[0]
    assert type(row["trading_day"]) is date
    assert isinstance(row["event_time"], datetime) and row["event_time"].tzinfo is not None
    assert row["close"] == Decimal("100.50000000")
    metadata = deepcopy(evidence.versions)
    assert validate_backup(evidence)["versions"] == 2
    assert evidence.versions == metadata
    for root in (service.root, restored):
        assert {
            path.relative_to(root).as_posix(): path.read_bytes()
            for path in root.rglob("*")
            if path.is_file()
        } == before


def test_missing_fixed_source_input_rejects_publication(services):  # noqa: F811
    from asterion_bindings.catalog import ReferenceCatalog, catalog_digest
    from storage_support import scheduler

    service = services
    body = options()
    evidence = body["identity"]
    evidence["catalog"]["inputs"] = [
        {"version_id": "missing", "source": "fixture", "checksum": "a" * 64}
    ]
    evidence["catalog_id"] = catalog_digest(ReferenceCatalog.model_validate(evidence["catalog"]))
    payload = {"source": "fixture", "csv": CSV, "options": body}
    service.tasks.submit("missing-input", "data.import_csv", payload)
    job = scheduler(service.engine).claim("test")
    with pytest.raises(ValueError, match="资料版本不存在"):
        publish_import(service.engine, service.root, job)
    assert service.library.list(include_archived=True)["total"] == 0


def test_coverage_cannot_reassign_imported_actual_identity(local):  # noqa: F811
    from test_coverage import contracts

    service, daily, request = local
    revised = contracts(service, listed="20240101")
    with pytest.raises(ValueError, match="导入时固定的实际合约身份不一致"):
        service.coverage.check(
            daily["id"], request.model_copy(update={"contracts_version_id": revised["id"]})
        )


def test_file_month_must_agree_with_explicit_full_delivery_month():
    value = import_identity("SHFE.rb2610")
    value["bindings"][0]["contract"] = "SHFE.rb2405"
    with pytest.raises(ValueError, match="完整交割年月不一致"):
        ImportIdentity.model_validate(value).resolve("SHFE.rb2405", date(2026, 9, 15))
