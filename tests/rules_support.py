"""Offline contract rule fixture; never registered in the shipped product."""

from storage_support import raw_engine

from asterion.contract_rules.public import RuleAccess, RuleSpec, RuleVersion, rule_id
from asterion.contract_rules.service import Rules
from asterion.data.public import VersionAccess
from asterion.distribution_storage import rule_storage


def rule_version(contract="SHFE.rb2405"):
    from import_identity_support import import_identity

    actual = import_identity(contract)["catalog"]["contracts"][0]
    spec = RuleSpec(
        contract=actual,
        basis=None,
        trading_time=time_version(contract),
        title="测试规则",
        source="离线测试构造",
        multiplier="10",
        tick_size="1",
        periods=[
            {
                "start": "2024-01-01",
                "end": "2024-12-31",
                "margin_rate": "0.1",
                "fee_mode": "per_lot",
                "open_fee": "2",
                "close_fee": "2",
                "settlement_basis": None,
            }
        ],
    )
    return RuleVersion(id=rule_id(spec), spec=spec)


def rule_access(engine, contract="SHFE.rb2405"):
    service = Rules(rule_storage(raw_engine(engine)), manual_versions())
    service.save(rule_version(contract).spec)
    return RuleAccess(service.read)


def manual_versions():
    def unavailable(*args, **kwargs):
        raise AssertionError("Manual rule fixture must not resolve a source")

    return VersionAccess(unavailable, unavailable)


def time_version(contract="SHFE.rb2405", start="2024-01-01", end="2024-12-31"):
    import re
    from datetime import date, timedelta

    from asterion.trading_time.public import TimeSpec, TimeVersion, time_id

    first, last = date.fromisoformat(start), date.fromisoformat(end)
    # All dates open is an explicit synthetic calendar, never a shipped default.
    calendar = [
        {"date": (first + timedelta(days=i)).isoformat(), "is_open": True, "night_open": False}
        for i in range(-1, (last - first).days + 1)
    ]
    spec = TimeSpec.model_validate(
        {
            "schema_version": 1,
            "exchange": contract.split(".")[0],
            "product": re.sub(r"[0-9]+$", "", contract.split(".")[1]).upper(),
            "title": "离线测试日历",
            "timezone": "Asia/Shanghai",
            "calendar_source": "测试构造",
            "night_source": "测试全日盘",
            "calendar": calendar,
            "periods": [
                {
                    "start": start,
                    "end": end,
                    "source": "测试构造",
                    "day": [
                        {
                            "start": "09:00:00",
                            "end": "15:00:00",
                            "end_offset": 0,
                            "phase": "continuous",
                        }
                    ],
                    "night": [],
                }
            ],
            "exceptions": [],
        }
    )
    return TimeVersion(id=time_id(spec), spec=spec)


def intraday_options():
    from import_identity_support import import_identity

    from asterion.trading_time.public import TimeSpec, TimeVersion, time_id

    spec = time_version("SHFE.rb2610", "2026-09-15", "2026-09-15").spec.model_dump(mode="json")
    spec["calendar"][0]["night_open"] = True
    spec["periods"][0]["night"] = [
        {"start": "21:00:00", "end": "23:00:00", "end_offset": 0, "phase": "continuous"}
    ]
    parsed = TimeSpec.model_validate(spec)
    return {
        "identity": import_identity("SHFE.rb2610"),
        "source_id": "fixture",
        "type_id": "futures.bars",
        "frequency": "1m",
        "timestamp_semantics": "bar_start",
        "trading_time": TimeVersion(id=time_id(parsed), spec=parsed).model_dump(mode="json"),
    }
