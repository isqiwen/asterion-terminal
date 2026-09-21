from import_identity_support import import_identity

"""Natural dates, exchange days, holiday exceptions and bar boundaries."""

from datetime import date, datetime

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine

from asterion.api.app import create_app
from asterion.data.importing import mapped
from asterion.data.public import ImportOptions
from asterion.platform.config import Settings
from asterion.trading_time.public import TimeSpec, TimeVersion, time_id


def example():
    return {
        "schema_version": 1,
        "exchange": "SHFE",
        "product": "AU",
        "title": "测试跨周末及假日",
        "timezone": "Asia/Shanghai",
        "calendar_source": "测试完整自然日日历",
        "night_source": "测试节前不开夜盘",
        "calendar": [
            {
                "date": f"2025-04-{i:02}",
                "is_open": i in {2, 3, 7, 8, 9, 10, 11, 14},
                "night_open": i in {2, 7, 8, 9, 10, 11, 14},
            }
            for i in range(2, 15)
        ],
        "periods": [
            {
                "start": "2025-04-03",
                "end": "2025-04-14",
                "source": "测试时段",
                "day": [
                    {"start": a, "end": b, "end_offset": 0, "phase": "continuous"}
                    for a, b in [
                        ("09:00:00", "10:15:00"),
                        ("10:30:00", "11:30:00"),
                        ("13:30:00", "15:00:00"),
                    ]
                ],
                "night": [
                    {"start": "20:55:00", "end": "21:00:00", "end_offset": 0, "phase": "auction"},
                    {
                        "start": "21:00:00",
                        "end": "02:30:00",
                        "end_offset": 1,
                        "phase": "continuous",
                    },
                ],
            }
        ],
        "exceptions": [],
    }


def resolve(stamp, value=None, boundary="event"):
    return TimeSpec.model_validate(value or example()).resolve(
        "SHFE.au2506", datetime.fromisoformat(stamp), boundary
    )


@pytest.mark.parametrize(
    "stamp,day",
    [
        ("2025-04-11T21:00:00+08:00", "2025-04-14"),
        ("2025-04-12T01:15:00+08:00", "2025-04-14"),
        ("2025-04-11T17:15:00Z", "2025-04-14"),
        ("2025-04-14T09:00:00+08:00", "2025-04-14"),
        ("2025-04-07T09:00:00+08:00", "2025-04-07"),
        ("2025-04-07T21:00:00+08:00", "2025-04-08"),
    ],
)
def test_resolve_natural_timestamp_to_trading_day(stamp, day):
    assert resolve(stamp).trading_day.isoformat() == day


@pytest.mark.parametrize(
    "stamp",
    [
        "2025-04-03T21:00:00+08:00",
        "2025-04-06T21:00:00+08:00",
        "2025-04-13T01:00:00+08:00",
        "2025-04-07T10:20:00+08:00",
        "2025-04-07T12:00:00+08:00",
        "2025-04-15T09:00:00+08:00",
    ],
)
def test_closed_holiday_eve_weekend_break_and_unknown_range_are_rejected(stamp):
    with pytest.raises(ValueError, match="交易时段"):
        resolve(stamp)


def test_auction_and_bar_end_boundary_are_explicit():
    assert resolve("2025-04-11T20:56:00+08:00").phase == "auction"
    assert resolve("2025-04-12T02:30:00+08:00", boundary="bar_end").trading_day == date(2025, 4, 14)
    with pytest.raises(ValueError):
        resolve("2025-04-12T02:30:00+08:00")
    with pytest.raises(ValueError):
        resolve("2025-04-11T20:56:00+08:00", boundary="bar_end")


def test_phase_change_and_effective_period_use_target_trading_day():
    value = example()
    second = dict(value["periods"][0])
    value["periods"][0]["end"] = "2025-04-13"
    second.update(
        start="2025-04-14",
        night=[{"start": "21:00:00", "end": "23:00:00", "end_offset": 0, "phase": "continuous"}],
    )
    value["periods"].append(second)
    assert resolve("2025-04-11T22:00:00+08:00", value).trading_day == date(2025, 4, 14)
    with pytest.raises(ValueError):
        resolve("2025-04-12T01:00:00+08:00", value)


def test_exception_can_suspend_product_without_changing_exchange_calendar():
    value = example()
    value["exceptions"] = [
        {"trading_day": "2025-04-14", "source": "测试临时停盘", "day": [], "night": []}
    ]
    with pytest.raises(ValueError, match="停盘"):
        TimeSpec.model_validate(value).daily("SHFE.au2506", date(2025, 4, 14))
    with pytest.raises(ValueError):
        resolve("2025-04-11T21:00:00+08:00", value)


def test_day_only_product_has_no_night_even_when_exchange_night_is_open():
    value = example()
    value["periods"][0]["night"] = []
    with pytest.raises(ValueError):
        resolve("2025-04-11T21:00:00+08:00", value)
    assert resolve("2025-04-14T09:00:00+08:00", value).session == "day"


@pytest.mark.parametrize("change", ["gap", "duplicate", "overlap", "missing_night", "closed_night"])
def test_incomplete_or_ambiguous_contract_fails(change):
    value = example()
    if change == "gap":
        value["calendar"].pop(3)
    if change == "duplicate":
        value["calendar"].insert(1, value["calendar"][0])
    if change == "overlap":
        value["periods"][0]["day"].append(value["periods"][0]["day"][0])
    if change == "missing_night":
        value["calendar"][0].pop("night_open")
    if change == "closed_night":
        value["calendar"][2]["night_open"] = True
    with pytest.raises(ValueError):
        TimeSpec.model_validate(value)


def test_import_cannot_accept_wrong_day_or_cross_break():
    spec = TimeSpec.model_validate(example())
    version = TimeVersion(id=time_id(spec), spec=spec)
    options = ImportOptions(
        identity=import_identity("SHFE.au2506"),
        type_id="futures.bars",
        frequency="1m",
        source_id="test",
        trading_time=version,
        timestamp_semantics="bar_end",
    )
    header = "contract,event_time,available_at,trading_day,open,high,low,close,volume\n"
    row = "SHFE.au2506,2025-04-12T02:30:00+08:00,2025-04-12T02:30:00+08:00,2025-04-14,10,11,9,10,2"
    assert mapped(header + row, options)[0]["trading_day"] == "2025-04-14"
    derived = mapped(header.replace("trading_day,", "") + row.replace("2025-04-14,", ""), options)
    assert derived[0]["trading_day"] == "2025-04-14"
    with pytest.raises(ValueError, match="交易日错误"):
        mapped(header + row.replace("2025-04-14", "2025-04-12"), options)
    with pytest.raises(ValueError):
        mapped(header + row.replace("02:30:00", "21:00:00"), options)
    with pytest.raises(ValueError):
        spec.resolve("SHFE.rb2506", datetime.fromisoformat("2025-04-14T09:00:00+08:00"))


def test_authenticated_api_persists_exact_version_and_resolves(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/time.db")
    app = create_app(
        Settings(
            token="trading-time-test-token-at-least-24", data_root=tmp_path, require_account=False
        ),
        engine,
    )
    client = TestClient(app)
    assert client.get("/api/v1/trading-time").status_code == 401
    client.headers["Authorization"] = "Bearer trading-time-test-token-at-least-24"
    saved = client.post("/api/v1/trading-time", json=example()).raise_for_status().json()
    assert client.post("/api/v1/trading-time", json=example()).json() == saved
    assert client.get("/api/v1/trading-time").json() == [saved]
    result = (
        client.post(
            "/api/v1/trading-time/resolve",
            json={
                "version": saved,
                "contract": "SHFE.au2506",
                "timestamp": "2025-04-12T01:00:00+08:00",
                "boundary": "event",
            },
        )
        .raise_for_status()
        .json()
    )
    assert result["trading_day"] == "2025-04-14"
    saved["spec"]["title"] = "改动"
    assert (
        client.post(
            "/api/v1/trading-time/resolve",
            json={
                "version": saved,
                "contract": "SHFE.au2506",
                "timestamp": "2025-04-12T01:00:00+08:00",
                "boundary": "event",
            },
        ).status_code
        == 422
    )
    app.state.plugins.close()
    engine.dispose()
