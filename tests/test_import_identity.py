from copy import deepcopy
from datetime import date

import pytest
from import_identity_support import import_identity
from storage_support import raw_engine
from test_coverage import sync  # noqa: F401
from test_import_connections import CSV, options, services  # noqa: F401
from test_local_coverage import local  # noqa: F401

from asterion.data.backup import load_evidence, validate_backup
from asterion.data.import_identity import ImportIdentity
from asterion.data.importing import encode_import, preview
from asterion.data.public import ImportOptions
from asterion.platform.files import read_files


def test_missing_identity_is_not_admitted():
    body = options().model_dump(mode="json")
    del body["identity"]
    with pytest.raises(ValueError, match="identity"):
        ImportOptions.model_validate(body)


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


def test_preview_rejects_unmapped_file_and_altered_catalogue():
    assert not preview(CSV.replace("SHFE.rb2610", "SHFE.rb2609"), options()).valid
    body = options().model_dump(mode="json")
    body["identity"]["catalog"]["contracts"][0]["last_trade_on"] = "2026-10-16"
    with pytest.raises(ValueError, match="指纹"):
        encode_import({"csv": CSV, "options": body})


def test_publication_revalidates_before_writing_any_snapshot(services):  # noqa: F811
    from storage_support import scheduler

    service, snapshots = services
    payload = {"source": "fixture", "csv": CSV, "options": options().model_dump(mode="json")}
    # Construct a worker artifact from valid input; submit a different, invalid authoritative input.
    content, _ = encode_import(payload)
    payload["csv"] = CSV.replace("2024-01-02", "2023-01-02")
    service.tasks.submit("bad-identity", "data.import_csv", payload)
    claimed = scheduler(service.engine).claim("worker")
    with pytest.raises(ValueError):
        snapshots.publish(claimed["id"], claimed["token"], content)
    assert snapshots.list() == []
    assert service.library.list()["total"] == 0
    assert not list((snapshots.root / "published").iterdir())


def test_restore_rejects_import_identity_fingerprint_damage(local):  # noqa: F811
    from dataclasses import replace

    service, daily, _ = local
    with raw_engine(service.engine).connect() as conn:
        evidence = load_evidence(conn, read_files(service.root), lambda value: value)
    validate_backup(evidence)
    rows = deepcopy(evidence.versions)
    manifest = next(row["manifest"] for row in rows if row["id"] == daily["id"])
    manifest["import_options"]["identity"]["catalog_id"] = "0" * 64
    with pytest.raises(ValueError, match="指纹"):
        validate_backup(replace(evidence, versions=rows))


def test_missing_fixed_source_input_rejects_publication(services):  # noqa: F811
    from storage_support import scheduler

    from asterion.data.reference import ReferenceCatalog
    from asterion.data.reference_store import catalog_digest

    service, snapshots = services
    body = options().model_dump(mode="json")
    evidence = body["identity"]
    evidence["catalog"]["inputs"] = [
        {"version_id": "missing", "source": "fixture", "checksum": "a" * 64}
    ]
    evidence["catalog_id"] = catalog_digest(ReferenceCatalog.model_validate(evidence["catalog"]))
    payload = {"source": "fixture", "csv": CSV, "options": body}
    artifact, _ = encode_import(payload)
    service.tasks.submit("missing-input", "data.import_csv", payload)
    job = scheduler(service.engine).claim("test")
    with pytest.raises(ValueError, match="资料版本不存在"):
        snapshots.publish(job["id"], job["token"], artifact)
    assert snapshots.list() == []
    assert service.library.list()["total"] == 0


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
