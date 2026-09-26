import math
import time
from datetime import date, datetime
from decimal import Decimal
from zoneinfo import ZoneInfo

from asterion_bindings.market_feed import QuoteEvent

from asterion.connections.public import AccountSummary, Position


def money(value, nonnegative=False):
    if type(value) not in {int, float} or not math.isfinite(value) or abs(value) >= 1e100:
        return None
    return None if nonnegative and value < 0 else Decimal(str(value))


def observation(raw):
    if len(raw["accounts"]) != 1:
        raise ValueError("需要唯一的人民币账户响应")
    account = raw["accounts"][0]
    if account["CurrencyID"] != "CNY":
        raise ValueError("仅支持人民币账户观察")
    day = account["TradingDay"]
    date.fromisoformat(day)
    summary = AccountSummary(
        trading_day=day,
        balance=money(account["Balance"]),
        available=money(account["Available"]),
        margin=money(account["CurrMargin"], True),
        position_profit=money(account["PositionProfit"]),
        close_profit=money(account["CloseProfit"]),
        commission=money(account["Commission"]),
    )
    positions, seen = {}, set()
    for row in raw["positions"]:
        quantity, today = row["Position"], row["TodayPosition"]
        if type(quantity) is not int or type(today) is not int or not 0 <= today <= quantity:
            raise ValueError("无效持仓数量")
        if quantity == 0:
            continue
        if row["TradingDay"] != day or row["PosiDirection"] not in {"2", "3"}:
            raise ValueError("持仓交易日或方向不一致")
        if row["PositionDate"] not in {"1", "2"}:
            raise ValueError("无效持仓日期分类")
        key = (row["ExchangeID"], row["InstrumentID"], row["PosiDirection"], row["HedgeFlag"])
        identity = (*key, row["PositionDate"])
        if identity in seen:
            raise ValueError("重复持仓响应")
        seen.add(identity)
        if key[0] not in {"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"}:
            raise ValueError("不支持该交易所持仓")
        if key not in positions:
            positions[key] = Position(
                exchange=key[0],
                symbol=key[1],
                name=None,
                direction="long" if key[2] == "2" else "short",
                hedge=key[3],
                quantity=0,
                today=0,
                yesterday=0,
                margin=Decimal(0),
                profit=Decimal(0),
            )
        position = positions[key]
        position.quantity += quantity
        position.today += today
        position.yesterday += quantity - today
        for target, source in (("margin", "UseMargin"), ("profit", "PositionProfit")):
            value, previous = money(row[source], target == "margin"), getattr(position, target)
            setattr(
                position,
                target,
                previous + value if previous is not None and value is not None else None,
            )
    return summary, list(positions.values())


def quote(raw):
    event_at = None
    try:
        if not 0 <= raw["UpdateMillisec"] <= 999:
            raise ValueError()
        event_at = (
            datetime.strptime(raw["ActionDay"] + raw["UpdateTime"], "%Y%m%d%H:%M:%S")
            .replace(tzinfo=ZoneInfo("Asia/Shanghai"))
            .timestamp()
            + raw["UpdateMillisec"] / 1000
        )
    except (ValueError, TypeError):
        pass

    def number(value, nonnegative=False):
        value = money(value, nonnegative)
        return float(value) if value is not None else None

    return QuoteEvent(
        exchange=raw["ExchangeID"],
        symbol=raw["InstrumentID"],
        last=number(raw["LastPrice"], True),
        previous_settlement=number(raw["PreSettlementPrice"], True),
        high=number(raw["HighestPrice"], True),
        low=number(raw["LowestPrice"], True),
        volume=raw["Volume"] if type(raw["Volume"]) is int and raw["Volume"] >= 0 else None,
        open_interest=number(raw["OpenInterest"], True),
        trading_day=raw["TradingDay"],
        action_day=raw["ActionDay"],
        source_time=raw["UpdateTime"],
        event_at=event_at,
        received_at=time.time(),
    )
