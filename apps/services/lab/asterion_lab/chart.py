"""A contract's bars for the market page's history chart: stored minutes
gathered into longer periods, or daily bars into calendar periods, with MACD.

Display only: nothing that trades or tests a strategy reads these. Prices and
volumes stay exact decimals; MACD is floating point.

- N-minute bars gather a trading day's minutes by their start, N minutes of
  the clock at a time (a break shortens the period it falls in). A bar is
  labelled when its period ends; it is partial when any of its minutes is.
- Daily bars are the days the contract traded in the opening auction. Weeks
  run Monday to Sunday; months, quarters and years are the calendar's. A
  period keeps its first open, highest high, lowest low, last close and the
  sum of volume, and is labelled with its last day. No day or period is
  invented.
- MACD: EMAs of 12 and 26 closes seeded with the first close, a signal EMA of
  9 starting at zero, histogram twice the difference; the first 33 bars have
  none.
"""
import datetime
from decimal import Decimal

MINUTES = (1, 5, 10, 15, 30, 60)
CALENDAR = ("day", "week", "month", "quarter", "year")
BEIJING = datetime.timezone(datetime.timedelta(hours=8))


def minute_bars(rows, minutes: int) -> list:
    """`rows`: (trading_day, start_ms, partial, open, high, low, close, volume)
    in time order, optionally followed by open interest, which a bar keeps as
    of its last minute. Returns dicts with the period's end as `time_ms`."""
    if minutes not in MINUTES:
        raise ValueError(f"minute periods are {', '.join(map(str, MINUTES))}")
    span = minutes * 60_000
    bars = []
    for day, start, partial, open_, high, low, close, volume, *interest in rows:
        if None in (open_, high, low, close):
            continue
        key = (day, start // span)
        if bars and bars[-1]["key"] == key:
            bar = bars[-1]
            bar["high"], bar["low"] = max(bar["high"], high), min(bar["low"], low)
            bar["close"], bar["volume"] = close, bar["volume"] + (volume or 0)
            bar["partial"] = bar["partial"] or bool(partial)
            bar["open_interest"] = interest[0] if interest else None
            continue
        bars.append({"key": key, "trading_day": day, "time_ms": (key[1] + 1) * span,
                     "open": open_, "high": high, "low": low, "close": close,
                     "volume": volume or Decimal(0), "partial": bool(partial),
                     "open_interest": interest[0] if interest else None})
    for bar in bars:
        del bar["key"]
    return bars


def calendar_bars(rows, period: str) -> list:
    """`rows`: (day, open, high, low, close, volume) in day order. Days
    without an opening auction are left out."""
    if period not in CALENDAR:
        raise ValueError(f"calendar periods are {', '.join(CALENDAR)}")
    bars, key = [], None
    for day, open_, high, low, close, volume in rows:
        if None in (open_, high, low, close):
            continue
        if period == "day":
            current = day
        elif period == "week":
            current = day - datetime.timedelta(days=day.weekday())
        else:
            size = {"month": 1, "quarter": 3, "year": 12}[period]
            current = (day.year, (day.month - 1) // size)
        if bars and current == key:
            bar = bars[-1]
            bar["high"], bar["low"] = max(bar["high"], high), min(bar["low"], low)
            bar["close"], bar["volume"] = close, bar["volume"] + (volume or 0)
            bar["trading_day"] = day
        else:
            bars.append({"trading_day": day, "open": open_, "high": high, "low": low,
                         "close": close, "volume": volume or Decimal(0), "partial": False})
        key = current
    for bar in bars:
        # Labelled at 15:00 Beijing time of the period's last day.
        bar["time_ms"] = int(datetime.datetime.combine(
            bar["trading_day"], datetime.time(15), BEIJING).timestamp()) * 1000
    return bars


def macd(closes) -> list:
    """(diff, signal, histogram) of each close, None for the first 33."""
    result, fast, slow, signal = [], 0.0, 0.0, 0.0
    for count, close in enumerate(closes, 1):
        value = float(close)
        if count == 1:
            fast = slow = value
        else:
            fast += (value - fast) * (2 / 13)
            slow += (value - slow) * (2 / 27)
        diff = fast - slow
        signal += (diff - signal) * 0.2
        result.append(None if count < 34 else (diff, signal, 2 * (diff - signal)))
    return result
