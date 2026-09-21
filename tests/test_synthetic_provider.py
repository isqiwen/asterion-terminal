"""The demonstration adapter must be reproducible, offline, and clearly fictional."""

from datetime import date

import httpx
import pytest
from synthetic_provider import CONTRACT, SYMBOL, Synthetic

from asterion.data.providers.public import ProviderError, SyncRequest
from asterion.data.types import builtin_types


@pytest.fixture(autouse=True)
def offline(monkeypatch):
    def forbidden(*args, **kwargs):
        raise AssertionError("The demonstration provider must not use network transport")

    monkeypatch.setattr(httpx.Client, "send", forbidden)
    monkeypatch.setattr(httpx.AsyncClient, "send", forbidden)
    monkeypatch.setattr("socket.socket.connect", forbidden)
    monkeypatch.setattr("socket.create_connection", forbidden)


def request(dataset="daily", **updates):
    data = {
        "command_id": "demo",
        "provider": "synthetic",
        "dataset": dataset,
        "exchange": "SIM",
        "symbol": SYMBOL if dataset == "daily" else "",
    }
    if dataset != "contracts":
        data |= {"start": "2024-01-02", "end": "2024-01-12"}
    return SyncRequest.model_validate(data | updates)


def fetch(provider, value, configuration=None):
    return [
        row
        for partition in provider.plan(value)
        for row in provider.fetch(partition, configuration or {})
    ]


def test_manifest_and_probe_are_usable_without_credentials():
    provider = Synthetic()
    assert provider.manifest.api_version == 2
    assert provider.manifest.demo
    assert "非真实行情" in provider.manifest.name
    assert all(capability.exchanges == ["SIM"] for capability in provider.manifest.capabilities)
    assert "无需网络或凭据" in provider.probe({})
    assert provider.manifest.configuration.fields[0].default == 7
    assert not provider.manifest.configuration.fields[0].secret


@pytest.mark.parametrize("dataset", ["contracts", "calendar", "daily"])
def test_all_capabilities_publish_standard_types_without_network(dataset):
    provider = Synthetic()
    value = request(
        dataset, **({} if dataset == "contracts" else {"start": "2024-01-01", "end": "2024-01-31"})
    )
    partitions = provider.plan(value)
    rows = fetch(provider, value)
    assert all(set(row) == set(partitions[0].fields) for row in rows)
    assert len(rows) < partitions[0].limit
    normalized = provider.normalize(value, rows)
    data_type = builtin_types().get(f"futures.{dataset}")
    data_type.validate(normalized)
    assert normalized and all(row["exchange"] == "SIM" for row in normalized)
    if dataset == "calendar":
        assert data_type.coverage(normalized, value.start, value.end) == "CALENDAR_COMPLETE"
        assert len(normalized) == 31
        # Jan 1 is deliberately an open day in the fictional weekday calendar.
        assert normalized[0] == {
            "exchange": "SIM",
            "date": "2024-01-01",
            "is_open": 1,
            "previous_trading_day": None,
        }
        assert normalized[5]["is_open"] == 0
    elif dataset == "contracts":
        assert len(normalized) == 1
        assert normalized[0]["symbol"] == SYMBOL
        assert normalized[0]["listed"] == "2024-01-01"
        assert normalized[0]["delisted"] == "2024-01-31"
        assert "不可交易" in normalized[0]["name"]
    else:
        assert len(normalized) == 23
        assert all(row["contract"] == CONTRACT for row in normalized)


def test_daily_seed_is_deterministic_and_independent_of_request_boundaries():
    provider = Synthetic()
    full = request(start="2024-01-01", end="2024-01-31")
    rows = fetch(provider, full, {"seed": 7})
    assert fetch(provider, full) == rows
    assert fetch(Synthetic(), full, {"seed": 7}) == rows
    assert fetch(provider, full, {"seed": 8}) != rows
    left = fetch(provider, request(start="2024-01-01", end="2024-01-14"))
    right = fetch(provider, request(start="2024-01-15", end="2024-01-31"))
    assert left + right == rows
    for seed in (0, 10000):
        provider.normalize(full, fetch(provider, full, {"seed": seed}))


def test_calendar_is_independent_of_request_boundaries():
    provider = Synthetic()
    rows = fetch(provider, request("calendar", start="2024-01-01", end="2024-01-31"))
    restricted = request("calendar", start="2024-01-08", end="2024-01-14")
    subset = fetch(provider, restricted)
    assert subset == rows[7:14]
    assert subset[0]["previous_day"] == "2024-01-05"
    provider.normalize(restricted, subset)


@pytest.mark.parametrize(
    "configuration",
    [
        {"seed": -1},
        {"seed": 10001},
        {"seed": True},
        {"seed": "7"},
        {"seed": 1.5},
        {"unknown": "value"},
    ],
)
def test_invalid_configuration_is_rejected(configuration):
    provider = Synthetic()
    with pytest.raises(ProviderError, match="种子"):
        provider.probe(configuration)
    with pytest.raises(ProviderError, match="种子"):
        fetch(provider, request(), configuration)


@pytest.mark.parametrize(
    "value",
    [
        request(exchange="SHFE"),
        request(symbol="RB2401.SHF"),
        request(symbol="OTHER001.SIM"),
        request(start="2023-12-31"),
        request(end="2024-02-01"),
        request(provider="tushare"),
        request("calendar", symbol=SYMBOL),
        request("contracts", symbol=SYMBOL),
        request("contracts", start="2024-01-01", end="2024-01-02"),
        request("other"),
    ],
)
def test_plan_rejects_outside_demo_scope(value):
    with pytest.raises(ProviderError):
        Synthetic().plan(value)


@pytest.mark.parametrize(
    "updates",
    [
        {"api": "fut_daily"},
        {"limit": 10000},
        {"fields": ["open"]},
        {
            "params": {
                "exchange": "SIM",
                "start_date": "2025-01-01",
                "end_date": "2025-01-31",
                "symbol": SYMBOL,
            }
        },
    ],
)
def test_fetch_rejects_tampered_partitions(updates):
    provider = Synthetic()
    partition = provider.plan(request())[0].model_copy(update=updates)
    with pytest.raises(ProviderError):
        provider.fetch(partition, {})


@pytest.mark.parametrize(
    "changes",
    [
        {"instrument": "RB2401.SHF"},
        {"venue": "SHFE"},
        {"day": "2024-01-01"},
        {"day": "2024-01-06"},
        {"day": "20240102"},
        {"open": "NaN"},
        {"open": "0"},
        {"open": "1.123456789"},
        {"open": "1000000000000"},
        {"high": "1"},
        {"low": "100000"},
        {"volume": "0.1"},
        {"volume": "-1"},
        {"open_interest": "1.25"},
        {"turnover": "-1"},
        {"new_field": "untrusted"},
    ],
)
def test_normalize_rejects_invalid_daily_rows(changes):
    provider = Synthetic()
    value = request()
    row = fetch(provider, value)[0] | changes
    with pytest.raises(ProviderError):
        provider.normalize(value, [row])


@pytest.mark.parametrize("dataset", ["contracts", "calendar", "daily"])
def test_normalize_rejects_duplicate_or_incomplete_rows(dataset):
    provider = Synthetic()
    value = request(dataset)
    row = fetch(provider, value)[0]
    with pytest.raises(ProviderError, match="重复"):
        provider.normalize(value, [row, row])
    incomplete = dict(row)
    del incomplete["venue"]
    with pytest.raises(ProviderError):
        provider.normalize(value, [incomplete])


def test_normalize_rejects_calendar_rule_changes_and_real_contracts():
    provider = Synthetic()
    value = request("calendar")
    row = fetch(provider, value)[0]
    for change in ({"open": 0}, {"open": True}, {"previous_day": None}):
        with pytest.raises(ProviderError):
            provider.normalize(value, [row | change])
    value = request("contracts")
    row = fetch(provider, value)[0]
    for change in ({"instrument": "RB2401.SHF"}, {"delisted": "2025-01-31"}):
        with pytest.raises(ProviderError):
            provider.normalize(value, [row | change])


def test_weekend_only_daily_response_is_empty_and_does_not_invent_bars():
    provider = Synthetic()
    value = request(start="2024-01-06", end="2024-01-07")
    assert fetch(provider, value) == []
    assert provider.normalize(value, []) == []
    assert value.start == date(2024, 1, 6)
