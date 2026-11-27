"""Dominant series of a product, from the warehouse's daily bars.

Its three parts are declared separately:

- roll basis: the largest open interest on the previous trading day, among the
  current month and later ones that settle on the day; never back to an earlier
  month;
- roll timing: a change takes effect on the next trading day, so the first day
  belongs to no month;
- adjustment: none, or ratio back-adjustment - each earlier month's prices are
  scaled by new over old settlement on the day before every roll after it, the
  last month keeps its prices.

Ratios and factors are exact decimals with eight places, rounded half up, as
the C++ Decimal computes them. It fails rather than guesses.
"""
from dataclasses import dataclass, field
from decimal import ROUND_HALF_UP, Decimal
import math

PLACES = Decimal("0.00000001")


def rounded(value: Decimal) -> Decimal:
    return value.quantize(PLACES, rounding=ROUND_HALF_UP)


def delivery_months(delivery_month: str) -> int:
    year, month = delivery_month.split("-")
    return int(year) * 12 + int(month)


def annualized_roll_yield(
    near_price: Decimal | int,
    far_price: Decimal | int,
    near_month: str,
    far_month: str,
) -> Decimal:
    """Annualized log roll yield 12 * ln(near / far) / delta_months (positive in backwardation)."""
    gap = delivery_months(far_month) - delivery_months(near_month)
    if gap <= 0 or near_price <= 0 or far_price <= 0:
        raise ValueError("roll yield requires positive prices and ascending delivery months")
    value = 12.0 * math.log(float(near_price) / float(far_price)) / gap
    return rounded(Decimal(f"{value:.12f}"))


def simple_return(current: Decimal | int, previous: Decimal | int) -> Decimal | None:
    """Single-bar simple return `(current - previous) / previous` in 8-decimal fixed point."""
    if previous <= 0 or current <= 0:
        return None
    return rounded(Decimal(current - previous) / Decimal(previous))


@dataclass
class Month:
    contract: str
    delivery_month: str
    settlement: dict = field(default_factory=dict)  # day -> Decimal
    open_interest: dict = field(default_factory=dict)
    last_bar: str = ""  # last day it traded; a traded series needs it from the roll on
    delist_date: str = ""  # exchange delist date when known; avoids looking past the window


@dataclass
class Segment:
    month: int
    begin: str
    before: str
    factor: Decimal = Decimal(1)


@dataclass
class Plan:
    days: list
    segments: list
    ratios: list  # of each segment's roll; 1 for the first
    second: dict = field(default_factory=dict)  # day -> next-dominant month index


def plan(months: list, traded: bool, end: str = "") -> Plan:
    for i in range(1, len(months)):
        if months[i].delivery_month <= months[i - 1].delivery_month:
            raise ValueError("a dominant series lists each month once, in delivery order")
    days = sorted({day for month in months for day in month.settlement if not end or day <= end})
    if len(days) < 2:
        raise ValueError("a dominant series requires at least two trading days")
    segments, ratios, second = [], [], {}
    for d in range(1, len(days)):
        day, previous = days[d], days[d - 1]
        first = segments[-1].month if segments else 0
        chosen = ending = None
        for m in range(first, len(months)):
            interest = months[m].open_interest.get(previous)
            if day not in months[m].settlement or interest is None:
                continue
            # Held past its last day, a month could not be left. The series may end in it.
            last_day = months[m].delist_date or max(months[m].settlement)
            if traded and day >= last_day and any(
                (later.delist_date or max(later.settlement)) > day for later in months[m + 1:]
            ):
                ending = m
                continue
            if chosen is None or interest > months[chosen].open_interest[previous]:
                chosen = m
        if chosen is None:
            apart = f", apart from {months[ending].contract} on its last day" if ending is not None else ""
            raise ValueError(f"no month of the series has data on {day} and open interest on "
                             f"{previous}{apart}")
        sub = None
        for m in range(chosen + 1, len(months)):
            interest = months[m].open_interest.get(previous)
            if day not in months[m].settlement or interest is None:
                continue
            if sub is None or interest > months[sub].open_interest[previous]:
                sub = m
        if sub is not None:
            second[day] = sub
        if segments and chosen == segments[-1].month:
            continue
        ratio = Decimal(1)
        if segments:
            old, new = months[segments[-1].month], months[chosen]
            # What is held of it is closed on its next bar: it needs one.
            if traded and old.last_bar < day:
                raise ValueError(f"the outgoing month has no bars from the roll day on: "
                                 f"{old.contract} from {day}")
            if previous not in old.settlement or previous not in new.settlement:
                raise ValueError(f"the roll lacks both settlement prices on the previous day: "
                                 f"{old.contract} on {day}")
            ratio = rounded(Decimal(new.settlement[previous]) / Decimal(old.settlement[previous]))
            if ratio <= 0:
                raise ValueError(f"invalid roll adjustment ratio on {day}")
        segments.append(Segment(chosen, day, previous))
        ratios.append(ratio)
    for i in range(len(segments) - 1, 0, -1):
        segments[i - 1].factor = rounded(segments[i].factor * ratios[i])
    return Plan(days, segments, ratios, second)
