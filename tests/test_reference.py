from copy import deepcopy

import pytest
from pydantic import ValidationError

from asterion.data.reference import ReferenceCatalog


def fixture():
    source = {
        "source": "synthetic-contract-test",
        "source_version": "1",
        "observed_at": "2026-01-01T00:00:00Z",
        "available_at": "2026-01-01T00:01:00Z",
    }
    return {
        "schema_version": 2,
        "inputs": [],
        "products": [
            {
                "id": "SHFE.RB",
                "exchange": "SHFE",
                "name": "test",
                "currency": "CNY",
                "provenance": source,
            }
        ],
        "symbols": [
            {
                "source": "tushare",
                "symbol": "RB2610.SHF",
                "contract_id": "SHFE.RB.202610.20251001",
                "valid_from": "2025-10-01",
                "valid_until": "2026-10-15",
                "provenance": source,
            }
        ],
        "contracts": [
            {
                "id": "SHFE.RB.202610.20251001",
                "product_id": "SHFE.RB",
                "delivery_month": "2026-10",
                "listed_on": "2025-10-01",
                "last_trade_on": "2026-10-15",
                "last_delivery_on": None,
                "provenance": source,
            }
        ],
    }


def test_catalog_roundtrip():
    catalog = ReferenceCatalog.model_validate(fixture())
    assert ReferenceCatalog.model_validate_json(catalog.model_dump_json()) == catalog


@pytest.mark.parametrize(
    "change",
    [
        "unknown_product",
        "missing_version",
        "invalid_lifecycle",
        "duplicate_contract",
        "invalid_delivery",
        "unknown_field",
    ],
)
def test_reference_rejects_inconsistent_data(change):
    value = fixture()
    if change == "unknown_product":
        value["contracts"][0]["product_id"] = "DCE.m"
    if change == "missing_version":
        value.pop("schema_version")
    if change == "invalid_lifecycle":
        value["contracts"][0]["last_trade_on"] = "2025-09-30"
    if change == "duplicate_contract":
        value["contracts"].append(deepcopy(value["contracts"][0]))
    if change == "invalid_delivery":
        value["contracts"][0]["last_delivery_on"] = "2026-10-14"
    if change == "unknown_field":
        value["unknown"] = []
    with pytest.raises(ValidationError):
        ReferenceCatalog.model_validate(value)


def query(symbol="RB2610.SHF", day="2026-09-14", information="2026-09-14T00:00:00Z"):
    from asterion.data.reference import ResolutionRequest

    return ResolutionRequest.model_validate(
        {
            "source": "tushare",
            "symbol": symbol,
            "trading_day": day,
            "information_at": information,
        }
    )


def test_resolution_pins_actual_identity_and_requires_available_evidence():
    catalog = ReferenceCatalog.model_validate(fixture())
    assert catalog.resolve(query()).contract.id == "SHFE.RB.202610.20251001"
    for day in ("2025-10-01", "2026-10-15"):
        assert catalog.resolve(query(day=day)).contract == catalog.contracts[0]
    for request in (
        query(symbol="rb2610.shf"),
        query(symbol="RB.SHF"),
        query(day="2025-09-30"),
        query(day="2026-10-16"),
        query(information="2026-01-01T00:00:30Z"),
    ):
        with pytest.raises(ValueError):
            catalog.resolve(request)


def test_three_digit_code_reuse_resolves_by_lifecycle_not_current_year():
    value = fixture()
    product = value["products"][0]
    product.update(id="CZCE.MA", exchange="CZCE")
    first = value["contracts"][0]
    first.update(
        id="CZCE.MA.202605.20250501",
        product_id="CZCE.MA",
        delivery_month="2026-05",
        listed_on="2025-05-01",
        last_trade_on="2026-05-15",
    )
    mapping = value["symbols"][0]
    mapping.update(
        symbol="MA605.ZCE",
        contract_id=first["id"],
        valid_from=first["listed_on"],
        valid_until=first["last_trade_on"],
    )
    second = deepcopy(first)
    second.update(
        id="CZCE.MA.203605.20350501",
        delivery_month="2036-05",
        listed_on="2035-05-01",
        last_trade_on="2036-05-15",
    )
    later = deepcopy(mapping)
    later.update(
        contract_id=second["id"],
        valid_from=second["listed_on"],
        valid_until=second["last_trade_on"],
    )
    value["contracts"].append(second)
    value["symbols"].append(later)
    catalog = ReferenceCatalog.model_validate(value)
    assert catalog.resolve(query("MA605.ZCE", "2026-05-01")).contract.id == first["id"]
    assert (
        catalog.resolve(query("MA605.ZCE", "2036-05-01", "2036-05-01T00:00:00Z")).contract.id
        == second["id"]
    )
    with pytest.raises(ValueError):
        catalog.resolve(query("MA605.ZCE", "2030-05-01"))


@pytest.mark.parametrize(
    "damage", ["overlap", "unknown", "outside", "identity", "missing_month", "reversed"]
)
def test_identity_catalog_rejects_conflicts(damage):
    value = fixture()
    if damage == "overlap":
        value["symbols"].append(deepcopy(value["symbols"][0]))
    elif damage == "unknown":
        value["symbols"][0]["contract_id"] = "unknown"
    elif damage == "outside":
        value["symbols"][0]["valid_from"] = "2025-09-30"
    elif damage == "identity":
        value["contracts"][0]["id"] = "SHFE.RB.203610.20251001"
    elif damage == "missing_month":
        value["contracts"][0].pop("delivery_month")
    else:
        value["symbols"][0]["valid_until"] = "2025-09-30"
    with pytest.raises(ValidationError):
        ReferenceCatalog.model_validate(value)
