from scan_support import unsupported_scan

"""Explicit standard source observations used by role publication tests."""

from asterion.data.public import VersionAccess


def sources():
    def version(identifier, type_id, schema, observed):
        return {
            "id": identifier,
            "created_at": 1,
            "manifest": {
                "type": {"id": type_id, "schema_version": schema},
                "source": "offline-feed",
                "layer": "STANDARD",
                "state": "PUBLISHED",
                "checksum": "a" * 64,
                "observed_at": observed,
                "available_at": observed,
            },
        }

    mapping = {
        "version": version("report-1", "futures.role_mapping", 1, "2025-04-15T00:00:00Z"),
        "total": 1,
        "rows": [
            {
                "symbol": "opaque-role",
                "target_symbol": "opaque-au",
                "product_id": "SHFE.AU",
                "exchange": "SHFE",
                "role": "main",
                "trading_day": "2025-04-14",
                "available_at": "2025-04-11T20:00:00+08:00",
            }
        ],
    }
    contracts = {
        "version": version("contracts-1", "futures.contracts", 4, "2020-01-01T00:00:00Z"),
        "total": 1,
        "rows": [
            {
                "exchange": "SHFE",
                "symbol": "opaque-au",
                "contract": "SHFE.au2506",
                "suggested_multiplier": None,
                "multiplier_note": "fixture",
                "name": "fixture",
                "product": "AU",
                "currency": "CNY",
                "delivery_month": "2025-06",
                "listed": "2024-06-17",
                "delisted": "2025-06-16",
                "last_delivery_on": None,
                "multiplier": None,
                "quote_unit_desc": None,
            }
        ],
    }
    return {"report-1": mapping, "contracts-1": contracts}


def port(values):
    return VersionAccess(
        lambda identifier, **kw: values[identifier], lambda _: None, unsupported_scan
    )
