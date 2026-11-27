"""Point-in-time (PIT) alignment for exchange fundamental and member-position tables.

Exchange post-close reports (warehouse_receipts, member_positions, inventory)
dated `report_day` are compiled after the 15:00 Beijing close. A report becomes
visible only on bars completing strictly after 15:00 on `report_day` AND at or
after `published_ms` when recorded, preventing same-day close lookahead.
"""
from bisect import bisect_right
import datetime
import functools
import math

_BEIJING = datetime.timezone(datetime.timedelta(hours=8))
_SUMMARY_TOKENS = ("合计", "总计", "小计")
_DAILY_MAX_AGE_DAYS = 14
_WEEKLY_MAX_AGE_DAYS = 21


@functools.lru_cache(maxsize=8192)
def _day_ordinal(day: str) -> int:
    return datetime.date.fromisoformat(day).toordinal()


def _close_ms(day: datetime.date) -> int:
    return int(datetime.datetime.combine(day, datetime.time(15), _BEIJING).timestamp()) * 1000


def _available_ms(day: datetime.date, published_ms) -> int:
    post_close = _close_ms(day) + 1
    if published_ms is None:
        return post_close
    return max(int(published_ms), post_close)


def _is_summary_label(*labels) -> bool:
    for label in labels:
        if label and any(token in label for token in _SUMMARY_TOKENS):
            return True
    return False


def _aggregate_detail_or_summary(rows: list, value_idx: int, label_indices: tuple) -> float | None:
    """Sum detail rows when present; fall back to summary rows only when a report publishes summaries alone."""
    details = []
    summaries = []
    for row in rows:
        value = row[value_idx]
        if value is None:
            continue
        numeric = float(value)
        if not math.isfinite(numeric):
            continue
        if _is_summary_label(*(row[i] for i in label_indices)):
            summaries.append(numeric)
        else:
            details.append(numeric)
    chosen = details if details else summaries
    return float(sum(chosen)) if chosen else None


def _build_timeline(records: dict) -> dict:
    """Turn `{product: [(available_ms, day_ord, payload), ...]}` into bisect-ready timelines."""
    out = {}
    for product, items in records.items():
        items.sort(key=lambda item: (item[0], item[1]))
        # Keep the latest report_day if multiple reports share an identical available_ms.
        deduped = []
        for avail, day_ord, payload in items:
            if deduped and deduped[-1][0] == avail:
                deduped[-1] = (avail, day_ord, payload)
            else:
                deduped.append((avail, day_ord, payload))
        out[product] = (
            [item[0] for item in deduped],
            [item[1] for item in deduped],
            [item[2] for item in deduped],
        )
    return out


class ExchangePit:
    """PIT fundamental timelines for one exchange over a date window."""

    def __init__(self, receipts: dict, members: dict, inventory: dict):
        self._receipts = receipts
        self._members = members
        self._inventory = inventory

    def empty(self) -> bool:
        return not (self._receipts or self._members or self._inventory)

    def lookup(self, product: str, bar_day: str, bar_end_ms: int) -> tuple[float, float, float, float]:
        """Return `(receipt_volume, member_long, member_short, inventory_total)` visible at `(bar_day, bar_end_ms)`."""
        if self.empty():
            return math.nan, math.nan, math.nan, math.nan
        day_ord = _day_ordinal(bar_day)
        receipt = self._latest(self._receipts, product, day_ord, bar_end_ms, _DAILY_MAX_AGE_DAYS)
        member = self._latest(self._members, product, day_ord, bar_end_ms, _DAILY_MAX_AGE_DAYS)
        inv = self._latest(self._inventory, product, day_ord, bar_end_ms, _WEEKLY_MAX_AGE_DAYS)
        m_long, m_short = member if member is not None else (math.nan, math.nan)
        return (
            receipt if receipt is not None else math.nan,
            m_long,
            m_short,
            inv if inv is not None else math.nan,
        )

    @staticmethod
    def _latest(table: dict, product: str, day_ord: int, bar_end_ms: int, max_age_days: int):
        timeline = table.get(product)
        if timeline is None:
            return None
        times, day_ords, payloads = timeline
        idx = bisect_right(times, bar_end_ms) - 1
        if idx < 0:
            return None
        if day_ord - day_ords[idx] > max_age_days:
            return None
        return payloads[idx]


EMPTY_PIT = ExchangePit({}, {}, {})


def load_exchange_pit(warehouse, version, exchange: str, begin: str, end: str) -> ExchangePit:
    """Load PIT fundamentals for `exchange` over `[begin - 30d, end]`.

    Skips querying any table absent from the pinned version so price-only
    datasets incur zero extra Parquet scans.
    """
    first = datetime.date.fromisoformat(begin)
    lookback = (first - min(datetime.timedelta(days=30), first - datetime.date.min)).isoformat()

    receipts_raw: dict[str, list] = {}
    if list(version.keys("warehouse_receipts", exchange=exchange, begin=lookback, end=end)):
        rows = warehouse.query(
            version,
            """select product, day, warehouse, area, volume, published_ms
               from warehouse_receipts
               where exchange = ? and day between ?::date and ?::date""",
            ("warehouse_receipts",),
            (exchange, lookback, end),
            exchange=exchange,
            begin=lookback,
            end=end,
        )
        grouped: dict[tuple, list] = {}
        for row in rows:
            grouped.setdefault((row[0], row[1]), []).append(row)
        for (product, day), group in grouped.items():
            total = _aggregate_detail_or_summary(group, 4, (2, 3))
            if total is None:
                continue
            pubs = [r[5] for r in group if r[5] is not None]
            avail = _available_ms(day, max(pubs) if pubs else None)
            receipts_raw.setdefault(product, []).append((avail, day.toordinal(), total))

    members_raw: dict[str, list] = {}
    if list(version.keys("member_positions", exchange=exchange, begin=lookback, end=end)):
        rows = warehouse.query(
            version,
            """select product, contract, day, member, long_position, short_position, published_ms
               from member_positions
               where exchange = ? and day between ?::date and ?::date""",
            ("member_positions",),
            (exchange, lookback, end),
            exchange=exchange,
            begin=lookback,
            end=end,
        )
        grouped = {}
        for row in rows:
            if not row[3] or _is_summary_label(row[3]):
                continue
            grouped.setdefault((row[0], row[2]), []).append(row)
        for (product, day), group in grouped.items():
            product_rows = [r for r in group if not r[1]]
            chosen = product_rows if product_rows else group
            longs = [float(r[4]) for r in chosen if r[4] is not None and math.isfinite(float(r[4]))]
            shorts = [float(r[5]) for r in chosen if r[5] is not None and math.isfinite(float(r[5]))]
            if not longs and not shorts:
                continue
            pubs = [r[6] for r in chosen if r[6] is not None]
            avail = _available_ms(day, max(pubs) if pubs else None)
            members_raw.setdefault(product, []).append((
                avail,
                day.toordinal(),
                (float(sum(longs)) if longs else math.nan, float(sum(shorts)) if shorts else math.nan),
            ))

    inventory_raw: dict[str, list] = {}
    if list(version.keys("inventory", exchange=exchange, begin=lookback, end=end)):
        rows = warehouse.query(
            version,
            """select product, day, area, warehouse, total, published_ms
               from inventory
               where exchange = ? and day between ?::date and ?::date""",
            ("inventory",),
            (exchange, lookback, end),
            exchange=exchange,
            begin=lookback,
            end=end,
        )
        grouped = {}
        for row in rows:
            grouped.setdefault((row[0], row[1]), []).append(row)
        for (product, day), group in grouped.items():
            total = _aggregate_detail_or_summary(group, 4, (2, 3))
            if total is None:
                continue
            pubs = [r[5] for r in group if r[5] is not None]
            avail = _available_ms(day, max(pubs) if pubs else None)
            inventory_raw.setdefault(product, []).append((avail, day.toordinal(), total))

    return ExchangePit(
        _build_timeline(receipts_raw),
        _build_timeline(members_raw),
        _build_timeline(inventory_raw),
    )
