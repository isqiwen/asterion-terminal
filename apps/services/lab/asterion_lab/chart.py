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
import functools
from itertools import groupby

import numpy as np
import polars as pl

MINUTES = (1, 5, 10, 15, 30, 60)
CALENDAR = ("day", "week", "month", "quarter", "year")
BEIJING = datetime.timezone(datetime.timedelta(hours=8))
_EPOCH_ORDINAL = 719163
_BEIJING_OFFSET_MS = 8 * 3_600_000


def _midnight_ms(day: datetime.date) -> int:
    return (day.toordinal() - _EPOCH_ORDINAL) * 86_400_000 - _BEIJING_OFFSET_MS


@functools.lru_cache(maxsize=512)
def _schedule(exchange: str, product: str, minutes: int):
    import asterion_engine

    if minutes not in MINUTES:
        raise ValueError(f"minute periods are {', '.join(map(str, MINUTES))}")
    sessions = tuple(asterion_engine.trading_sessions(exchange, product))
    periods: dict[int, int] = {}
    for start, end in sessions:
        for slot in range(start, end if start < end else end + 1440):
            minute = slot % 1440
            bucket = minute // minutes
            periods[bucket] = periods.get(bucket, 0) | (1 << (minute % minutes))
    full = (1 << minutes) - 1
    openings = tuple(start for start, _ in sessions if start == sessions[0][0] or start >= 18 * 60)
    until_open = tuple(
        next((opening - m for opening in openings if opening - 5 <= m < opening), 0)
        for m in range(1440)
    )
    session_windows = []
    for start, end in sessions:
        night = start >= 18 * 60
        stop = end if start < end else 1440 + end
        offsets = tuple(
            (bucket + 1) * minutes * 60_000
            for bucket in range(start // minutes, (stop - 1) // minutes + 1)
        )
        session_windows.append((night, offsets))
    return sessions, periods, full, until_open, tuple(session_windows)


def _sessions(contract):
    exchange, product, _ = contract.split("/")
    return list(_schedule(exchange, product, 1)[0])


def _observed_periods(rows, minutes, sessions):
    if minutes not in MINUTES:
        raise ValueError(f"minute periods are {', '.join(map(str, MINUTES))}")
    periods: dict[int, int] = {}
    for start, end in sessions:
        for slot in range(start, end if start < end else end + 1440):
            minute = slot % 1440
            bucket = minute // minutes
            periods[bucket] = periods.get(bucket, 0) | (1 << (minute % minutes))
    full = (1 << minutes) - 1
    span = minutes * 60_000
    openings = tuple(start for start, _ in sessions if start == sessions[0][0] or start >= 18 * 60)
    until_open = tuple(
        next((opening - m for opening in openings if opening - 5 <= m < opening), 0)
        for m in range(1440)
    )

    def period(row):
        start = row[1]
        u = until_open[(start // 60_000 + 480) % 1440]
        return row[0], (start + u * 60_000) // span

    # A zero-volume auction row carries a quote that never traded.
    traded = (row for row in rows if row[7] or not until_open[(row[1] // 60_000 + 480) % 1440])
    for (day, bucket), group in groupby(traded, period):
        end_ms = (bucket + 1) * span
        observed = 0
        bar = None
        partial = False
        for _, start, source_partial, open_, high, low, close, volume, *interest in group:
            start_min = start // 60_000
            if not until_open[(start_min + 480) % 1440]:
                observed |= 1 << (start_min % minutes)
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
        local_start = (bucket * minutes + 480) % 1440
        expected = periods.get(local_start // minutes) if periods else full
        partial = partial or expected is None or observed != expected
        if bar is not None:
            bar["open_interest"] = last_interest
            bar["partial"] = partial
        yield end_ms, partial, bar


def minute_bars(rows, minutes: int, contract: str) -> list:
    """Aggregate priced periods from rows of (day, start_ms, partial, OHLC, volume[, OI])."""
    exchange, product, _ = contract.split("/")
    sessions = _schedule(exchange, product, minutes)[0]
    return [bar for _, _, bar in _observed_periods(rows, minutes, sessions) if bar is not None]


def _missing_windows(
    day: str, observed: set[int], session_windows: tuple, calendar: dict, listed: datetime.date | None
):
    date = datetime.date.fromisoformat(day)
    previous = date - datetime.timedelta(days=3 if date.weekday() == 0 else 1)
    day_base = _midnight_ms(date)
    prev_base = _midnight_ms(previous)
    expected = set()
    unchecked = not session_windows
    prior_open = None
    prior_checked = False
    for night, offsets in session_windows:
        base = prev_base if night else day_base
        windows = {base + offset for offset in offsets}
        if night and windows.isdisjoint(observed):
            if not prior_checked:
                prior_open = calendar.get(previous.isoformat())
                prior_checked = True
            if prior_open is False:
                continue
            if date == listed or prior_open is None:
                unchecked = True
                continue
        expected.update(windows)
    return sorted(expected - observed), unchecked


def minute_coverage(day: str, rows: list, minutes: int, contract: str, calendar: dict,
                    listed: datetime.date | None = None):
    """Priced bars and coverage of one trading day; period ends are actual timestamps."""
    exchange, product, _ = contract.split("/")
    sessions, _, _, _, session_windows = _schedule(exchange, product, minutes)
    observed, partial, bars = set(), [], []
    for end_ms, incomplete, bar in _observed_periods(rows, minutes, sessions):
        observed.add(end_ms)
        if incomplete:
            partial.append(end_ms)
        if bar is not None:
            bars.append(bar)
    missing, unchecked = _missing_windows(day, observed, session_windows, calendar, listed)
    return bars, dict(missing=missing, partial=partial,
                      zero_volume_minutes=sum(row[7] == 0 for row in rows), unchecked=unchecked)


def _aggregate_raw_frame(frame: pl.DataFrame, day_list: list[str], minutes: int, contract: str):
    import asterion_engine

    exchange, product, _ = contract.split("/")
    sessions, _, _, _, session_windows = _schedule(exchange, product, minutes)
    if frame.is_empty():
        empty_i32 = np.empty(0, dtype=np.int32)
        empty_i64 = np.empty(0, dtype=np.int64)
        empty_f64 = np.empty(0, dtype=np.float64)
        empty_u8 = np.empty(0, dtype=np.uint8)
        return (
            (empty_i32, empty_i64, empty_i64, empty_i64, empty_i64, empty_i64, empty_i64,
             empty_f64, empty_u8, empty_i32, empty_i64, empty_u8,
             np.zeros(len(day_list), dtype=np.int32)),
            session_windows,
        )
    day_to_id = {d: i for i, d in enumerate(day_list)}
    day_ids = frame["day"].replace_strict(day_to_id, return_dtype=pl.Int32).to_numpy()
    start_ms = frame["start_ms"].to_numpy()
    src_partial = frame["partial"].fill_null(False).cast(pl.UInt8).to_numpy()
    ohlc_valid = (
        frame.select(
            (
                pl.col("open").is_not_null()
                & pl.col("high").is_not_null()
                & pl.col("low").is_not_null()
                & pl.col("close").is_not_null()
            ).cast(pl.UInt8)
        )
        .to_series()
        .to_numpy()
    )
    open_raw = frame["open"].fill_null(0).to_numpy()
    high_raw = frame["high"].fill_null(0).to_numpy()
    low_raw = frame["low"].fill_null(0).to_numpy()
    close_raw = frame["close"].fill_null(0).to_numpy()
    vol_valid = frame["volume"].is_not_null().cast(pl.UInt8).to_numpy()
    vol_raw = frame["volume"].fill_null(0).to_numpy()
    oi_f64 = frame["open_interest"].fill_null(float("nan")).to_numpy()
    result = asterion_engine.aggregate_minutes(
        day_ids,
        start_ms,
        src_partial,
        ohlc_valid,
        open_raw,
        high_raw,
        low_raw,
        close_raw,
        vol_valid,
        vol_raw,
        oi_f64,
        minutes,
        list(sessions),
        len(day_list),
    )
    return result, session_windows


def minute_bars_raw(frame: pl.DataFrame, minutes: int, contract: str):
    """Fast C++ minute aggregation over scaled raw-int64 Polars DataFrame."""
    if frame.is_empty():
        return [], np.empty(0, dtype=np.int64), np.empty(0, dtype=np.int64), np.empty(0, dtype=np.uint8)
    day_list = frame["day"].unique(maintain_order=True).to_list()
    (
        bar_day,
        bar_end_ms,
        _b_open,
        _b_high,
        _b_low,
        bar_close,
        _b_vol,
        _b_oi,
        bar_partial,
        *_rest,
    ), _ = _aggregate_raw_frame(frame, day_list, minutes, contract)
    return [day_list[i] for i in bar_day.tolist()], bar_end_ms, bar_close, bar_partial


def minute_coverage_raw(
    frame: pl.DataFrame,
    settled_days: list[str],
    minutes: int,
    contract: str,
    calendar: dict,
    listed: datetime.date | None = None,
):
    """Fast C++ minute aggregation + coverage across all settled trading days in one pass."""
    if not frame.is_empty():
        settled_set = set(settled_days)
        frame = frame.filter(pl.col("day").is_in(list(settled_set)))
    day_list = frame["day"].unique(maintain_order=True).to_list() if not frame.is_empty() else []
    (
        bar_day,
        bar_end_ms,
        bar_open,
        bar_high,
        bar_low,
        bar_close,
        bar_vol,
        bar_oi,
        bar_partial,
        obs_day,
        obs_end_ms,
        obs_partial,
        zero_vol_by_day,
    ), session_windows = _aggregate_raw_frame(frame, day_list, minutes, contract)

    observed_by_day: list[set[int]] = [set() for _ in day_list]
    partial_count_by_day = [0] * len(day_list)
    for d_idx, end_ms, is_part in zip(obs_day.tolist(), obs_end_ms.tolist(), obs_partial.tolist()):
        observed_by_day[d_idx].add(end_ms)
        if is_part:
            partial_count_by_day[d_idx] += 1

    excluded = []
    coverage = dict(contract=contract, missing={}, zero_volume_minutes=0, unchecked_days=[])
    for d_idx, day in enumerate(day_list):
        if partial_count_by_day[d_idx]:
            excluded.extend([day] * partial_count_by_day[d_idx])
        missing, unchecked = _missing_windows(day, observed_by_day[d_idx], session_windows, calendar, listed)
        if missing:
            coverage["missing"][day] = len(missing)
        coverage["zero_volume_minutes"] += int(zero_vol_by_day[d_idx])
        if unchecked:
            coverage["unchecked_days"].append(day)

    stored_days = {day_list[i] for i in bar_day.tolist()}
    bars = [
        (
            day_list[d_idx],
            end_ms * 10**6,
            o,
            h,
            lo,
            c,
            v,
            oi,
        )
        for d_idx, end_ms, o, h, lo, c, v, oi, is_part in zip(
            bar_day.tolist(),
            bar_end_ms.tolist(),
            bar_open.tolist(),
            bar_high.tolist(),
            bar_low.tolist(),
            bar_close.tolist(),
            bar_vol.tolist(),
            bar_oi.tolist(),
            bar_partial.tolist(),
        )
        if not is_part
    ]
    return bars, stored_days, excluded, coverage


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
