"""A contract's bars for the market page's history chart: stored minutes
gathered into longer periods, or daily bars into calendar periods, with MACD.

Minute aggregation is shared with backtests and strategy history. Calendar
aggregation and MACD serve the chart. Prices and volumes stay exact decimals;
MACD is floating point.

- N-minute bars gather a trading day's minutes by their start, N minutes of
  the clock at a time (a break shortens the period it falls in). A bar is
  labelled when its period ends; missing or partial trading minutes mark it partial.
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
from itertools import groupby

MINUTES = (1, 5, 10, 15, 30, 60)
CALENDAR = ("day", "week", "month", "quarter", "year")
BEIJING = datetime.timezone(datetime.timedelta(hours=8))


def _sessions(contract):
    import asterion_engine

    exchange, product, _ = contract.split("/")
    return asterion_engine.trading_sessions(exchange, product)


def _observed_periods(rows, minutes, sessions):
    if minutes not in MINUTES:
        raise ValueError(f"minute periods are {', '.join(map(str, MINUTES))}")
    periods = {}
    for start, end in sessions:
        for slot in range(start, end if start < end else end + 1440):
            minute = slot % 1440
            periods.setdefault(minute // minutes, set()).add(minute % minutes)
    full = set(range(minutes))
    span = minutes * 60_000
    openings = [start for start, _ in sessions if start == sessions[0][0] or start >= 18 * 60]
    def until_open(start):
        minute = (start // 60_000 + 8 * 60) % 1440
        return next((opening - minute for opening in openings if opening - 5 <= minute < opening), 0)
    def period(row):
        return row[0], (row[1] + until_open(row[1]) * 60_000) // span
    # A zero-volume auction row carries a quote that never traded.
    traded = (row for row in rows if row[7] or not until_open(row[1]))
    for (day, bucket), group in groupby(traded, period):
        end_ms = (bucket + 1) * span
        observed = set()
        bar = None
        partial = False
        for _, start, source_partial, open_, high, low, close, volume, *interest in group:
            if not until_open(start):
                observed.add(start // 60_000 % minutes)
            last_interest = interest[0] if interest else None
            partial = partial or bool(source_partial)
            if None in (open_, high, low, close):
                partial = partial or volume != 0
                continue
            if bar is None:
                bar = {"trading_day": day, "time_ms": end_ms,
                       "open": open_, "high": high, "low": low, "volume": Decimal(0)}
            bar["high"], bar["low"] = max(bar["high"], high), min(bar["low"], low)
            bar["close"], bar["volume"] = close, bar["volume"] + (volume or 0)
        local_start = (bucket * minutes + 8 * 60) % 1440
        expected = periods.get(local_start // minutes) if periods else full
        partial = partial or expected is None or observed != expected
        if bar is not None:
            bar["open_interest"] = last_interest
            bar["partial"] = partial
        yield end_ms, partial, bar


def minute_bars(rows, minutes: int, contract: str) -> list:
    """Aggregate priced periods from rows of (day, start_ms, partial, OHLC, volume[, OI])."""
    return [bar for _, _, bar in _observed_periods(rows, minutes, _sessions(contract)) if bar is not None]


def minute_coverage(day: str, rows: list, minutes: int, contract: str, calendar: dict,
                    listed: datetime.date | None = None):
    """Priced bars and coverage of one trading day; period ends are actual timestamps."""
    sessions = _sessions(contract)
    observed, partial, bars = set(), [], []
    for end_ms, incomplete, bar in _observed_periods(rows, minutes, sessions):
        observed.add(end_ms)
        if incomplete:
            partial.append(end_ms)
        if bar is not None:
            bars.append(bar)

    date = datetime.date.fromisoformat(day)
    previous = date - datetime.timedelta(days=3 if date.weekday() == 0 else 1)
    expected = set()
    unchecked = not sessions
    for start, end in sessions:
        night = start >= 18 * 60
        midnight = datetime.datetime.combine(previous if night else date, datetime.time(), BEIJING)
        base = int(midnight.timestamp()) * 1000
        stop = end if start < end else 1440 + end
        windows = {base + (bucket + 1) * minutes * 60_000
                   for bucket in range(start // minutes, (stop - 1) // minutes + 1)}
        if night and windows.isdisjoint(observed):
            prior_open = calendar.get(previous.isoformat())
            if prior_open is False:
                continue
            if date == listed or prior_open is None:
                unchecked = True
                continue
        expected.update(windows)
    return bars, dict(missing=sorted(expected - observed), partial=partial,
                      zero_volume_minutes=sum(row[7] == 0 for row in rows), unchecked=unchecked)


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
