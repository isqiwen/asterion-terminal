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
        "products": [
            {
                "id": "SHFE.rb",
                "exchange": "SHFE",
                "name": "test",
                "currency": "CNY",
                "provenance": source,
            }
        ],
        "contracts": [
            {
                "id": "SHFE.rb2610",
                "product_id": "SHFE.rb",
                "delivery_month": "2026-10",
                "listed_at": "2025-10-01T00:00:00Z",
                "last_trade_at": "2026-10-15T07:00:00Z",
                "multiplier": "10",
                "tick_size": "1",
                "provenance": source,
            }
        ],
        "calendars": [
            {
                "product_id": "SHFE.rb",
                "version": "1",
                "provenance": source,
                "days": [
                    {
                        "trading_day": "2026-09-21",
                        "sessions": [
                            {
                                "opens_at": "2026-09-18T21:00:00+08:00",
                                "closes_at": "2026-09-18T23:00:00+08:00",
                                "kind": "night",
                            }
                        ],
                    }
                ],
            }
        ],
        "rules": [
            {
                "contract_id": "SHFE.rb2610",
                "version": "1",
                "effective_from": "2026-01-01T00:00:00Z",
                "margin_rate": "0.1",
                "open_fee": {"per_lot": "1"},
                "close_fee": {"per_lot": "1"},
                "close_today_fee": {"per_lot": "2"},
                "requires_close_today": True,
                "provenance": source,
            }
        ],
    }


def test_night_session_explicitly_belongs_to_next_trading_day():
    catalog = ReferenceCatalog.model_validate(fixture())
    day = catalog.calendars[0].days[0]
    assert day.sessions[0].opens_at.date() < day.trading_day
    assert ReferenceCatalog.model_validate_json(catalog.model_dump_json()) == catalog


@pytest.mark.parametrize(
    "change",
    [
        "unknown_product",
        "overlap",
        "naive_time",
        "duplicate_contract",
        "invalid_tick",
        "closed_sessions",
    ],
)
def test_reference_rejects_inconsistent_data(change):
    value = fixture()
    if change == "unknown_product":
        value["contracts"][0]["product_id"] = "DCE.m"
    if change == "overlap":
        other = deepcopy(value["rules"][0])
        other["version"] = "2"
        value["rules"].append(other)
    if change == "naive_time":
        value["contracts"][0]["listed_at"] = "2025-10-01T00:00:00"
    if change == "duplicate_contract":
        value["contracts"].append(deepcopy(value["contracts"][0]))
    if change == "invalid_tick":
        value["contracts"][0]["tick_size"] = "0"
    if change == "closed_sessions":
        value["calendars"][0]["days"][0]["closed"] = True
    with pytest.raises(ValidationError):
        ReferenceCatalog.model_validate(value)
