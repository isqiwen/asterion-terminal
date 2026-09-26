"""Explicit, synthetic identity evidence for tests; never a production catalogue."""

from asterion_bindings.catalog import ImportIdentity, ReferenceCatalog, catalog_digest


def import_identity(contract="SHFE.rb2405"):
    product, month, listed, last = {
        "DCE.m2405": ("DCE.M", "2024-05", "2023-05-16", "2024-05-15"),
        "SHFE.rb2405": ("SHFE.RB", "2024-05", "2023-05-16", "2024-05-15"),
        "SHFE.rb2610": ("SHFE.RB", "2026-10", "2024-01-02", "2026-10-15"),
        "SHFE.au2506": ("SHFE.AU", "2025-06", "2024-06-17", "2025-06-16"),
    }[contract]
    actual_id = product + "." + month.replace("-", "") + "." + listed.replace("-", "")
    provenance = {
        "source": "fixture",
        "source_version": "1",
        "observed_at": "2020-01-01T00:00:00Z",
        "available_at": "2020-01-01T00:00:00Z",
    }
    catalog = ReferenceCatalog.model_validate(
        {
            "schema_version": 2,
            "inputs": [],
            "products": [
                {
                    "id": product,
                    "exchange": product.split(".")[0],
                    "name": "Explicit test fixture",
                    "currency": "CNY",
                    "provenance": provenance,
                }
            ],
            "contracts": [
                {
                    "id": actual_id,
                    "product_id": product,
                    "delivery_month": month,
                    "listed_on": listed,
                    "last_trade_on": last,
                    "last_delivery_on": None,
                    "provenance": provenance,
                }
            ],
            "symbols": [
                {
                    "source": "fixture",
                    "symbol": contract,
                    "contract_id": actual_id,
                    "valid_from": listed,
                    "valid_until": last,
                    "provenance": provenance,
                }
            ],
        }
    )
    return ImportIdentity(
        catalog_id=catalog_digest(catalog),
        catalog=catalog,
        information_at="2026-09-20T00:00:00Z",
        bindings=[{"contract": contract, "source": "fixture", "symbol": contract}],
    ).model_dump(mode="json")


def source_identity(contract, source, symbol):
    from asterion_bindings.catalog import SourceIdentity

    value = import_identity(contract)
    catalog = value["catalog"]
    catalog["inputs"] = [{"version_id": "fixture-source", "source": source, "checksum": "a" * 64}]
    for item in [*catalog["products"], *catalog["contracts"], *catalog["symbols"]]:
        item["provenance"].update(source=source, source_version="fixture-source")
    catalog["symbols"][0].update(source=source, symbol=symbol)
    parsed = ReferenceCatalog.model_validate(catalog)
    return SourceIdentity(
        catalog_id=catalog_digest(parsed),
        catalog=parsed,
        source=source,
        symbol=symbol,
        information_at=value["information_at"],
    ).model_dump(mode="json")
