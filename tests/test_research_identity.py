"""Identity evidence is mandatory even when coverage completeness is waived."""

import hashlib
from copy import deepcopy
from dataclasses import replace

import pytest
from storage_support import raw_engine
from test_coverage import sync  # noqa: F401
from test_local_coverage import local  # noqa: F401
from test_research import services  # noqa: F401

from asterion.data.backup import load_evidence, validate_backup
from asterion.data.public import VersionAccess
from asterion.data.reference import ReferenceCatalog
from asterion.data.reference_store import catalog_digest
from asterion.platform.files import read_files
from asterion.platform.serialization import canonical
from asterion.research.service import validate_input


def resign(payload):
    payload["input_checksum"] = hashlib.sha256(
        canonical({k: v for k, v in payload.items() if k != "input_checksum"})
    ).hexdigest()


def test_exploration_cannot_bypass_identity(services):  # noqa: F811
    service, body, _ = services
    with pytest.raises(ValueError, match="身份目录"):
        service.submit(body.model_copy(update={"coverage_report_id": None}))
    assert service.list() == []


def test_preparation_freezes_actual_identity_and_validation_detects_substitution(services):  # noqa: F811
    service, body, _ = services
    job = service.submit(body)
    payload = job["payload"]
    assert {bar["contract_id"] for bar in payload["bars"]} == {"SHFE.RB.202405.20230516"}
    validate_input(payload)
    changed = deepcopy(payload)
    changed["bars"][0]["contract_id"] = "SHFE.RB.203405.20330516"
    resign(changed)
    with pytest.raises(ValueError, match="合约身份目录不一致"):
        validate_input(changed)
    changed = deepcopy(payload)
    changed["coverage"]["identity"]["catalog"]["contracts"][0]["last_delivery_on"] = "2024-05-20"
    resign(changed)
    with pytest.raises(ValueError, match="指纹"):
        validate_input(changed)


def test_same_code_cannot_hide_two_actual_lifecycles(services):  # noqa: F811
    service, body, _ = services
    port = service.versions
    report = deepcopy(port.coverage(body.coverage_report_id))
    catalog = report["identity"]["catalog"]
    first = catalog["contracts"][0]
    second = first | {"id": "SHFE.RB.202405.20240103", "listed_on": "2024-01-03"}
    first["last_trade_on"] = "2024-01-02"
    mapping = catalog["symbols"][0]
    later = mapping | {"contract_id": second["id"], "valid_from": "2024-01-03"}
    mapping["valid_until"] = "2024-01-02"
    catalog["contracts"].append(second)
    catalog["symbols"].append(later)
    report["identity"]["catalog_id"] = catalog_digest(ReferenceCatalog.model_validate(catalog))
    service.versions = VersionAccess(port.read, lambda _: report)
    with pytest.raises(ValueError, match="不同生命周期"):
        service.submit(body)
    assert service.list() == []


def test_backup_checks_embedded_coverage_catalogue(services):  # noqa: F811
    service, _, root = services
    with raw_engine(service.engine).connect() as conn:
        evidence = load_evidence(conn, read_files(root / "data"), lambda value: value)
    validate_backup(evidence)
    damaged = deepcopy(evidence.coverage[0])
    damaged["identity"]["catalog_id"] = "0" * 64
    with pytest.raises(ValueError, match="指纹"):
        validate_backup(replace(evidence, coverage=(damaged,)))


def test_local_code_is_explicit_and_never_inferred_from_suffix_or_case(local):  # noqa: F811
    service, daily, body = local
    report = service.coverage.check(
        daily["id"], body.model_copy(update={"reference_symbol": "rb2610"})
    )
    assert report["identity"] is None and report["status"] == "UNCONFIRMED"
    exact = service.coverage.check(daily["id"], body)
    assert exact["identity"]["symbol"] == "RB2610.SHF"
    assert exact["identity"]["catalog"]["inputs"][0]["version_id"] == body.contracts_version_id
