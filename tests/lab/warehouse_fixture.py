"""A small warehouse for lab's tests: SHFE/rb with two months over 30 weekdays.

The end-to-end tests write it into an isolated Terminal's lab directory:
python3 -I -B warehouse_fixture.py <lab-source> <lab-directory>
"""
import datetime
from decimal import Decimal
from pathlib import Path
import sys

if __name__ == "__main__":
    sys.path.insert(0, sys.argv.pop(1))
from asterion_lab.warehouse import Warehouse  # noqa: E402

FIRST = datetime.date(2026, 1, 5)


def weekdays(count):
    days, day = [], FIRST
    while len(days) < count:
        if day.weekday() < 5:
            days.append(day)
        day += datetime.timedelta(days=1)
    return days


DAYS = weekdays(30)


def daily(contract, day, close, interest, auction=True):
    exchange, product, _ = contract.split("/")
    close = Decimal(close)
    return dict(exchange=exchange, product=product, contract=contract, day=day.isoformat(),
                open=close if auction else None, high=close + 15 if auction else None,
                low=close - 15 if auction else None, close=close if auction else None,
                settlement=close, previous_close=None, previous_settlement=None,
                volume=Decimal(100), amount=Decimal(100) * close * 10, open_interest=Decimal(interest),
                open_interest_change=None, delivery_settlement=None)


def contract_row(contract):
    exchange, product, month = contract.split("/")
    return dict(exchange=exchange, contract=contract, product=product, delivery_month=month,
                source_instrument=contract, name=contract, list_date="2025-01-02",
                delist_date="2026-12-15", last_delivery_date=None, multiplier=Decimal(10),
                per_unit=Decimal(1), trade_unit="吨", quote_unit="元/吨",
                tick_description="1元/吨", delivery_description="", trading_hours="")


def closes(count):
    """Up for a while, then down, each leg with pullbacks: rules that follow a
    trend take a side, and a price decided at a close comes back to fill."""
    result, close = [], 3000
    for i in range(count):
        rising = i < count // 2
        close += (30 if i % 2 == 0 else -10) * (1 if rising else -1)
        result.append(close)
    return result


def warehouse(directory: Path):
    """SHFE/rb with two months: 2605 most held until day 15, then 2610."""
    rows = []
    for i, (day, close) in enumerate(zip(DAYS, closes(len(DAYS)))):
        rows.append(daily("SHFE/rb/2026-05", day, close, 1000 if i < 15 else 100))
        rows.append(daily("SHFE/rb/2026-10", day, close + 20, 500 if i < 15 else 2000))
    writer = Warehouse(directory).writer()
    writer.put_rows("calendar/SHFE/2026", "calendar", [
        dict(exchange="SHFE", day=day.isoformat(), open=True, previous_trading_day=None)
        for day in DAYS])
    writer.put_rows("contracts/SHFE", "contracts",
                    [contract_row("SHFE/rb/2026-05"), contract_row("SHFE/rb/2026-10")])
    writer.add_days("SHFE", "2026-01", [row for row in rows if row["day"] < "2026-02"],
                    [day.isoformat() for day in DAYS if day.isoformat() < "2026-02"])
    writer.add_days("SHFE", "2026-02", [row for row in rows if row["day"] >= "2026-02"],
                    [day.isoformat() for day in DAYS if day.isoformat() >= "2026-02"])
    writer.commit()


if __name__ == "__main__":
    target = Path(sys.argv[1])
    target.mkdir(parents=True, exist_ok=True)
    warehouse(target)
